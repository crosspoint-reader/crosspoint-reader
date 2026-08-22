use std::error::Error;
use std::fmt;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::os::fd::{AsFd, OwnedFd};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use nix::errno::Errno;
use nix::libc;
use nix::poll::{poll, PollFd, PollFlags, PollTimeout};
use nix::sys::socket::{setsockopt, sockopt};
use serde_json::{json, Value};

use crate::control::{ControlServer, DisplayCommand};
use crate::discovery::{discover_network_devices, DEFAULT_WIFI_PORT};
use crate::handshake::{
    parse_server_accept, protocol_client_name, protocol_host_time, terminal_hello, HandshakeError,
    ServerAccept, HANDSHAKE_LINE_LIMIT,
};
use crate::protocol::{
    decode_diagnostic_refresh_event, decode_diagnostic_response, encode_diagnostic_command,
    encode_frame, is_known_frame_type, is_optional_frame_type, DiagnosticCommand,
    DiagnosticEventPhase, DiagnosticMetrics, DiagnosticRefreshEvent, DiagnosticSessionMetadata,
    DiagnosticStatus, Frame, FrameDecoder, FrameType, ProtocolError, MAX_FRAME_PAYLOAD,
};
use crate::pty::{exit_status_code, PtySession};
use crate::signals::ShutdownSignals;
use crate::transport::{SecurityMode, TerminalStream};

pub const DEFAULT_CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
pub const DEFAULT_RETRY_INTERVAL: Duration = Duration::from_secs(1);
pub const DEFAULT_APPROVAL_TIMEOUT: Duration = Duration::from_secs(60);
pub const DEFAULT_MAX_BPS: usize = 262_144;
pub const DEFAULT_CAPTURE_LIMIT: usize = 8 * 1024 * 1024;
pub const DENIED_RETRY_INTERVAL: Duration = Duration::from_secs(300);
const EVENT_LOOP_INTERVAL: Duration = Duration::from_millis(100);
const PTY_BURST_QUIET: Duration = Duration::from_millis(24);
const NETWORK_READ_SIZE: usize = 2048;
const RAW_PTY_READ_SIZE: usize = 1024;
const LOCAL_INPUT_READ_SIZE: usize = 256;
const MAX_PENDING_INPUT: usize = 4096;
const MAX_PENDING_NETWORK_OUTPUT: usize = 2 * (MAX_FRAME_PAYLOAD + 8);
const LOCAL_EXIT_BYTE: u8 = 0x1c;
const DISPLAY_CONTROL_TIMEOUT: Duration = Duration::from_secs(20);
const DEFERRED_CLEAN_QUIET: Duration = Duration::from_millis(500);
const TCP_KEEPALIVE_IDLE_SECONDS: u32 = 3;
const TCP_KEEPALIVE_INTERVAL_SECONDS: u32 = 1;
const TCP_KEEPALIVE_PROBES: u32 = 3;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProtocolPreference {
    Auto,
    V3,
    V2,
    V1,
}

impl ProtocolPreference {
    fn versions(self) -> &'static [u8] {
        match self {
            Self::Auto => &[3, 2, 1],
            Self::V3 => &[3],
            Self::V2 => &[2],
            Self::V1 => &[1],
        }
    }
}

#[derive(Clone, Debug)]
pub struct BridgeConfig {
    pub host: String,
    pub port: u16,
    pub retry_interval: Duration,
    pub discovery_timeout: Duration,
    pub approval_timeout: Duration,
    pub max_bps: usize,
    pub capture_output: Option<PathBuf>,
    pub capture_limit: usize,
    pub reconnect: bool,
    pub protocol: ProtocolPreference,
    pub verbose: bool,
    pub security: SecurityMode,
}

impl Default for BridgeConfig {
    fn default() -> Self {
        Self {
            host: "auto".to_owned(),
            port: DEFAULT_WIFI_PORT,
            retry_interval: DEFAULT_RETRY_INTERVAL,
            discovery_timeout: crate::discovery::DEFAULT_DISCOVERY_TIMEOUT,
            approval_timeout: DEFAULT_APPROVAL_TIMEOUT,
            max_bps: DEFAULT_MAX_BPS,
            capture_output: None,
            capture_limit: DEFAULT_CAPTURE_LIMIT,
            reconnect: false,
            protocol: ProtocolPreference::Auto,
            verbose: false,
            security: SecurityMode::Tls,
        }
    }
}

#[derive(Debug)]
pub enum BridgeError {
    Io(io::Error),
    Handshake(HandshakeError),
    Protocol(ProtocolError),
    NoDevice,
    MultipleDevices(String),
    NoResolvedAddress(String),
    VersionMismatch { requested: u8, accepted: u8 },
    PendingInputOverflow,
    CaptureIo(io::Error),
    CaptureLimitExceeded(usize),
    ControlProtocol(String),
    SessionEnded,
}

impl fmt::Display for BridgeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => error.fmt(formatter),
            Self::Handshake(error) => error.fmt(formatter),
            Self::Protocol(error) => error.fmt(formatter),
            Self::NoDevice => formatter.write_str("no knietty terminal found on the local network"),
            Self::MultipleDevices(devices) => write!(
                formatter,
                "multiple knietty terminals found ({devices}); pass --host"
            ),
            Self::NoResolvedAddress(host) => {
                write!(formatter, "could not resolve a TCP address for {host:?}")
            }
            Self::VersionMismatch {
                requested,
                accepted,
            } => write!(
                formatter,
                "X4 accepted protocol v{accepted} after a v{requested} request"
            ),
            Self::PendingInputOverflow => {
                formatter.write_str("X4 input exceeded the bounded host queue")
            }
            Self::CaptureIo(error) => error.fmt(formatter),
            Self::CaptureLimitExceeded(limit) => write!(
                formatter,
                "PTY output capture reached its {limit}-byte limit"
            ),
            Self::ControlProtocol(message) => formatter.write_str(message),
            Self::SessionEnded => formatter.write_str("X4 ended the terminal session"),
        }
    }
}

impl Error for BridgeError {}

impl From<io::Error> for BridgeError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

impl From<HandshakeError> for BridgeError {
    fn from(error: HandshakeError) -> Self {
        Self::Handshake(error)
    }
}

impl From<ProtocolError> for BridgeError {
    fn from(error: ProtocolError) -> Self {
        Self::Protocol(error)
    }
}

#[derive(Debug)]
enum ConnectError {
    Bridge(BridgeError),
    Denied,
    Interrupted(i32),
}

impl From<BridgeError> for ConnectError {
    fn from(error: BridgeError) -> Self {
        Self::Bridge(error)
    }
}

#[derive(Debug)]
struct ConnectedTerminal {
    stream: TerminalStream,
    accepted: ServerAccept,
    label: String,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct OutputMetrics {
    pty_bytes: u64,
    pty_reads: u64,
    output_frames: u64,
    burst_ends: u64,
}

#[derive(Debug)]
struct PendingDisplayControl {
    command: DisplayCommand,
    diagnostic_command: DiagnosticCommand,
    sequence: u32,
    accepted: bool,
    presented: bool,
    local_response: bool,
    deadline: Instant,
}

fn profile_name(profile: u8) -> &'static str {
    match profile {
        0 => "panel-default",
        1 => "terminal-interactive",
        2 => "terminal-settle",
        _ => "unknown",
    }
}

fn font_name(font: u8) -> &'static str {
    match font {
        1 => "terminus",
        2 => "unifont",
        3 => "fallback",
        _ => "unknown",
    }
}

fn refresh_path_name(path: u8) -> &'static str {
    match path {
        0 => "none",
        1 => "window-fast",
        2 => "fallback-fast",
        3 => "half",
        _ => "unknown",
    }
}

fn status_result(device: &str, metadata: &DiagnosticSessionMetadata) -> Value {
    json!({
        "command": "status",
        "device": device,
        "profile": profile_name(metadata.profile),
        "profile_id": metadata.profile,
        "spi_mhz": metadata.spi_mhz,
        "inverted": metadata.inverted(),
        "fading_fix": metadata.fading_fix(),
        "adaptive_refresh": metadata.adaptive_refresh(),
        "overclocked_spi": metadata.overclocked_spi(),
        "waveform_100ms": metadata.waveform_100ms(),
        "auto_settle": metadata.auto_settle(),
        "balanced_sustain": metadata.balanced_sustain(),
        "ram_ping_pong": metadata.ram_ping_pong(),
        "orientation": metadata.orientation,
        "board": metadata.board,
        "controller": metadata.controller,
        "battery_percent": metadata.battery_percent,
        "rssi_dbm": metadata.rssi_dbm,
        "columns": metadata.columns,
        "rows": metadata.rows,
        "font": font_name(metadata.font),
        "font_id": metadata.font,
        "display_width": metadata.display_width,
        "display_height": metadata.display_height,
        "free_heap": metadata.free_heap,
        "minimum_free_heap": metadata.minimum_free_heap,
        "build": metadata.build,
        "freeink": metadata.freeink,
    })
}

fn metrics_result(
    device: &str,
    metrics: &DiagnosticMetrics,
    host: OutputMetrics,
    burst_boundaries_enabled: bool,
) -> Value {
    json!({
        "command": "metrics",
        "device": device,
        "updates": metrics.updates,
        "window": metrics.windowed,
        "fallback": metrics.fallback,
        "settle": metrics.settle,
        "clean": metrics.clean,
        "last_total_us": metrics.last_total_us,
        "last_waveform_us": metrics.last_waveform_us,
        "last_queue_us": metrics.last_queue_us,
        "last_render_us": metrics.last_render_us,
        "last_transfer_us": metrics.last_transfer_us,
        "last_plane_us": metrics.last_plane_us,
        "last_lut_us": metrics.last_lut_us,
        "last_baseline_us": metrics.last_baseline_us,
        "average_total_us": metrics.average_total_us,
        "minimum_total_us": metrics.minimum_total_us,
        "maximum_total_us": metrics.maximum_total_us,
        "last_region": {
            "width": metrics.last_region_width,
            "height": metrics.last_region_height,
            "bytes": metrics.last_region_bytes,
        },
        "free_heap": metrics.free_heap,
        "minimum_free_heap": metrics.minimum_free_heap,
        "device_pipeline": {
            "rx_bytes": metrics.rx_bytes,
            "rx_reads": metrics.rx_reads,
            "burst_ends": metrics.burst_ends,
            "burst_snapshots": metrics.burst_snapshots,
            "burst_timeouts": metrics.burst_timeouts,
            "async_tail_updates": metrics.async_tail_updates,
        },
        "host_pipeline": {
            "burst1": burst_boundaries_enabled,
            "pty_bytes": host.pty_bytes,
            "pty_reads": host.pty_reads,
            "output_frames": host.output_frames,
            "burst_ends": host.burst_ends,
            "quiet_ms": PTY_BURST_QUIET.as_millis(),
        },
    })
}

fn refresh_result(command: DisplayCommand, event: &DiagnosticRefreshEvent) -> Value {
    json!({
        "command": command.as_str(),
        "phase": "ready",
        "actual_path": refresh_path_name(event.actual_path),
        "actual_path_id": event.actual_path,
        "fallback_reason": event.fallback_reason,
        "inverted": event.flags & DiagnosticSessionMetadata::FLAG_INVERTED != 0,
        "used_window": event.flags & 0x02 != 0,
        "fading_fix": event.flags & 0x04 != 0,
        "queue_us": event.queue_us,
        "render_us": event.render_us,
        "transfer_us": event.transfer_us,
        "waveform_us": event.waveform_us,
        "baseline_us": event.baseline_us,
        "total_us": event.total_us,
        "logical_region": {
            "x": event.logical_x,
            "y": event.logical_y,
            "width": event.logical_width,
            "height": event.logical_height,
        },
        "aligned_region": {
            "x": event.aligned_x,
            "y": event.aligned_y,
            "width": event.aligned_width,
            "height": event.aligned_height,
        },
        "transfer_bytes": event.transfer_bytes,
        "free_heap": event.free_heap,
        "minimum_free_heap": event.minimum_free_heap,
    })
}

#[derive(Debug)]
struct PtyCapture {
    file: File,
    limit: usize,
    written: usize,
}

impl PtyCapture {
    fn create(path: &Path, limit: usize) -> io::Result<Self> {
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(path)
            .map_err(|error| {
                io::Error::new(
                    error.kind(),
                    format!(
                        "could not create private PTY capture {}: {error}",
                        path.display()
                    ),
                )
            })?;
        Ok(Self {
            file,
            limit,
            written: 0,
        })
    }

    fn record(&mut self, bytes: &[u8]) -> Result<(), BridgeError> {
        if bytes.len() > self.limit.saturating_sub(self.written) {
            return Err(BridgeError::CaptureLimitExceeded(self.limit));
        }
        self.file.write_all(bytes).map_err(|error| {
            BridgeError::CaptureIo(io::Error::new(
                error.kind(),
                format!("could not write PTY output capture: {error}"),
            ))
        })?;
        self.written += bytes.len();
        Ok(())
    }
}

fn errno_to_io(error: Errno) -> io::Error {
    io::Error::from_raw_os_error(error as i32)
}

fn configure_tcp_keepalive(stream: &TcpStream) -> Result<(), BridgeError> {
    setsockopt(stream, sockopt::KeepAlive, &true).map_err(errno_to_io)?;
    #[cfg(target_os = "macos")]
    setsockopt(stream, sockopt::TcpKeepAlive, &TCP_KEEPALIVE_IDLE_SECONDS).map_err(errno_to_io)?;
    #[cfg(target_os = "linux")]
    setsockopt(stream, sockopt::TcpKeepIdle, &TCP_KEEPALIVE_IDLE_SECONDS).map_err(errno_to_io)?;
    setsockopt(
        stream,
        sockopt::TcpKeepInterval,
        &TCP_KEEPALIVE_INTERVAL_SECONDS,
    )
    .map_err(errno_to_io)?;
    setsockopt(stream, sockopt::TcpKeepCount, &TCP_KEEPALIVE_PROBES).map_err(errno_to_io)?;
    Ok(())
}

fn poll_timeout(duration: Duration) -> PollTimeout {
    let milliseconds = duration.as_millis().min(u16::MAX as u128) as u16;
    PollTimeout::from(milliseconds)
}

fn resolve_explicit(host: &str, port: u16) -> Result<Vec<SocketAddr>, BridgeError> {
    let addresses: Vec<_> = (host, port).to_socket_addrs()?.collect();
    if addresses.is_empty() {
        Err(BridgeError::NoResolvedAddress(host.to_owned()))
    } else {
        Ok(addresses)
    }
}

fn connect_addresses(
    addresses: &[SocketAddr],
    timeout: Duration,
) -> io::Result<(TcpStream, SocketAddr)> {
    let mut last_error = None;
    for address in addresses {
        match TcpStream::connect_timeout(address, timeout) {
            Ok(stream) => return Ok((stream, *address)),
            Err(error) => last_error = Some(error),
        }
    }
    Err(last_error.unwrap_or_else(|| {
        io::Error::new(io::ErrorKind::AddrNotAvailable, "target has no TCP address")
    }))
}

fn read_handshake_line(
    stream: &mut TerminalStream,
    timeout: Duration,
    signals: &ShutdownSignals,
) -> Result<Vec<u8>, ConnectError> {
    stream.set_nonblocking(true).map_err(BridgeError::Io)?;
    let deadline = Instant::now() + timeout;
    let mut line = Vec::with_capacity(HANDSHAKE_LINE_LIMIT);
    let mut chunk = [0_u8; 1];
    loop {
        if let Some(signal) = signals.received() {
            return Err(ConnectError::Interrupted(signal));
        }
        if line.len() >= HANDSHAKE_LINE_LIMIT {
            return Err(BridgeError::Handshake(HandshakeError::LineTooLong).into());
        }
        match stream.read(&mut chunk) {
            Ok(0) => return Err(BridgeError::Handshake(HandshakeError::Disconnected).into()),
            Ok(_) => {
                line.push(chunk[0]);
                if chunk[0] == b'\n' {
                    return Ok(line);
                }
                continue;
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
            Err(error) => return Err(BridgeError::Io(error).into()),
        }

        let now = Instant::now();
        if now >= deadline {
            return Err(BridgeError::Io(io::Error::new(
                io::ErrorKind::TimedOut,
                "timed out waiting for approval on the X4",
            ))
            .into());
        }
        let wait = deadline
            .saturating_duration_since(now)
            .min(EVENT_LOOP_INTERVAL);
        let mut descriptors = [PollFd::new(stream.as_fd(), PollFlags::POLLIN)];
        match poll(&mut descriptors, poll_timeout(wait)) {
            Ok(_) | Err(Errno::EINTR) => {}
            Err(error) => return Err(BridgeError::Io(errno_to_io(error)).into()),
        }
    }
}

fn append_bounded_input(target: &mut Vec<u8>, input: &[u8]) -> Result<(), BridgeError> {
    if target.len().saturating_add(input.len()) > MAX_PENDING_INPUT {
        return Err(BridgeError::PendingInputOverflow);
    }
    target.extend_from_slice(input);
    Ok(())
}

fn append_local_input(target: &mut Vec<u8>, input: &[u8]) -> Result<bool, BridgeError> {
    if let Some(exit_at) = input.iter().position(|byte| *byte == LOCAL_EXIT_BYTE) {
        append_bounded_input(target, &input[..exit_at])?;
        Ok(true)
    } else {
        append_bounded_input(target, input)?;
        Ok(false)
    }
}

pub struct NetworkBridge<'a> {
    session: &'a mut PtySession,
    config: BridgeConfig,
    signals: &'a ShutdownSignals,
    local_input: Option<&'a OwnedFd>,
    connection: Option<TerminalStream>,
    protocol_version: u8,
    frame_decoder: FrameDecoder,
    next_tx_sequence: u32,
    pending_output: Vec<u8>,
    pending_input: Vec<u8>,
    capture: Option<PtyCapture>,
    next_write_at: Instant,
    output_burst_open: bool,
    output_burst_due: Option<Instant>,
    burst_boundaries_enabled: bool,
    output_metrics: OutputMetrics,
    connected_once: bool,
    local_exit_requested: bool,
    last_retry_error: String,
    last_retry_log_at: Option<Instant>,
    control_server: Option<ControlServer>,
    pending_control: Option<PendingDisplayControl>,
    ignored_control_sequence: Option<u32>,
    deferred_clean_due: Option<Instant>,
}

impl<'a> NetworkBridge<'a> {
    pub fn new(
        session: &'a mut PtySession,
        config: BridgeConfig,
        signals: &'a ShutdownSignals,
        local_input: Option<&'a OwnedFd>,
    ) -> Result<Self, BridgeError> {
        if config.max_bps == 0 {
            return Err(BridgeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                "maximum output rate must be greater than zero",
            )));
        }
        if config.capture_limit == 0 {
            return Err(BridgeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                "PTY output capture limit must be greater than zero",
            )));
        }
        let capture = config
            .capture_output
            .as_deref()
            .map(|path| PtyCapture::create(path, config.capture_limit))
            .transpose()
            .map_err(BridgeError::CaptureIo)?;
        Ok(Self {
            session,
            config,
            signals,
            local_input,
            connection: None,
            protocol_version: 0,
            frame_decoder: FrameDecoder::new(),
            next_tx_sequence: 1,
            pending_output: Vec::with_capacity(MAX_FRAME_PAYLOAD + 8),
            pending_input: Vec::with_capacity(MAX_PENDING_INPUT),
            capture,
            next_write_at: Instant::now(),
            output_burst_open: false,
            output_burst_due: None,
            burst_boundaries_enabled: false,
            output_metrics: OutputMetrics::default(),
            connected_once: false,
            local_exit_requested: false,
            last_retry_error: String::new(),
            last_retry_log_at: None,
            control_server: None,
            pending_control: None,
            ignored_control_sequence: None,
            deferred_clean_due: None,
        })
    }

    fn log(&self, message: impl fmt::Display) {
        eprintln!("knietty: {message}");
    }

    fn log_retry_error(&mut self, error: &BridgeError) {
        if !self.config.verbose {
            return;
        }
        let message = error.to_string();
        let now = Instant::now();
        let should_log = message != self.last_retry_error
            || match self.last_retry_log_at {
                None => true,
                Some(last) => now.duration_since(last) >= Duration::from_secs(30),
            };
        if should_log {
            self.log(&message);
            self.last_retry_error = message;
            self.last_retry_log_at = Some(now);
        }
    }

    fn fail_local_control(&mut self, message: impl fmt::Display) {
        if let Some(server) = &mut self.control_server {
            server.fail(message);
        }
    }

    fn complete_local_control(&mut self, result: Value) {
        if let Some(server) = &mut self.control_server {
            server.complete(result);
        }
    }

    fn start_display_control(
        &mut self,
        command: DisplayCommand,
        local_response: bool,
    ) -> Result<(), String> {
        let (diagnostic_command, variant) = match command {
            DisplayCommand::Status => (DiagnosticCommand::SessionInfo, None),
            DisplayCommand::Metrics => (DiagnosticCommand::Metrics, None),
            DisplayCommand::Clean => (DiagnosticCommand::Clean, None),
            DisplayCommand::PolarityNormal => (DiagnosticCommand::SetPolarity, Some(0)),
            DisplayCommand::PolarityInverted => (DiagnosticCommand::SetPolarity, Some(1)),
            DisplayCommand::CleanDeferred => {
                return Err("deferred clean must be scheduled before wire encoding".to_owned())
            }
        };
        let payload = encode_diagnostic_command(diagnostic_command, None, variant)
            .map_err(|error| error.to_string())?;
        let sequence = self.next_tx_sequence;
        let frame = encode_frame(FrameType::ControlRequest.as_u8(), &payload, sequence, 0)
            .map_err(|error| error.to_string())?;
        if self.pending_output.len().saturating_add(frame.len()) > MAX_PENDING_NETWORK_OUTPUT {
            return Err("host output queue is busy; retry the display command".to_owned());
        }
        self.pending_output.extend_from_slice(&frame);
        self.next_tx_sequence = self.next_tx_sequence.wrapping_add(1);
        self.pending_control = Some(PendingDisplayControl {
            command,
            diagnostic_command,
            sequence,
            accepted: false,
            presented: false,
            local_response,
            deadline: Instant::now() + DISPLAY_CONTROL_TIMEOUT,
        });
        Ok(())
    }

    fn poll_deferred_clean(&mut self) {
        let Some(due) = self.deferred_clean_due else {
            return;
        };
        if Instant::now() < due
            || self.pending_control.is_some()
            || !self.pending_output.is_empty()
            || self.connection.is_none()
            || self.protocol_version != 3
        {
            return;
        }
        self.deferred_clean_due = None;
        if let Err(error) = self.start_display_control(DisplayCommand::Clean, false) {
            self.log(format_args!("deferred display clean failed: {error}"));
        }
    }

    fn poll_local_control(&mut self) {
        self.poll_deferred_clean();
        if self
            .pending_control
            .as_ref()
            .is_some_and(|pending| Instant::now() >= pending.deadline)
        {
            let pending = self.pending_control.take().expect("expired command exists");
            self.ignored_control_sequence = Some(pending.sequence);
            let message = format!(
                "X4 timed out while executing display {}",
                pending.command.as_str()
            );
            if pending.local_response {
                self.fail_local_control(&message);
            } else {
                self.log(message);
            }
        }

        let request = match self.control_server.as_mut() {
            Some(server) => match server.poll_request() {
                Ok(request) => request,
                Err(error) => {
                    self.log(format_args!("local display-control socket failed: {error}"));
                    self.control_server = None;
                    self.pending_control = None;
                    return;
                }
            },
            None => return,
        };
        let Some(command) = request else {
            return;
        };

        if self.connection.is_none() {
            self.fail_local_control("X4 is not connected");
            return;
        }
        if self.protocol_version != 3 {
            self.fail_local_control("display control requires knietty protocol v3");
            return;
        }
        if self.pending_control.is_some() {
            self.fail_local_control("another display command is still active");
            return;
        }

        if command == DisplayCommand::CleanDeferred {
            self.deferred_clean_due = Some(Instant::now() + DEFERRED_CLEAN_QUIET);
            self.complete_local_control(json!({
                "command": "clean",
                "scheduled": true,
                "quiet_ms": DEFERRED_CLEAN_QUIET.as_millis(),
            }));
            return;
        }
        if let Err(error) = self.start_display_control(command, true) {
            self.fail_local_control(error);
        }
    }

    fn resolve_target(&self) -> Result<(Vec<SocketAddr>, String), BridgeError> {
        if self.config.host != "auto" {
            return Ok((
                resolve_explicit(&self.config.host, self.config.port)?,
                self.config.host.clone(),
            ));
        }
        let devices = discover_network_devices(self.config.discovery_timeout, self.config.port)?;
        match devices.as_slice() {
            [] => Err(BridgeError::NoDevice),
            [device] => Ok((
                vec![SocketAddr::new(device.address, device.port)],
                device.name.clone(),
            )),
            _ => {
                let choices = devices
                    .iter()
                    .map(|device| format!("{} ({})", device.name, device.address))
                    .collect::<Vec<_>>()
                    .join(", ");
                Err(BridgeError::MultipleDevices(choices))
            }
        }
    }

    fn connect_protocol(
        &self,
        addresses: &[SocketAddr],
        label: &str,
        version: u8,
    ) -> Result<ConnectedTerminal, ConnectError> {
        if let Some(signal) = self.signals.received() {
            return Err(ConnectError::Interrupted(signal));
        }
        let (raw_stream, address) =
            connect_addresses(addresses, DEFAULT_CONNECT_TIMEOUT).map_err(BridgeError::Io)?;
        raw_stream.set_nodelay(true).map_err(BridgeError::Io)?;
        configure_tcp_keepalive(&raw_stream)?;
        let mut stream = TerminalStream::connect(
            raw_stream,
            label,
            self.config.security,
            DEFAULT_CONNECT_TIMEOUT,
        )
        .map_err(BridgeError::Io)?;
        if let Some(pairing) = stream.pairing() {
            if pairing.first_pairing {
                self.log(format_args!(
                    "first pairing code {} (verify it matches the X4 before pressing Confirm)",
                    pairing.code
                ));
                if self.config.verbose {
                    self.log(format_args!(
                        "device fingerprint {}; host fingerprint {}",
                        pairing.device_fingerprint, pairing.host_fingerprint
                    ));
                }
            } else if self.config.verbose {
                self.log(format_args!(
                    "authenticated paired X4 {}",
                    pairing.device_fingerprint
                ));
            }
        }
        stream
            .set_write_timeout(Some(self.config.approval_timeout))
            .map_err(BridgeError::Io)?;
        let client_name = protocol_client_name(None);
        let (epoch, offset) = protocol_host_time();
        stream
            .write_all(terminal_hello(version, &client_name, epoch, offset).as_bytes())
            .map_err(BridgeError::Io)?;
        self.log(format_args!(
            "requesting approval on {label} ({}:{})",
            address.ip(),
            address.port()
        ));
        let response =
            read_handshake_line(&mut stream, self.config.approval_timeout, self.signals)?;
        let accepted = match parse_server_accept(&response) {
            Err(HandshakeError::Denied) => return Err(ConnectError::Denied),
            result => result.map_err(BridgeError::Handshake)?,
        };
        if accepted.version != version {
            return Err(BridgeError::VersionMismatch {
                requested: version,
                accepted: accepted.version,
            }
            .into());
        }
        stream.confirm_pairing().map_err(BridgeError::Io)?;
        if version == 3 {
            let pair_commit = encode_frame(FrameType::Heartbeat.as_u8(), b"", 0, 0)
                .map_err(BridgeError::Protocol)?;
            stream.write_all(&pair_commit).map_err(BridgeError::Io)?;
        }
        stream.set_write_timeout(None).map_err(BridgeError::Io)?;
        Ok(ConnectedTerminal {
            stream,
            accepted,
            label: label.to_owned(),
        })
    }

    fn connect(&mut self) -> Result<ConnectedTerminal, ConnectError> {
        let (addresses, label) = self.resolve_target().map_err(ConnectError::Bridge)?;
        for version in self.config.protocol.versions() {
            match self.connect_protocol(&addresses, &label, *version) {
                Err(ConnectError::Bridge(BridgeError::Handshake(
                    HandshakeError::VersionRejected,
                ))) if self.config.protocol == ProtocolPreference::Auto && *version != 1 => {
                    if self.config.verbose {
                        self.log("X4 rejected this protocol version; trying an older protocol");
                    }
                }
                result => return result,
            }
        }
        unreachable!("the protocol version list is never empty")
    }

    fn install_connection(&mut self, connected: ConnectedTerminal) -> Result<(), BridgeError> {
        self.session
            .resize(connected.accepted.cols, connected.accepted.rows)?;
        self.protocol_version = connected.accepted.version;
        self.frame_decoder.reset();
        self.next_tx_sequence = 1;
        self.pending_output.clear();
        self.pending_input.clear();
        self.next_write_at = Instant::now();
        self.output_burst_open = false;
        self.output_burst_due = None;
        self.burst_boundaries_enabled = connected.accepted.version == 3
            && connected
                .accepted
                .capabilities
                .iter()
                .any(|capability| capability == "burst1");
        self.output_metrics = OutputMetrics::default();
        self.connected_once = true;
        self.last_retry_error.clear();
        self.last_retry_log_at = None;
        self.pending_control = None;
        self.ignored_control_sequence = None;
        self.deferred_clean_due = None;
        if connected.accepted.version == 3 && self.control_server.is_none() {
            match ControlServer::bind(&connected.label) {
                Ok(server) => {
                    if self.config.verbose {
                        self.log(format_args!(
                            "display control available at {}",
                            server.path().display()
                        ));
                    }
                    self.control_server = Some(server);
                }
                Err(error) => self.log(format_args!(
                    "could not expose local display controls; terminal remains usable: {error}"
                )),
            }
        }
        self.log(format_args!(
            "connected to {} at {}x{} using protocol v{}",
            connected.label,
            connected.accepted.cols,
            connected.accepted.rows,
            connected.accepted.version
        ));
        self.connection = Some(connected.stream);
        Ok(())
    }

    fn disconnect(&mut self, reason: impl fmt::Display) {
        let reason = reason.to_string();
        if let Some(pending) = self.pending_control.take() {
            if pending.local_response {
                self.fail_local_control(format_args!("X4 disconnected: {reason}"));
            }
        }
        self.connection.take();
        self.protocol_version = 0;
        self.frame_decoder.reset();
        self.pending_output.clear();
        self.pending_input.clear();
        self.output_burst_open = false;
        self.output_burst_due = None;
        self.burst_boundaries_enabled = false;
        self.ignored_control_sequence = None;
        self.deferred_clean_due = None;
        let suffix = if self.config.reconnect {
            "; waiting for terminal"
        } else {
            ""
        };
        self.log(format_args!("disconnected ({reason}){suffix}"));
    }

    fn handle_control_response(&mut self, frame: &Frame) -> Result<(), BridgeError> {
        if self.ignored_control_sequence == Some(frame.sequence) {
            return Ok(());
        }
        let Some(pending) = self.pending_control.as_mut() else {
            return Err(BridgeError::ControlProtocol(format!(
                "unsolicited X4 control response for sequence {}",
                frame.sequence
            )));
        };
        if frame.sequence != pending.sequence {
            return Err(BridgeError::ControlProtocol(format!(
                "X4 control response sequence {} does not match request {}",
                frame.sequence, pending.sequence
            )));
        }
        let response = decode_diagnostic_response(&frame.payload)?;
        if response.schema != 1 || response.command != pending.diagnostic_command.as_u8() {
            return Err(BridgeError::ControlProtocol(
                "X4 display-control response does not match its request".to_owned(),
            ));
        }
        if response.status != DiagnosticStatus::Accepted as u8 {
            let error = response.error;
            let command = pending.command;
            let local_response = pending.local_response;
            self.pending_control = None;
            let message = format!("X4 rejected display {} (error {error})", command.as_str());
            if local_response {
                self.fail_local_control(&message);
            } else {
                self.log(message);
            }
            return Ok(());
        }
        if pending.accepted {
            return Err(BridgeError::ControlProtocol(
                "X4 sent duplicate display-control acceptance".to_owned(),
            ));
        }

        if matches!(
            pending.command,
            DisplayCommand::Status | DisplayCommand::Metrics
        ) {
            let device = self
                .control_server
                .as_ref()
                .map(ControlServer::device_id)
                .unwrap_or("x4")
                .to_owned();
            let local_response = pending.local_response;
            let result = match pending.command {
                DisplayCommand::Status => status_result(
                    &device,
                    response.metadata.as_ref().ok_or_else(|| {
                        BridgeError::ControlProtocol(
                            "X4 status response has no metadata".to_owned(),
                        )
                    })?,
                ),
                DisplayCommand::Metrics => metrics_result(
                    &device,
                    response.metrics.as_ref().ok_or_else(|| {
                        BridgeError::ControlProtocol(
                            "X4 metrics response has no snapshot".to_owned(),
                        )
                    })?,
                    self.output_metrics,
                    self.burst_boundaries_enabled,
                ),
                _ => unreachable!("read-only command was checked above"),
            };
            self.pending_control = None;
            if local_response {
                self.complete_local_control(result);
            }
        } else {
            pending.accepted = true;
        }
        Ok(())
    }

    fn handle_refresh_event(&mut self, frame: &Frame) -> Result<(), BridgeError> {
        let event = decode_diagnostic_refresh_event(&frame.payload)?;
        if self.ignored_control_sequence == Some(frame.sequence) {
            if event.phase == DiagnosticEventPhase::Ready as u8
                || event.phase == DiagnosticEventPhase::Failed as u8
            {
                self.ignored_control_sequence = None;
            }
            return Ok(());
        }
        let Some(pending) = self.pending_control.as_mut() else {
            return Err(BridgeError::ControlProtocol(format!(
                "unsolicited X4 refresh event for sequence {}",
                frame.sequence
            )));
        };
        if frame.sequence != pending.sequence
            || event.schema != 1
            || event.command != pending.diagnostic_command.as_u8()
            || event.first_sequence != pending.sequence
            || event.last_sequence != pending.sequence
            || event.coalesced != 1
        {
            return Err(BridgeError::ControlProtocol(
                "X4 display refresh does not match its request".to_owned(),
            ));
        }
        if !pending.accepted {
            return Err(BridgeError::ControlProtocol(
                "X4 display refresh arrived before command acceptance".to_owned(),
            ));
        }
        if event.phase == DiagnosticEventPhase::Presented as u8 {
            if pending.presented {
                return Err(BridgeError::ControlProtocol(
                    "X4 sent duplicate PRESENTED refresh telemetry".to_owned(),
                ));
            }
            pending.presented = true;
            return Ok(());
        }
        if event.phase == DiagnosticEventPhase::Failed as u8 {
            let command = pending.command;
            let local_response = pending.local_response;
            self.pending_control = None;
            let message = format!("X4 display {} failed", command.as_str());
            if local_response {
                self.fail_local_control(&message);
            } else {
                self.log(message);
            }
            return Ok(());
        }
        if event.phase != DiagnosticEventPhase::Ready as u8 || !pending.presented {
            return Err(BridgeError::ControlProtocol(
                "X4 display refresh telemetry arrived out of order".to_owned(),
            ));
        }
        let command = pending.command;
        let local_response = pending.local_response;
        self.pending_control = None;
        if local_response {
            self.complete_local_control(refresh_result(command, &event));
        }
        Ok(())
    }

    fn flush_pty_input(&mut self) -> Result<(), BridgeError> {
        if self.pending_input.is_empty() {
            return Ok(());
        }
        match self.session.write(&self.pending_input) {
            Ok(0) => {
                Err(io::Error::new(io::ErrorKind::WriteZero, "PTY closed while writing").into())
            }
            Ok(written) => {
                self.pending_input.drain(..written);
                Ok(())
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => Ok(()),
            Err(error) => Err(error.into()),
        }
    }

    fn read_local_input(&mut self) -> Result<(), BridgeError> {
        let Some(local_input) = self.local_input else {
            return Ok(());
        };
        let mut buffer = [0_u8; LOCAL_INPUT_READ_SIZE];
        match nix::unistd::read(local_input, &mut buffer) {
            Ok(0) => self.local_input = None,
            Ok(length) => {
                self.local_exit_requested |=
                    append_local_input(&mut self.pending_input, &buffer[..length])?;
                self.flush_pty_input()?;
            }
            Err(Errno::EAGAIN) => {}
            Err(error) => return Err(errno_to_io(error).into()),
        }
        Ok(())
    }

    fn queue_due_output_burst_end(&mut self, now: Instant) -> Result<(), BridgeError> {
        let Some(due) = self.output_burst_due else {
            return Ok(());
        };
        if !self.burst_boundaries_enabled || !self.output_burst_open || now < due {
            return Ok(());
        }
        self.pending_output = encode_frame(
            FrameType::TerminalOutputEnd.as_u8(),
            b"",
            self.next_tx_sequence,
            0,
        )?;
        self.next_tx_sequence = self.next_tx_sequence.wrapping_add(1);
        self.output_burst_open = false;
        self.output_burst_due = None;
        self.output_metrics.burst_ends = self.output_metrics.burst_ends.saturating_add(1);
        Ok(())
    }

    fn write_network(&mut self) -> Result<(), BridgeError> {
        if self.pending_output.is_empty() {
            let mut raw = [0_u8; RAW_PTY_READ_SIZE];
            let read_size = if self.protocol_version == 3 {
                MAX_FRAME_PAYLOAD
            } else {
                raw.len()
            };
            match self.session.read(&mut raw[..read_size]) {
                Ok(0) => {}
                Ok(length) if self.protocol_version == 3 => {
                    if self.deferred_clean_due.is_some() {
                        self.deferred_clean_due = Some(Instant::now() + DEFERRED_CLEAN_QUIET);
                    }
                    if let Some(capture) = &mut self.capture {
                        capture.record(&raw[..length])?;
                    }
                    self.pending_output = encode_frame(
                        FrameType::TerminalOutput.as_u8(),
                        &raw[..length],
                        self.next_tx_sequence,
                        0,
                    )?;
                    self.next_tx_sequence = self.next_tx_sequence.wrapping_add(1);
                    self.output_metrics.pty_bytes =
                        self.output_metrics.pty_bytes.saturating_add(length as u64);
                    self.output_metrics.pty_reads = self.output_metrics.pty_reads.saturating_add(1);
                    self.output_metrics.output_frames =
                        self.output_metrics.output_frames.saturating_add(1);
                    if self.burst_boundaries_enabled {
                        self.output_burst_open = true;
                        self.output_burst_due = Some(Instant::now() + PTY_BURST_QUIET);
                    }
                }
                Ok(length) => {
                    if self.deferred_clean_due.is_some() {
                        self.deferred_clean_due = Some(Instant::now() + DEFERRED_CLEAN_QUIET);
                    }
                    if let Some(capture) = &mut self.capture {
                        capture.record(&raw[..length])?;
                    }
                    self.pending_output.extend_from_slice(&raw[..length]);
                    self.output_metrics.pty_bytes =
                        self.output_metrics.pty_bytes.saturating_add(length as u64);
                    self.output_metrics.pty_reads = self.output_metrics.pty_reads.saturating_add(1);
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
                Err(error) if error.raw_os_error() == Some(libc::EIO) => {}
                Err(error) => return Err(error.into()),
            }
            if self.pending_output.is_empty() {
                self.queue_due_output_burst_end(Instant::now())?;
            }
        }
        if self.pending_output.is_empty() || Instant::now() < self.next_write_at {
            return Ok(());
        }
        let connection = self.connection.as_mut().expect("connection is installed");
        match connection.write(&self.pending_output) {
            Ok(0) => Err(io::Error::new(
                io::ErrorKind::ConnectionReset,
                "socket closed while writing",
            )
            .into()),
            Ok(written) => {
                self.pending_output.drain(..written);
                self.next_write_at = Instant::now()
                    + Duration::from_secs_f64(written as f64 / self.config.max_bps as f64);
                Ok(())
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => Ok(()),
            Err(error) => Err(error.into()),
        }
    }

    fn read_network(&mut self) -> Result<(), BridgeError> {
        let mut received = [0_u8; NETWORK_READ_SIZE];
        let connection = self.connection.as_mut().expect("connection is installed");
        let length = match connection.read(&mut received) {
            Ok(0) => {
                return Err(
                    io::Error::new(io::ErrorKind::ConnectionReset, "socket closed by X4").into(),
                )
            }
            Ok(length) => length,
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(()),
            Err(error) => return Err(error.into()),
        };
        if self.protocol_version == 3 {
            for frame in self.frame_decoder.feed(&received[..length])? {
                if frame.frame_type == FrameType::TerminalInput.as_u8() {
                    append_bounded_input(&mut self.pending_input, &frame.payload)?;
                } else if frame.frame_type == FrameType::SessionEnd.as_u8()
                    && frame.payload.is_empty()
                {
                    return Err(BridgeError::SessionEnded);
                } else if frame.frame_type == FrameType::ControlResponse.as_u8() {
                    self.handle_control_response(&frame)?;
                } else if frame.frame_type == FrameType::RefreshEvent.as_u8() {
                    self.handle_refresh_event(&frame)?;
                } else if frame.frame_type == FrameType::Heartbeat.as_u8()
                    || is_optional_frame_type(frame.frame_type)
                {
                    continue;
                } else if is_known_frame_type(frame.frame_type) {
                    return Err(BridgeError::Protocol(ProtocolError::UnexpectedFrameType(
                        frame.frame_type,
                    )));
                } else {
                    return Err(BridgeError::Protocol(
                        ProtocolError::UnknownMandatoryFrameType(frame.frame_type),
                    ));
                }
            }
        } else {
            append_bounded_input(&mut self.pending_input, &received[..length])?;
        }
        self.flush_pty_input()
    }

    fn poll_connected(&mut self) -> Result<(), BridgeError> {
        self.poll_local_control();
        let now = Instant::now();
        let can_write_network = !self.pending_output.is_empty() && now >= self.next_write_at;
        let socket_flags = (if self.pending_input.is_empty() {
            PollFlags::POLLIN
        } else {
            PollFlags::empty()
        }) | if can_write_network {
            PollFlags::POLLOUT
        } else {
            PollFlags::empty()
        };
        let pty_flags = (if self.pending_output.is_empty() && now >= self.next_write_at {
            PollFlags::POLLIN
        } else {
            PollFlags::empty()
        }) | if self.pending_input.is_empty() {
            PollFlags::empty()
        } else {
            PollFlags::POLLOUT
        };
        let mut wait = if now < self.next_write_at {
            self.next_write_at
                .saturating_duration_since(now)
                .min(EVENT_LOOP_INTERVAL)
        } else if self.pending_output.is_empty() {
            EVENT_LOOP_INTERVAL
        } else {
            Duration::ZERO
        };
        if let Some(due) = self.output_burst_due {
            wait = wait.min(due.saturating_duration_since(now));
        }

        let (socket_events, pty_events, local_events) = {
            let connection = self.connection.as_ref().expect("connection is installed");
            let mut descriptors = vec![
                PollFd::new(connection.as_fd(), socket_flags),
                PollFd::new(self.session.master()?.as_fd(), pty_flags),
            ];
            if let Some(local_input) = self.local_input.filter(|_| self.pending_input.is_empty()) {
                descriptors.push(PollFd::new(local_input.as_fd(), PollFlags::POLLIN));
            }
            match poll(&mut descriptors, poll_timeout(wait)) {
                Ok(_) | Err(Errno::EINTR) => {}
                Err(error) => return Err(errno_to_io(error).into()),
            }
            (
                descriptors[0].revents().unwrap_or(PollFlags::empty()),
                descriptors[1].revents().unwrap_or(PollFlags::empty()),
                descriptors
                    .get(2)
                    .and_then(PollFd::revents)
                    .unwrap_or(PollFlags::empty()),
            )
        };

        if socket_events.intersects(PollFlags::POLLIN | PollFlags::POLLHUP | PollFlags::POLLERR) {
            self.read_network()?;
        }
        if local_events.contains(PollFlags::POLLIN) {
            self.read_local_input()?;
        }
        if pty_events.contains(PollFlags::POLLOUT) {
            self.flush_pty_input()?;
        }
        if pty_events.contains(PollFlags::POLLIN)
            || socket_events.contains(PollFlags::POLLOUT)
            || (!self.pending_output.is_empty() && Instant::now() >= self.next_write_at)
            || self
                .output_burst_due
                .is_some_and(|due| Instant::now() >= due)
        {
            self.write_network()?;
        }
        if socket_events.contains(PollFlags::POLLNVAL) {
            return Err(
                io::Error::new(io::ErrorKind::InvalidInput, "invalid network descriptor").into(),
            );
        }
        self.poll_local_control();
        Ok(())
    }

    fn wait_for_retry(&mut self, duration: Duration) -> Result<Option<i32>, BridgeError> {
        let deadline = Instant::now() + duration;
        loop {
            self.poll_local_control();
            if let Some(signal) = self.signals.received() {
                return Ok(Some(128 + signal));
            }
            if self.local_exit_requested {
                return Ok(Some(0));
            }
            if let Some(status) = self.session.try_wait()? {
                return Ok(Some(exit_status_code(status)));
            }
            let now = Instant::now();
            if now >= deadline {
                return Ok(None);
            }
            let wait = deadline
                .saturating_duration_since(now)
                .min(EVENT_LOOP_INTERVAL);
            if let Some(local_input) = self.local_input {
                let ready = {
                    let mut descriptor = [PollFd::new(local_input.as_fd(), PollFlags::POLLIN)];
                    match poll(&mut descriptor, poll_timeout(wait)) {
                        Ok(_) | Err(Errno::EINTR) => {}
                        Err(error) => return Err(errno_to_io(error).into()),
                    }
                    descriptor[0]
                        .revents()
                        .unwrap_or(PollFlags::empty())
                        .contains(PollFlags::POLLIN)
                };
                if ready {
                    self.read_local_input()?;
                }
            } else {
                let mut descriptors = [];
                match poll(&mut descriptors, poll_timeout(wait)) {
                    Ok(_) | Err(Errno::EINTR) => {}
                    Err(error) => return Err(errno_to_io(error).into()),
                }
            }
        }
    }

    pub fn run(&mut self) -> Result<i32, BridgeError> {
        loop {
            if let Some(signal) = self.signals.received() {
                return Ok(128 + signal);
            }
            if self.local_exit_requested {
                return Ok(0);
            }
            if let Some(status) = self.session.try_wait()? {
                return Ok(exit_status_code(status));
            }

            if self.connection.is_none() {
                let retry_interval = match self.connect() {
                    Ok(connected) => {
                        self.install_connection(connected)?;
                        continue;
                    }
                    Err(ConnectError::Interrupted(signal)) => return Ok(128 + signal),
                    Err(ConnectError::Denied) => {
                        self.log(format_args!(
                            "connection denied on the X4; retrying in {}s",
                            DENIED_RETRY_INTERVAL.as_secs()
                        ));
                        DENIED_RETRY_INTERVAL
                    }
                    Err(ConnectError::Bridge(error)) => {
                        self.log_retry_error(&error);
                        self.config.retry_interval
                    }
                };
                if let Some(code) = self.wait_for_retry(retry_interval)? {
                    return Ok(code);
                }
                continue;
            }

            if let Err(error) = self.poll_connected() {
                self.disconnect(&error);
                if matches!(
                    error,
                    BridgeError::CaptureIo(_) | BridgeError::CaptureLimitExceeded(_)
                ) {
                    return Err(error);
                }
                if self.connected_once && !self.config.reconnect {
                    return Ok(0);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::io::{BufRead, BufReader};
    use std::net::TcpListener;
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::UnixStream;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::thread;

    fn temp_capture_path(label: &str) -> PathBuf {
        static NEXT_ID: AtomicU64 = AtomicU64::new(1);
        std::env::temp_dir().join(format!(
            "knietty-{}-{}-{label}.raw",
            std::process::id(),
            NEXT_ID.fetch_add(1, Ordering::Relaxed)
        ))
    }

    #[test]
    fn pty_capture_is_private_bounded_and_refuses_overwrite() {
        let path = temp_capture_path("privacy");
        let mut capture = PtyCapture::create(&path, 4).unwrap();
        capture.record(b"ab").unwrap();
        capture.record(b"cd").unwrap();
        assert!(matches!(
            capture.record(b"e"),
            Err(BridgeError::CaptureLimitExceeded(4))
        ));
        drop(capture);

        assert_eq!(fs::read(&path).unwrap(), b"abcd");
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o077, 0);
        assert_eq!(
            PtyCapture::create(&path, 4).unwrap_err().kind(),
            io::ErrorKind::AlreadyExists
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn protocol_preferences_are_ordered_and_bounded() {
        assert_eq!(ProtocolPreference::Auto.versions(), &[3, 2, 1]);
        assert_eq!(ProtocolPreference::V3.versions(), &[3]);
        assert_eq!(ProtocolPreference::V2.versions(), &[2]);
        assert_eq!(ProtocolPreference::V1.versions(), &[1]);
    }

    #[test]
    fn input_queue_rejects_overflow_without_partial_append() {
        let mut pending = vec![0; MAX_PENDING_INPUT - 2];
        assert_eq!(
            append_bounded_input(&mut pending, b"abc")
                .unwrap_err()
                .to_string(),
            "X4 input exceeded the bounded host queue"
        );
        assert_eq!(pending.len(), MAX_PENDING_INPUT - 2);
    }

    #[test]
    fn local_ctrl_c_is_forwarded_and_ctrl_backslash_is_consumed() {
        let mut pending = Vec::new();
        assert!(append_local_input(&mut pending, b"abc\x03\x1cignored").unwrap());
        assert_eq!(pending, b"abc\x03");
    }

    fn fake_listener() -> (TcpListener, SocketAddr) {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let address = listener.local_addr().unwrap();
        (listener, address)
    }

    fn read_hello(stream: &TcpStream) -> String {
        let mut line = String::new();
        BufReader::new(stream.try_clone().unwrap())
            .read_line(&mut line)
            .unwrap();
        line
    }

    fn refresh_payload(
        phase: DiagnosticEventPhase,
        command: DiagnosticCommand,
        sequence: u32,
    ) -> Vec<u8> {
        let mut payload = vec![0_u8; crate::protocol::REFRESH_EVENT_SIZE];
        payload[0] = 1;
        payload[1] = phase as u8;
        payload[2] = command.as_u8();
        payload[3] = 3;
        payload[4] = 3;
        payload[8 + 14 * 4..8 + 15 * 4].copy_from_slice(&125_000_u32.to_be_bytes());
        payload[84..88].copy_from_slice(&47_200_u32.to_be_bytes());
        payload[91] = 1;
        payload[92..96].copy_from_slice(&sequence.to_be_bytes());
        payload[96..100].copy_from_slice(&sequence.to_be_bytes());
        payload[100..104].copy_from_slice(&123_456_u32.to_be_bytes());
        payload[104..108].copy_from_slice(&120_000_u32.to_be_bytes());
        payload
    }

    fn metrics_payload() -> Vec<u8> {
        let mut payload = vec![
            1,
            DiagnosticCommand::Metrics.as_u8(),
            DiagnosticStatus::Accepted as u8,
            0,
        ];
        for value in 1_u32..=16 {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        payload.extend_from_slice(&800_u16.to_be_bytes());
        payload.extend_from_slice(&432_u16.to_be_bytes());
        payload.extend_from_slice(&42_768_u32.to_be_bytes());
        payload.extend_from_slice(&53_416_u32.to_be_bytes());
        payload.extend_from_slice(&44_588_u32.to_be_bytes());
        for value in 17_u32..=22 {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        payload
    }

    fn test_bridge<'a>(
        session: &'a mut PtySession,
        signals: &'a ShutdownSignals,
        address: SocketAddr,
        protocol: ProtocolPreference,
    ) -> NetworkBridge<'a> {
        NetworkBridge::new(
            session,
            BridgeConfig {
                host: address.ip().to_string(),
                port: address.port(),
                retry_interval: Duration::from_millis(20),
                discovery_timeout: Duration::from_millis(20),
                approval_timeout: Duration::from_secs(2),
                max_bps: DEFAULT_MAX_BPS,
                capture_output: None,
                capture_limit: DEFAULT_CAPTURE_LIMIT,
                reconnect: false,
                protocol,
                verbose: false,
                security: SecurityMode::InsecurePlaintext,
            },
            signals,
            None,
        )
        .unwrap()
    }

    #[test]
    fn automatic_handshake_falls_back_only_after_version_rejection() {
        let (listener, address) = fake_listener();
        let server = thread::spawn(move || {
            for (expected, response) in [
                ("KNIETTY/3", b"KNIETTY/3 ERROR\n".as_slice()),
                ("KNIETTY/2", b"KNIETTY/2 ERROR\n".as_slice()),
                ("KNIETTY/1", b"KNIETTY/1 ACCEPT 42 21\n".as_slice()),
            ] {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(2)))
                    .unwrap();
                assert!(read_hello(&stream).starts_with(expected));
                stream.write_all(response).unwrap();
            }
        });
        let mut session = PtySession::spawn("sleep 5", 80, 24, "vt100").unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::Auto);
        let connected = bridge.connect().unwrap();
        assert_eq!(connected.accepted.version, 1);
        assert_eq!((connected.accepted.cols, connected.accepted.rows), (42, 21));
        drop(connected);
        drop(bridge);
        session.close().unwrap();
        server.join().unwrap();
    }

    #[test]
    fn approval_denial_and_malformed_response_are_distinct() {
        for (response, expected) in [
            (b"KNIETTY/3 DENY\n".as_slice(), "denied"),
            (b"not-a-handshake\n".as_slice(), "malformed"),
        ] {
            let (listener, address) = fake_listener();
            let response = response.to_vec();
            let server = thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                assert!(read_hello(&stream).starts_with("KNIETTY/3"));
                stream.write_all(&response).unwrap();
            });
            let mut session = PtySession::spawn("sleep 5", 80, 24, "vt100").unwrap();
            let signals = ShutdownSignals::install().unwrap();
            let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::V3);
            let error = bridge.connect().unwrap_err();
            match (expected, error) {
                ("denied", ConnectError::Denied) => {}
                (
                    "malformed",
                    ConnectError::Bridge(BridgeError::Handshake(HandshakeError::Unexpected(_))),
                ) => {}
                (_, error) => panic!("unexpected connection result: {error:?}"),
            }
            drop(bridge);
            session.close().unwrap();
            server.join().unwrap();
        }
    }

    #[test]
    fn v3_bridge_relays_pty_output_and_device_input_then_exits_on_disconnect() {
        let capture_path = temp_capture_path("v3-bridge");
        let (listener, address) = fake_listener();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            assert!(read_hello(&stream).starts_with("KNIETTY/3 HELLO terminal frame "));
            stream.write_all(b"KNIETTY/3 ACCEPT 42 21 frame\n").unwrap();
            stream
                .write_all(
                    &encode_frame(FrameType::TerminalInput.as_u8(), b"hello\n", 9, 0).unwrap(),
                )
                .unwrap();

            let mut decoder = FrameDecoder::new();
            let mut terminal_output = Vec::new();
            let mut buffer = [0_u8; 256];
            while !terminal_output
                .windows(b"got:hello".len())
                .any(|window| window == b"got:hello")
            {
                let length = stream.read(&mut buffer).unwrap();
                assert_ne!(length, 0, "host disconnected before relaying PTY output");
                for frame in decoder.feed(&buffer[..length]).unwrap() {
                    if frame.frame_type == FrameType::Heartbeat.as_u8() {
                        assert_eq!(frame.sequence, 0);
                        assert!(frame.payload.is_empty());
                        continue;
                    }
                    assert_eq!(frame.frame_type, FrameType::TerminalOutput.as_u8());
                    terminal_output.extend_from_slice(&frame.payload);
                }
            }
        });
        let mut session = PtySession::spawn(
            "IFS= read -r value; printf 'got:%s\\n' \"$value\"; sleep 5",
            80,
            24,
            "vt100",
        )
        .unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::V3);
        bridge.capture = Some(PtyCapture::create(&capture_path, 4096).unwrap());
        assert_eq!(bridge.run().unwrap(), 0);
        drop(bridge);
        session.close().unwrap();
        server.join().unwrap();
        let captured = fs::read(&capture_path).unwrap();
        assert!(captured
            .windows(b"got:hello".len())
            .any(|window| window == b"got:hello"));
        fs::remove_file(capture_path).unwrap();
    }

    #[test]
    fn burst1_drains_multi_frame_pty_output_before_the_quiet_boundary() {
        let (listener, address) = fake_listener();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            assert!(read_hello(&stream).starts_with("KNIETTY/3 HELLO terminal frame "));
            stream
                .write_all(b"KNIETTY/3 ACCEPT 80 24 frame,burst1\n")
                .unwrap();

            let mut decoder = FrameDecoder::new();
            let mut buffer = [0_u8; 1024];
            let mut bytes = 0;
            let mut frames = 0;
            let mut first_output_at = None;
            let elapsed = 'read: loop {
                let length = stream.read(&mut buffer).unwrap();
                assert_ne!(length, 0, "host disconnected before the burst boundary");
                for frame in decoder.feed(&buffer[..length]).unwrap() {
                    if frame.frame_type == FrameType::Heartbeat.as_u8() {
                        continue;
                    }
                    if frame.frame_type == FrameType::TerminalOutput.as_u8() {
                        first_output_at.get_or_insert_with(Instant::now);
                        bytes += frame.payload.len();
                        frames += 1;
                        continue;
                    }
                    assert_eq!(frame.frame_type, FrameType::TerminalOutputEnd.as_u8());
                    assert!(frame.payload.is_empty());
                    break 'read first_output_at
                        .expect("boundary followed PTY output")
                        .elapsed();
                }
            };
            stream
                .write_all(&encode_frame(FrameType::SessionEnd.as_u8(), b"", 999, 0).unwrap())
                .unwrap();
            (bytes, frames, elapsed)
        });

        let mut session = PtySession::spawn(
            "awk 'BEGIN { for (i=0; i<4096; i++) printf \"x\" }'; sleep 5",
            80,
            24,
            "vt100",
        )
        .unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::V3);
        assert_eq!(bridge.run().unwrap(), 0);
        drop(bridge);
        session.close().unwrap();
        let (bytes, frames, elapsed) = server.join().unwrap();
        assert_eq!(bytes, 4096);
        assert_eq!(frames, 8);
        assert!(
            elapsed < Duration::from_millis(400),
            "multi-frame burst took {elapsed:?}; pacing likely slept for the 100 ms event-loop interval"
        );
    }

    #[test]
    fn v3_session_end_stops_the_bridge_before_the_tcp_peer_closes() {
        let (listener, address) = fake_listener();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            assert!(read_hello(&stream).starts_with("KNIETTY/3"));
            stream.write_all(b"KNIETTY/3 ACCEPT 42 21 frame\n").unwrap();
            stream
                .write_all(&encode_frame(FrameType::SessionEnd.as_u8(), b"", 1, 0).unwrap())
                .unwrap();

            let mut buffer = [0_u8; 256];
            loop {
                match stream.read(&mut buffer) {
                    Ok(0) => break,
                    Ok(_) => continue,
                    Err(error) => panic!("host did not close after SESSION_END: {error}"),
                }
            }
        });
        let mut session = PtySession::spawn("sleep 5", 80, 24, "vt100").unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::V3);
        assert_eq!(bridge.run().unwrap(), 0);
        drop(bridge);
        session.close().unwrap();
        server.join().unwrap();
    }

    #[test]
    fn active_bridge_relays_a_bounded_clean_command_and_waits_for_ready() {
        let (listener, address) = fake_listener();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            assert!(read_hello(&stream).starts_with("KNIETTY/3"));
            stream
                .write_all(b"KNIETTY/3 ACCEPT 80 24 frame,burst1\n")
                .unwrap();

            let mut decoder = FrameDecoder::new();
            let mut buffer = [0_u8; 256];
            let sequence = loop {
                let length = stream.read(&mut buffer).unwrap();
                assert_ne!(length, 0, "bridge closed before the display command");
                let mut found = None;
                for frame in decoder.feed(&buffer[..length]).unwrap() {
                    if frame.frame_type == FrameType::ControlRequest.as_u8() {
                        assert_eq!(frame.payload, vec![DiagnosticCommand::Clean.as_u8()]);
                        found = Some(frame.sequence);
                    }
                }
                if let Some(sequence) = found {
                    break sequence;
                }
            };

            let mut response = encode_frame(
                FrameType::ControlResponse.as_u8(),
                &[
                    1,
                    DiagnosticCommand::Clean.as_u8(),
                    DiagnosticStatus::Accepted as u8,
                    0,
                ],
                sequence,
                0,
            )
            .unwrap();
            response.extend_from_slice(
                &encode_frame(
                    FrameType::RefreshEvent.as_u8(),
                    &refresh_payload(
                        DiagnosticEventPhase::Presented,
                        DiagnosticCommand::Clean,
                        sequence,
                    ),
                    sequence,
                    0,
                )
                .unwrap(),
            );
            response.extend_from_slice(
                &encode_frame(
                    FrameType::RefreshEvent.as_u8(),
                    &refresh_payload(
                        DiagnosticEventPhase::Ready,
                        DiagnosticCommand::Clean,
                        sequence,
                    ),
                    sequence,
                    0,
                )
                .unwrap(),
            );
            response.extend_from_slice(
                &encode_frame(FrameType::SessionEnd.as_u8(), b"", sequence + 1, 0).unwrap(),
            );
            stream.write_all(&response).unwrap();
        });

        let mut session = PtySession::spawn("sleep 5", 80, 24, "vt100").unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::V3);
        bridge.control_server =
            Some(ControlServer::bind(&format!("bridge-test-{}", std::process::id())).unwrap());
        let connected = bridge.connect().unwrap();
        bridge.install_connection(connected).unwrap();
        let control_path = bridge.control_server.as_ref().unwrap().path().to_owned();
        let client = thread::spawn(move || {
            let mut stream = UnixStream::connect(control_path).unwrap();
            stream.write_all(b"KNIETTY-CONTROL/1 clean\n").unwrap();
            let mut response = String::new();
            stream.read_to_string(&mut response).unwrap();
            serde_json::from_str::<Value>(&response).unwrap()
        });

        assert_eq!(bridge.run().unwrap(), 0);
        let response = client.join().unwrap();
        assert_eq!(response["ok"], true);
        assert_eq!(response["result"]["command"], "clean");
        assert_eq!(response["result"]["phase"], "ready");
        assert_eq!(response["result"]["actual_path"], "half");
        assert_eq!(response["result"]["total_us"], 125_000);
        drop(bridge);
        session.close().unwrap();
        server.join().unwrap();
    }

    #[test]
    fn active_bridge_fetches_read_only_metrics_without_a_refresh() {
        let (listener, address) = fake_listener();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            assert!(read_hello(&stream).starts_with("KNIETTY/3"));
            stream
                .write_all(b"KNIETTY/3 ACCEPT 80 24 frame,burst1\n")
                .unwrap();

            let mut decoder = FrameDecoder::new();
            let mut buffer = [0_u8; 256];
            let sequence = loop {
                let length = stream.read(&mut buffer).unwrap();
                assert_ne!(length, 0, "bridge closed before the metrics command");
                let mut found = None;
                for frame in decoder.feed(&buffer[..length]).unwrap() {
                    if frame.frame_type == FrameType::ControlRequest.as_u8() {
                        assert_eq!(frame.payload, vec![DiagnosticCommand::Metrics.as_u8()]);
                        found = Some(frame.sequence);
                    }
                }
                if let Some(sequence) = found {
                    break sequence;
                }
            };

            let mut response = encode_frame(
                FrameType::ControlResponse.as_u8(),
                &metrics_payload(),
                sequence,
                0,
            )
            .unwrap();
            response.extend_from_slice(
                &encode_frame(FrameType::SessionEnd.as_u8(), b"", sequence + 1, 0).unwrap(),
            );
            stream.write_all(&response).unwrap();
        });

        let mut session = PtySession::spawn("sleep 5", 80, 24, "vt100").unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::V3);
        bridge.control_server =
            Some(ControlServer::bind(&format!("metrics-test-{}", std::process::id())).unwrap());
        let connected = bridge.connect().unwrap();
        bridge.install_connection(connected).unwrap();
        let control_path = bridge.control_server.as_ref().unwrap().path().to_owned();
        let client = thread::spawn(move || {
            let mut stream = UnixStream::connect(control_path).unwrap();
            stream.write_all(b"KNIETTY-CONTROL/1 metrics\n").unwrap();
            let mut response = String::new();
            stream.read_to_string(&mut response).unwrap();
            serde_json::from_str::<Value>(&response).unwrap()
        });

        assert_eq!(bridge.run().unwrap(), 0);
        let response = client.join().unwrap();
        assert_eq!(response["ok"], true);
        assert_eq!(response["result"]["command"], "metrics");
        assert_eq!(response["result"]["updates"], 1);
        assert_eq!(response["result"]["fallback"], 3);
        assert_eq!(response["result"]["last_region"]["width"], 800);
        assert_eq!(response["result"]["last_region"]["bytes"], 42_768);
        assert_eq!(response["result"]["free_heap"], 53_416);
        assert_eq!(response["result"]["device_pipeline"]["rx_bytes"], 17);
        assert_eq!(response["result"]["device_pipeline"]["burst_ends"], 19);
        assert_eq!(response["result"]["host_pipeline"]["burst1"], true);
        assert_eq!(response["result"]["host_pipeline"]["quiet_ms"], 24);
        drop(bridge);
        session.close().unwrap();
        server.join().unwrap();
    }

    #[test]
    fn reconnect_opens_a_new_session_and_preserves_the_pty_child() {
        let (listener, address) = fake_listener();
        let server = thread::spawn(move || {
            let (mut first, _) = listener.accept().unwrap();
            assert!(read_hello(&first).starts_with("KNIETTY/3"));
            first.write_all(b"KNIETTY/3 ACCEPT 42 21 frame\n").unwrap();
            drop(first);

            let (mut second, _) = listener.accept().unwrap();
            assert!(read_hello(&second).starts_with("KNIETTY/3"));
            second.write_all(b"KNIETTY/3 ACCEPT 42 21 frame\n").unwrap();
            second
                .write_all(
                    &encode_frame(FrameType::TerminalInput.as_u8(), b"exit\n", 1, 0).unwrap(),
                )
                .unwrap();
            let mut buffer = [0_u8; 256];
            while second.read(&mut buffer).unwrap_or(0) != 0 {}
        });
        let mut session = PtySession::spawn(
            "IFS= read -r value; test \"$value\" = exit",
            80,
            24,
            "vt100",
        )
        .unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut bridge = test_bridge(&mut session, &signals, address, ProtocolPreference::V3);
        bridge.config.reconnect = true;
        assert_eq!(bridge.run().unwrap(), 0);
        drop(bridge);
        session.close().unwrap();
        server.join().unwrap();
    }
}

use std::collections::{HashMap, HashSet, VecDeque};
use std::error::Error;
use std::fmt;
use std::fs::{self, File};
use std::io::{self, BufWriter, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::path::PathBuf;
use std::thread;
use std::time::{Duration, Instant};

use nix::sys::time::TimeValLike;
use nix::sys::utsname::uname;
use nix::time::ClockId;
use serde_json::{json, Map, Value};

use crate::bridge::{DEFAULT_APPROVAL_TIMEOUT, DEFAULT_CONNECT_TIMEOUT};
use crate::discovery::{discover_network_devices, DEFAULT_DISCOVERY_TIMEOUT, DEFAULT_WIFI_PORT};
use crate::handshake::{
    diagnostics_hello, parse_server_accept, protocol_client_name, protocol_host_time,
    HandshakeError, ServerAccept, HANDSHAKE_LINE_LIMIT,
};
use crate::protocol::{
    decode_diagnostic_refresh_event, decode_diagnostic_response, encode_diagnostic_command,
    encode_frame, is_optional_frame_type, u32_before_or_equal, DiagnosticCommand,
    DiagnosticEventPhase, DiagnosticPattern, DiagnosticRefreshEvent, DiagnosticSessionMetadata,
    DiagnosticStatus, Frame, FrameDecoder, FrameType, ProtocolError,
};
use crate::signals::ShutdownSignals;
use crate::transport::{SecurityMode, TerminalStream};

const IO_POLL_INTERVAL: Duration = Duration::from_millis(100);
const RECEIVE_BUFFER_SIZE: usize = 2048;
pub const DEFAULT_COMMAND_TIMEOUT: Duration = Duration::from_secs(15);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DiagnosticSuite {
    Smoke,
    Latency,
    Cadence,
    Burst,
}

impl DiagnosticSuite {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Smoke => "smoke",
            Self::Latency => "latency",
            Self::Cadence => "cadence",
            Self::Burst => "burst",
        }
    }
}

#[derive(Clone, Debug)]
pub struct DiagnosticsConfig {
    pub host: String,
    pub port: u16,
    pub suite: DiagnosticSuite,
    pub output: PathBuf,
    pub repetitions: u8,
    pub settle: Duration,
    pub discovery_timeout: Duration,
    pub approval_timeout: Duration,
    pub command_timeout: Duration,
    pub verbose: bool,
    pub security: SecurityMode,
}

impl Default for DiagnosticsConfig {
    fn default() -> Self {
        Self {
            host: "auto".to_owned(),
            port: DEFAULT_WIFI_PORT,
            suite: DiagnosticSuite::Smoke,
            output: PathBuf::from("knietty-diagnostics.jsonl"),
            repetitions: 3,
            settle: Duration::from_secs(1),
            discovery_timeout: DEFAULT_DISCOVERY_TIMEOUT,
            approval_timeout: DEFAULT_APPROVAL_TIMEOUT,
            command_timeout: DEFAULT_COMMAND_TIMEOUT,
            verbose: false,
            security: SecurityMode::Tls,
        }
    }
}

#[derive(Debug)]
pub enum DiagnosticError {
    Io(io::Error),
    Json(serde_json::Error),
    Protocol(ProtocolError),
    Handshake(HandshakeError),
    Message(String),
    Interrupted(i32),
}

impl fmt::Display for DiagnosticError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => error.fmt(formatter),
            Self::Json(error) => error.fmt(formatter),
            Self::Protocol(error) => error.fmt(formatter),
            Self::Handshake(error) => error.fmt(formatter),
            Self::Message(message) => formatter.write_str(message),
            Self::Interrupted(signal) => write!(formatter, "interrupted by signal {signal}"),
        }
    }
}

impl Error for DiagnosticError {}

impl From<io::Error> for DiagnosticError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

impl From<serde_json::Error> for DiagnosticError {
    fn from(error: serde_json::Error) -> Self {
        Self::Json(error)
    }
}

impl From<ProtocolError> for DiagnosticError {
    fn from(error: ProtocolError) -> Self {
        Self::Protocol(error)
    }
}

impl From<HandshakeError> for DiagnosticError {
    fn from(error: HandshakeError) -> Self {
        Self::Handshake(error)
    }
}

type Context = Map<String, Value>;

fn message(text: impl Into<String>) -> DiagnosticError {
    DiagnosticError::Message(text.into())
}

fn monotonic_ns() -> Result<u64, DiagnosticError> {
    let timestamp = ClockId::CLOCK_MONOTONIC
        .now()
        .map_err(|error| io::Error::from_raw_os_error(error as i32))?;
    u64::try_from(timestamp.num_nanoseconds())
        .map_err(|_| message("host monotonic clock returned a negative timestamp"))
}

fn connect_addresses(
    addresses: &[SocketAddr],
    timeout: Duration,
) -> Result<(TcpStream, SocketAddr), DiagnosticError> {
    let mut last_error = None;
    for address in addresses {
        match TcpStream::connect_timeout(address, timeout) {
            Ok(stream) => return Ok((stream, *address)),
            Err(error) => last_error = Some(error),
        }
    }
    Err(last_error
        .unwrap_or_else(|| io::Error::new(io::ErrorKind::AddrNotAvailable, "no TCP address"))
        .into())
}

fn write_record(output: &mut dyn Write, record: &Context) -> Result<(), DiagnosticError> {
    serde_json::to_writer(&mut *output, record)?;
    output.write_all(b"\n")?;
    output.flush()?;
    Ok(())
}

fn with_context(mut record: Context, context: &Context) -> Context {
    for (key, value) in context {
        record.insert(key.clone(), value.clone());
    }
    record
}

fn object(value: Value) -> Context {
    value
        .as_object()
        .expect("JSON literal is an object")
        .clone()
}

fn append_refresh_values(record: &mut Context, event: &DiagnosticRefreshEvent) {
    for (key, value) in [
        ("timestamp_us", json!(event.timestamp_us)),
        ("rx_at_us", json!(event.rx_at_us)),
        ("parsed_at_us", json!(event.parsed_at_us)),
        ("queued_at_us", json!(event.queued_at_us)),
        ("render_started_at_us", json!(event.render_started_at_us)),
        ("queue_us", json!(event.queue_us)),
        ("render_us", json!(event.render_us)),
        ("transfer_us", json!(event.transfer_us)),
        ("lut_us", json!(event.lut_us)),
        ("plane_us", json!(event.plane_us)),
        ("activation_to_busy_us", json!(event.activation_to_busy_us)),
        ("waveform_us", json!(event.waveform_us)),
        ("baseline_us", json!(event.baseline_us)),
        ("power_off_us", json!(event.power_off_us)),
        ("total_us", json!(event.total_us)),
        ("logical_x", json!(event.logical_x)),
        ("logical_y", json!(event.logical_y)),
        ("logical_width", json!(event.logical_width)),
        ("logical_height", json!(event.logical_height)),
        ("aligned_x", json!(event.aligned_x)),
        ("aligned_y", json!(event.aligned_y)),
        ("aligned_width", json!(event.aligned_width)),
        ("aligned_height", json!(event.aligned_height)),
        ("transfer_bytes", json!(event.transfer_bytes)),
        ("dirty_cells", json!(event.dirty_cells)),
        ("dirty_rows", json!(event.dirty_rows)),
        ("coalesced", json!(event.coalesced)),
        ("first_sequence", json!(event.first_sequence)),
        ("last_sequence", json!(event.last_sequence)),
        ("free_heap", json!(event.free_heap)),
        ("minimum_free_heap", json!(event.minimum_free_heap)),
    ] {
        record.insert(key.to_owned(), value);
    }
}

fn session_metadata_values(metadata: &DiagnosticSessionMetadata) -> Context {
    object(json!({
        "profile": metadata.profile,
        "spi_mhz": metadata.spi_mhz,
        "flags": metadata.flags,
        "auto_settle": metadata.auto_settle(),
        "balanced_sustain": metadata.balanced_sustain(),
        "waveform_100ms": metadata.waveform_100ms(),
        "ram_ping_pong": metadata.ram_ping_pong(),
        "orientation": metadata.orientation,
        "board": metadata.board,
        "controller": metadata.controller,
        "battery_percent": metadata.battery_percent,
        "rssi_dbm": metadata.rssi_dbm,
        "columns": metadata.columns,
        "rows": metadata.rows,
        "font": metadata.font,
        "display_width": metadata.display_width,
        "display_height": metadata.display_height,
        "free_heap": metadata.free_heap,
        "minimum_free_heap": metadata.minimum_free_heap,
        "build": metadata.build,
        "freeink": metadata.freeink,
    }))
}

fn host_platform() -> Result<(String, String), DiagnosticError> {
    let identity = uname().map_err(|error| io::Error::from_raw_os_error(error as i32))?;
    Ok((
        identity.sysname().to_string_lossy().into_owned(),
        identity.release().to_string_lossy().into_owned(),
    ))
}

#[derive(Clone, Debug)]
struct PatternRequest {
    pattern: DiagnosticPattern,
    variant: u8,
    context: Context,
}

#[derive(Clone, Debug)]
struct SentRequest {
    sent_at_ns: u64,
    sample_index: u8,
}

pub struct DiagnosticClient<'a> {
    config: DiagnosticsConfig,
    signals: &'a ShutdownSignals,
    connection: Option<TerminalStream>,
    decoder: FrameDecoder,
    pending_frames: VecDeque<Frame>,
    sequence: u32,
    target_label: String,
}

impl<'a> DiagnosticClient<'a> {
    pub fn new(config: DiagnosticsConfig, signals: &'a ShutdownSignals) -> Self {
        let target_label = config.host.clone();
        Self {
            config,
            signals,
            connection: None,
            decoder: FrameDecoder::new(),
            pending_frames: VecDeque::new(),
            sequence: 1,
            target_label,
        }
    }

    fn log(&self, text: impl fmt::Display) {
        eprintln!("knietty diagnose: {text}");
    }

    fn check_signal(&self) -> Result<(), DiagnosticError> {
        match self.signals.received() {
            Some(signal) => Err(DiagnosticError::Interrupted(signal)),
            None => Ok(()),
        }
    }

    fn resolve_target(&mut self) -> Result<Vec<SocketAddr>, DiagnosticError> {
        if self.config.host != "auto" {
            let addresses: Vec<_> = (self.config.host.as_str(), self.config.port)
                .to_socket_addrs()?
                .collect();
            if addresses.is_empty() {
                return Err(message(format!(
                    "could not resolve a TCP address for {:?}",
                    self.config.host
                )));
            }
            return Ok(addresses);
        }
        let devices = discover_network_devices(self.config.discovery_timeout, self.config.port)?;
        match devices.as_slice() {
            [] => Err(message("no knietty terminal found on the local network")),
            [device] => {
                self.target_label = device.name.clone();
                Ok(vec![SocketAddr::new(device.address, device.port)])
            }
            _ => {
                let choices = devices
                    .iter()
                    .map(|device| format!("{} ({})", device.name, device.address))
                    .collect::<Vec<_>>()
                    .join(", ");
                Err(message(format!(
                    "multiple knietty terminals found ({choices}); pass --host"
                )))
            }
        }
    }

    fn read_handshake_line(&self, stream: &mut TerminalStream) -> Result<Vec<u8>, DiagnosticError> {
        let deadline = Instant::now() + self.config.approval_timeout;
        let mut line = Vec::with_capacity(HANDSHAKE_LINE_LIMIT);
        let mut byte = [0_u8; 1];
        stream.set_read_timeout(Some(IO_POLL_INTERVAL))?;
        loop {
            self.check_signal()?;
            if line.len() >= HANDSHAKE_LINE_LIMIT {
                return Err(HandshakeError::LineTooLong.into());
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "timed out waiting for diagnostics approval on the X4",
                )
                .into());
            }
            match stream.read(&mut byte) {
                Ok(0) => return Err(HandshakeError::Disconnected.into()),
                Ok(_) => {
                    line.push(byte[0]);
                    if byte[0] == b'\n' {
                        return Ok(line);
                    }
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                    ) => {}
                Err(error) => return Err(error.into()),
            }
        }
    }

    fn connect(&mut self) -> Result<ServerAccept, DiagnosticError> {
        self.check_signal()?;
        let addresses = self.resolve_target()?;
        let (raw_stream, address) = connect_addresses(&addresses, DEFAULT_CONNECT_TIMEOUT)?;
        raw_stream.set_nodelay(true)?;
        let mut stream = TerminalStream::connect(
            raw_stream,
            &self.target_label,
            self.config.security,
            DEFAULT_CONNECT_TIMEOUT,
        )?;
        if let Some(pairing) = stream.pairing() {
            if pairing.first_pairing {
                self.log(format_args!(
                    "first pairing code {} (verify it matches the X4 before pressing Confirm)",
                    pairing.code
                ));
            } else if self.config.verbose {
                self.log(format_args!(
                    "authenticated paired X4 {}",
                    pairing.device_fingerprint
                ));
            }
        }
        stream.set_write_timeout(Some(self.config.approval_timeout))?;
        let (epoch, offset) = protocol_host_time();
        let hello = diagnostics_hello(&protocol_client_name(None), epoch, offset);
        stream.write_all(hello.as_bytes())?;
        self.log(format_args!(
            "approve the bounded display test on {} ({}:{})",
            self.target_label,
            address.ip(),
            address.port()
        ));
        let response = self.read_handshake_line(&mut stream)?;
        let accepted = parse_server_accept(&response)?;
        if accepted.version != 3
            || !accepted
                .capabilities
                .iter()
                .any(|capability| capability == "diag1")
        {
            return Err(message("X4 does not advertise diagnostics protocol diag1"));
        }
        stream.confirm_pairing()?;
        let pair_commit = encode_frame(FrameType::Heartbeat.as_u8(), b"", 0, 0)?;
        stream.write_all(&pair_commit)?;
        stream.set_write_timeout(Some(self.config.command_timeout))?;
        stream.set_read_timeout(Some(IO_POLL_INTERVAL))?;
        self.connection = Some(stream);
        Ok(accepted)
    }

    fn connection(&mut self) -> Result<&mut TerminalStream, DiagnosticError> {
        self.connection
            .as_mut()
            .ok_or_else(|| message("diagnostics connection is not open"))
    }

    fn next_frame(&mut self, deadline: Instant) -> Result<Frame, DiagnosticError> {
        loop {
            self.check_signal()?;
            if let Some(frame) = self.pending_frames.pop_front() {
                return Ok(frame);
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "timed out waiting for a diagnostic response",
                )
                .into());
            }
            let mut buffer = [0_u8; RECEIVE_BUFFER_SIZE];
            match self.connection()?.read(&mut buffer) {
                Ok(0) => return Err(message("X4 disconnected during diagnostics")),
                Ok(length) => self
                    .pending_frames
                    .extend(self.decoder.feed(&buffer[..length])?),
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                    ) => {}
                Err(error) => return Err(error.into()),
            }
        }
    }

    fn next_sequence(&mut self) -> u32 {
        let sequence = self.sequence;
        self.sequence = self.sequence.wrapping_add(1);
        sequence
    }

    fn sleep_interruptible(&self, duration: Duration) -> Result<(), DiagnosticError> {
        let deadline = Instant::now() + duration;
        loop {
            self.check_signal()?;
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Ok(());
            }
            thread::sleep(remaining.min(IO_POLL_INTERVAL));
        }
    }

    fn request(
        &mut self,
        output: &mut dyn Write,
        command: DiagnosticCommand,
        pattern: Option<DiagnosticPattern>,
        variant: Option<u8>,
        expect_refresh: bool,
        context: &Context,
    ) -> Result<Option<DiagnosticSessionMetadata>, DiagnosticError> {
        let sequence = self.next_sequence();
        let payload = encode_diagnostic_command(command, pattern, variant)?;
        let sent_at_ns = monotonic_ns()?;
        let encoded = encode_frame(FrameType::ControlRequest.as_u8(), &payload, sequence, 0)?;
        self.connection()?.write_all(&encoded)?;

        let deadline = Instant::now() + self.config.command_timeout;
        let mut response_metadata = None;
        let mut phases = Vec::with_capacity(2);
        let mut presented_at_us = None;
        loop {
            let frame = self.next_frame(deadline)?;
            let received_at_ns = monotonic_ns()?;
            if frame.sequence != sequence {
                return Err(message(format!(
                    "diagnostic response sequence {} does not match request {sequence}",
                    frame.sequence
                )));
            }
            if frame.frame_type == FrameType::ControlResponse.as_u8() {
                let response = decode_diagnostic_response(&frame.payload)?;
                if response.schema != 1 || response.command != command.as_u8() {
                    return Err(message("diagnostic status does not match its request"));
                }
                let record = with_context(
                    object(json!({
                        "record": "response",
                        "sequence": sequence,
                        "command": command.as_u8(),
                        "status": response.status,
                        "error": response.error,
                        "host_sent_ns": sent_at_ns,
                        "host_received_ns": received_at_ns,
                    })),
                    context,
                );
                if command != DiagnosticCommand::SessionInfo {
                    write_record(output, &record)?;
                }
                if response.status != DiagnosticStatus::Accepted as u8 {
                    return Err(message(format!(
                        "diagnostic command {:?} was rejected (error {})",
                        command, response.error
                    )));
                }
                response_metadata = response.metadata;
                if !expect_refresh {
                    return Ok(response_metadata);
                }
                continue;
            }
            if frame.frame_type == FrameType::RefreshEvent.as_u8() {
                let event = decode_diagnostic_refresh_event(&frame.payload)?;
                if event.schema != 1
                    || event.command != command.as_u8()
                    || event.first_sequence != sequence
                    || event.last_sequence != sequence
                    || event.coalesced != 1
                {
                    return Err(message("diagnostic refresh does not match its request"));
                }
                let mut record = with_context(
                    object(json!({
                        "record": "refresh",
                        "sequence": sequence,
                        "phase": event.phase,
                        "command": event.command,
                        "requested_path": event.requested_path,
                        "actual_path": event.actual_path,
                        "fallback_reason": event.fallback_reason,
                        "flags": event.flags,
                        "queue_depth": event.queue_depth,
                        "host_received_ns": received_at_ns,
                    })),
                    context,
                );
                append_refresh_values(&mut record, &event);
                write_record(output, &record)?;
                phases.push(event.phase);
                if event.phase == DiagnosticEventPhase::Presented as u8 {
                    presented_at_us = Some(event.timestamp_us);
                }
                if phases
                    == [
                        DiagnosticEventPhase::Presented as u8,
                        DiagnosticEventPhase::Ready as u8,
                    ]
                {
                    let presented = presented_at_us
                        .ok_or_else(|| message("diagnostic PRESENTED timestamp is missing"))?;
                    if !u32_before_or_equal(presented, event.timestamp_us) {
                        return Err(message("diagnostic READY timestamp precedes PRESENTED"));
                    }
                    return Ok(response_metadata);
                }
                if phases != [DiagnosticEventPhase::Presented as u8] {
                    return Err(message("diagnostic refresh events arrived out of order"));
                }
                continue;
            }
            if frame.frame_type == FrameType::Heartbeat.as_u8()
                || is_optional_frame_type(frame.frame_type)
            {
                continue;
            }
            return Err(message(format!(
                "unexpected diagnostics frame type 0x{:02x}",
                frame.frame_type
            )));
        }
    }

    fn send_pattern_batch(
        &mut self,
        output: &mut dyn Write,
        requests: &[PatternRequest],
        interval: Duration,
        context: &Context,
    ) -> Result<(), DiagnosticError> {
        let mut sent = HashMap::new();
        let mut sequence_order = Vec::with_capacity(requests.len());
        let started_at = Instant::now();
        for (index, request) in requests.iter().enumerate() {
            let deadline = started_at + interval.saturating_mul(index as u32);
            self.sleep_interruptible(deadline.saturating_duration_since(Instant::now()))?;
            let sequence = self.next_sequence();
            let sent_at_ns = monotonic_ns()?;
            let payload = encode_diagnostic_command(
                DiagnosticCommand::Pattern,
                Some(request.pattern),
                Some(request.variant),
            )?;
            let encoded = encode_frame(FrameType::ControlRequest.as_u8(), &payload, sequence, 0)?;
            self.connection()?.write_all(&encoded)?;
            let sample_index = request
                .context
                .get("sample_index")
                .and_then(Value::as_u64)
                .and_then(|value| u8::try_from(value).ok())
                .ok_or_else(|| message("cadence request is missing its sample index"))?;
            sent.insert(
                sequence,
                SentRequest {
                    sent_at_ns,
                    sample_index,
                },
            );
            sequence_order.push(sequence);
        }

        let sequence_index: HashMap<_, _> = sequence_order
            .iter()
            .enumerate()
            .map(|(index, sequence)| (*sequence, index))
            .collect();
        let mut responses = HashSet::new();
        let mut covered = HashSet::new();
        let mut phases: HashMap<(u32, u32), Vec<u8>> = HashMap::new();
        let mut presented_at_us = HashMap::new();
        let deadline = Instant::now() + self.config.command_timeout;
        while responses.len() != sequence_order.len() || covered.len() != sequence_order.len() {
            let frame = self.next_frame(deadline)?;
            let received_at_ns = monotonic_ns()?;
            if frame.frame_type == FrameType::ControlResponse.as_u8() {
                let request = sent.get(&frame.sequence).ok_or_else(|| {
                    message(format!(
                        "unexpected diagnostic batch response sequence {}",
                        frame.sequence
                    ))
                })?;
                if responses.contains(&frame.sequence) {
                    return Err(message(format!(
                        "unexpected diagnostic batch response sequence {}",
                        frame.sequence
                    )));
                }
                let response = decode_diagnostic_response(&frame.payload)?;
                if response.schema != 1 || response.command != DiagnosticCommand::Pattern.as_u8() {
                    return Err(message(
                        "diagnostic batch status does not match its request",
                    ));
                }
                let request_context = &requests[*sequence_index
                    .get(&frame.sequence)
                    .expect("sent sequence has an index")]
                .context;
                let record = with_context(
                    with_context(
                        object(json!({
                            "record": "response",
                            "sequence": frame.sequence,
                            "command": DiagnosticCommand::Pattern.as_u8(),
                            "status": response.status,
                            "error": response.error,
                            "host_sent_ns": request.sent_at_ns,
                            "host_received_ns": received_at_ns,
                        })),
                        context,
                    ),
                    request_context,
                );
                write_record(output, &record)?;
                if response.status != DiagnosticStatus::Accepted as u8 {
                    return Err(message(format!(
                        "diagnostic batch sequence {} was rejected (error {})",
                        frame.sequence, response.error
                    )));
                }
                responses.insert(frame.sequence);
                continue;
            }
            if frame.frame_type == FrameType::RefreshEvent.as_u8() {
                let event = decode_diagnostic_refresh_event(&frame.payload)?;
                let first_index = sequence_index.get(&event.first_sequence).copied();
                let last_index = sequence_index.get(&event.last_sequence).copied();
                let (Some(first_index), Some(last_index)) = (first_index, last_index) else {
                    return Err(message(
                        "diagnostic batch event has an invalid sequence range",
                    ));
                };
                if frame.sequence != event.last_sequence || first_index > last_index {
                    return Err(message(
                        "diagnostic batch event has an invalid sequence range",
                    ));
                }
                let event_sequences = &sequence_order[first_index..=last_index];
                if event.schema != 1
                    || event.command != DiagnosticCommand::Pattern.as_u8()
                    || usize::from(event.coalesced) != event_sequences.len()
                    || usize::from(event.queue_depth) != event_sequences.len() - 1
                {
                    return Err(message(
                        "diagnostic batch event has inconsistent coalescing metadata",
                    ));
                }
                let key = (event.first_sequence, event.last_sequence);
                let event_phases = phases.entry(key).or_default();
                event_phases.push(event.phase);
                if event.phase == DiagnosticEventPhase::Presented as u8 {
                    presented_at_us.insert(key, event.timestamp_us);
                } else if event.phase == DiagnosticEventPhase::Ready as u8 {
                    if event_phases
                        != &[
                            DiagnosticEventPhase::Presented as u8,
                            DiagnosticEventPhase::Ready as u8,
                        ]
                    {
                        return Err(message(
                            "diagnostic batch refresh events arrived out of order",
                        ));
                    }
                    let presented = *presented_at_us.get(&key).ok_or_else(|| {
                        message("diagnostic batch PRESENTED timestamp is missing")
                    })?;
                    if !u32_before_or_equal(presented, event.timestamp_us) {
                        return Err(message(
                            "diagnostic batch READY timestamp precedes PRESENTED",
                        ));
                    }
                    if event_sequences
                        .iter()
                        .any(|sequence| covered.contains(sequence))
                    {
                        return Err(message("diagnostic batch refresh ranges overlap"));
                    }
                    covered.extend(event_sequences.iter().copied());
                } else {
                    return Err(message("diagnostic batch returned a failed refresh event"));
                }
                let first_sent = sent
                    .get(&event.first_sequence)
                    .expect("event first sequence was sent");
                let last_sent = sent
                    .get(&event.last_sequence)
                    .expect("event last sequence was sent");
                let mut record = with_context(
                    object(json!({
                        "record": "refresh",
                        "sequence": frame.sequence,
                        "phase": event.phase,
                        "command": event.command,
                        "requested_path": event.requested_path,
                        "actual_path": event.actual_path,
                        "fallback_reason": event.fallback_reason,
                        "flags": event.flags,
                        "queue_depth": event.queue_depth,
                        "first_host_sent_ns": first_sent.sent_at_ns,
                        "last_host_sent_ns": last_sent.sent_at_ns,
                        "first_sample_index": first_sent.sample_index,
                        "last_sample_index": last_sent.sample_index,
                        "host_received_ns": received_at_ns,
                    })),
                    context,
                );
                append_refresh_values(&mut record, &event);
                write_record(output, &record)?;
                continue;
            }
            if frame.frame_type == FrameType::Heartbeat.as_u8()
                || is_optional_frame_type(frame.frame_type)
            {
                continue;
            }
            return Err(message(format!(
                "unexpected diagnostics frame type 0x{:02x}",
                frame.frame_type
            )));
        }
        Ok(())
    }

    fn run_smoke(&mut self, output: &mut dyn Write) -> Result<(), DiagnosticError> {
        let commands = [
            (DiagnosticCommand::Reset, None, None),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Cell),
                Some(0),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Cell),
                Some(1),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Cursor),
                Some(1),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Cursor),
                Some(0),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Row),
                Some(0),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::DisjointRows),
                Some(1),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Scroll),
                Some(0),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Checker),
                Some(0),
            ),
            (
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Full),
                Some(0),
            ),
            (DiagnosticCommand::SetPolarity, None, Some(1)),
            (DiagnosticCommand::SetPolarity, None, Some(0)),
            (DiagnosticCommand::Clean, None, None),
        ];
        for (command, pattern, variant) in commands {
            self.log(match pattern {
                Some(pattern) => format!("running {command:?}/{pattern:?}/{}", variant.unwrap()),
                None => format!("running {command:?}"),
            });
            self.request(output, command, pattern, variant, true, &Context::new())?;
        }
        Ok(())
    }

    fn run_latency(
        &mut self,
        output: &mut dyn Write,
        repetitions: u8,
    ) -> Result<(), DiagnosticError> {
        let patterns = [
            ("cell_top", DiagnosticPattern::Cell),
            ("cell_middle", DiagnosticPattern::CellMiddle),
            ("cell_bottom", DiagnosticPattern::CellBottom),
            ("adjacent_cells", DiagnosticPattern::AdjacentCells),
            ("cursor", DiagnosticPattern::Cursor),
            ("row", DiagnosticPattern::Row),
            ("disjoint_rows", DiagnosticPattern::DisjointRows),
            ("scroll", DiagnosticPattern::Scroll),
            ("boundary_under_8k", DiagnosticPattern::BoundaryUnder),
            ("boundary_over_8k", DiagnosticPattern::BoundaryOver),
            ("checker", DiagnosticPattern::Checker),
            ("full", DiagnosticPattern::Full),
        ];
        self.request(
            output,
            DiagnosticCommand::Reset,
            None,
            None,
            true,
            &object(json!({"case": "setup_reset"})),
        )?;
        let mut polarity = 0_u8;
        for repetition in 0..repetitions {
            let requested_polarity = repetition & 1;
            if requested_polarity != polarity {
                self.log(format_args!(
                    "latency repetition {}: polarity {requested_polarity}",
                    repetition + 1
                ));
                self.request(
                    output,
                    DiagnosticCommand::SetPolarity,
                    None,
                    Some(requested_polarity),
                    true,
                    &object(json!({
                        "case": "setup_polarity",
                        "repetition": repetition + 1,
                    })),
                )?;
                polarity = requested_polarity;
            }
            let pattern_order: Box<dyn Iterator<Item = &(&'static str, DiagnosticPattern)>> =
                if repetition % 2 == 0 {
                    Box::new(patterns.iter())
                } else {
                    Box::new(patterns.iter().rev())
                };
            for (case, pattern) in pattern_order {
                for variant in [1_u8, 0] {
                    let direction = if (variant == 1) != (polarity != 0) {
                        "white_to_black"
                    } else {
                        "black_to_white"
                    };
                    self.log(format_args!(
                        "latency {}/{}: {case} {direction}",
                        repetition + 1,
                        repetitions
                    ));
                    self.request(
                        output,
                        DiagnosticCommand::Pattern,
                        Some(*pattern),
                        Some(variant),
                        true,
                        &object(json!({
                            "case": case,
                            "pattern": pattern.as_u8(),
                            "variant": variant,
                            "direction": direction,
                            "repetition": repetition + 1,
                        })),
                    )?;
                }
            }
        }
        if polarity != 0 {
            self.request(
                output,
                DiagnosticCommand::SetPolarity,
                None,
                Some(0),
                true,
                &object(json!({"case": "restore_polarity"})),
            )?;
        }
        self.request(
            output,
            DiagnosticCommand::Clean,
            None,
            None,
            true,
            &object(json!({"case": "final_clean"})),
        )?;
        Ok(())
    }

    fn run_cadence(
        &mut self,
        output: &mut dyn Write,
        settle: Duration,
    ) -> Result<(), DiagnosticError> {
        self.request(
            output,
            DiagnosticCommand::Reset,
            None,
            None,
            true,
            &object(json!({"case": "setup_reset"})),
        )?;
        let mut next_variant = 1_u8;
        for polarity in [0_u8, 1] {
            if polarity != 0 {
                self.request(
                    output,
                    DiagnosticCommand::SetPolarity,
                    None,
                    Some(polarity),
                    true,
                    &object(json!({"case": "setup_polarity", "polarity": polarity})),
                )?;
            }
            let intervals: &[u64] = if polarity == 0 {
                &[600, 400, 200, 100, 50, 25]
            } else {
                &[25, 50, 100, 200, 400, 600]
            };
            for interval_ms in intervals {
                let mut requests = Vec::with_capacity(6);
                for sample_index in 1_u8..=6 {
                    let direction = if (next_variant == 1) != (polarity != 0) {
                        "white_to_black"
                    } else {
                        "black_to_white"
                    };
                    requests.push(PatternRequest {
                        pattern: DiagnosticPattern::CellMiddle,
                        variant: next_variant,
                        context: object(json!({
                            "sample_index": sample_index,
                            "variant": next_variant,
                            "direction": direction,
                        })),
                    });
                    next_variant = 1 - next_variant;
                }
                self.log(format_args!(
                    "cadence: polarity {polarity}, 6 updates at {interval_ms} ms"
                ));
                self.send_pattern_batch(
                    output,
                    &requests,
                    Duration::from_millis(*interval_ms),
                    &object(json!({
                        "case": "cadence",
                        "polarity": polarity,
                        "interval_ms": interval_ms,
                        "requested_count": requests.len(),
                    })),
                )?;
                self.sleep_interruptible(settle)?;
            }
        }
        self.request(
            output,
            DiagnosticCommand::SetPolarity,
            None,
            Some(0),
            true,
            &object(json!({"case": "restore_polarity"})),
        )?;
        self.request(
            output,
            DiagnosticCommand::Clean,
            None,
            None,
            true,
            &object(json!({"case": "final_clean"})),
        )?;
        Ok(())
    }

    fn run_burst(&mut self, output: &mut dyn Write) -> Result<(), DiagnosticError> {
        let patterns = [
            (1_u16, DiagnosticPattern::Burst1),
            (2, DiagnosticPattern::Burst2),
            (5, DiagnosticPattern::Burst5),
            (10, DiagnosticPattern::Burst10),
            (25, DiagnosticPattern::Burst25),
            (100, DiagnosticPattern::Burst100),
        ];
        self.request(
            output,
            DiagnosticCommand::Reset,
            None,
            None,
            true,
            &object(json!({"case": "setup_reset"})),
        )?;
        for polarity in [0_u8, 1] {
            if polarity != 0 {
                self.request(
                    output,
                    DiagnosticCommand::SetPolarity,
                    None,
                    Some(polarity),
                    true,
                    &object(json!({"case": "setup_polarity", "polarity": polarity})),
                )?;
            }
            let pattern_order: Box<dyn Iterator<Item = &(u16, DiagnosticPattern)>> =
                if polarity == 0 {
                    Box::new(patterns.iter())
                } else {
                    Box::new(patterns.iter().rev())
                };
            for (requested_cells, pattern) in pattern_order {
                for variant in [1_u8, 0] {
                    let direction = if (variant == 1) != (polarity != 0) {
                        "white_to_black"
                    } else {
                        "black_to_white"
                    };
                    self.log(format_args!(
                        "burst: polarity {polarity}, {requested_cells} cells {direction}"
                    ));
                    self.request(
                        output,
                        DiagnosticCommand::Pattern,
                        Some(*pattern),
                        Some(variant),
                        true,
                        &object(json!({
                            "case": "burst",
                            "polarity": polarity,
                            "requested_cells": requested_cells,
                            "pattern": pattern.as_u8(),
                            "variant": variant,
                            "direction": direction,
                        })),
                    )?;
                }
            }
        }
        self.request(
            output,
            DiagnosticCommand::SetPolarity,
            None,
            Some(0),
            true,
            &object(json!({"case": "restore_polarity"})),
        )?;
        self.request(
            output,
            DiagnosticCommand::Clean,
            None,
            None,
            true,
            &object(json!({"case": "final_clean"})),
        )?;
        Ok(())
    }

    pub fn run_suite(&mut self) -> Result<i32, DiagnosticError> {
        if !(1..=3).contains(&self.config.repetitions) {
            return Err(message("diagnostic repetitions must be between 1 and 3"));
        }
        let accepted = self.connect()?;
        if let Some(parent) = self
            .config
            .output
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
        {
            fs::create_dir_all(parent)?;
        }
        let file = File::create(&self.config.output)?;
        let mut output = BufWriter::new(file);
        let metadata = self
            .request(
                &mut output,
                DiagnosticCommand::SessionInfo,
                None,
                None,
                false,
                &Context::new(),
            )?
            .ok_or_else(|| message("diagnostic session response omitted metadata"))?;
        let (host_os, host_release) = host_platform()?;
        let mut session_record = object(json!({
            "record": "session",
            "schema": 1,
            "suite": self.config.suite.as_str(),
            "suite_version": 1,
            "repetitions": if self.config.suite == DiagnosticSuite::Latency {
                self.config.repetitions
            } else {
                1
            },
            "target": self.target_label,
            "host_os": host_os,
            "host_release": host_release,
            "columns": accepted.cols,
            "rows": accepted.rows,
        }));
        session_record.extend(session_metadata_values(&metadata));
        write_record(&mut output, &session_record)?;

        match self.config.suite {
            DiagnosticSuite::Smoke => self.run_smoke(&mut output)?,
            DiagnosticSuite::Latency => self.run_latency(&mut output, self.config.repetitions)?,
            DiagnosticSuite::Cadence => self.run_cadence(&mut output, self.config.settle)?,
            DiagnosticSuite::Burst => self.run_burst(&mut output)?,
        }
        self.request(
            &mut output,
            DiagnosticCommand::Stop,
            None,
            None,
            false,
            &Context::new(),
        )?;
        output.flush()?;
        self.log(format_args!("wrote {}", self.config.output.display()));
        Ok(0)
    }
}

pub fn run_diagnostics(
    config: DiagnosticsConfig,
    signals: &ShutdownSignals,
) -> Result<i32, DiagnosticError> {
    DiagnosticClient::new(config, signals).run_suite()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEMP_FILE_COUNTER: AtomicU64 = AtomicU64::new(0);

    fn temporary_output(name: &str) -> PathBuf {
        let counter = TEMP_FILE_COUNTER.fetch_add(1, Ordering::Relaxed);
        std::env::temp_dir().join(format!(
            "knietty-rust-{name}-{}-{counter}.jsonl",
            std::process::id()
        ))
    }

    fn read_line(stream: &TcpStream) -> String {
        let mut line = String::new();
        BufReader::new(stream.try_clone().unwrap())
            .read_line(&mut line)
            .unwrap();
        line
    }

    fn session_response() -> Vec<u8> {
        let mut payload = vec![
            1,
            DiagnosticCommand::SessionInfo.as_u8(),
            DiagnosticStatus::Accepted as u8,
            0,
            4,
            20,
            DiagnosticSessionMetadata::FLAG_WAVEFORM_100MS
                | DiagnosticSessionMetadata::FLAG_RAM_PING_PONG,
            1,
            4,
            7,
            72,
            (-55_i8) as u8,
            80,
            24,
            2,
        ];
        payload.extend_from_slice(&800_u16.to_be_bytes());
        payload.extend_from_slice(&480_u16.to_be_bytes());
        payload.extend_from_slice(&50_000_u32.to_be_bytes());
        payload.extend_from_slice(&49_000_u32.to_be_bytes());
        payload.push(8);
        payload.extend_from_slice(b"deadbeef");
        payload.push(4);
        payload.extend_from_slice(b"cafe");
        payload
    }

    fn status_response(command: u8) -> Vec<u8> {
        vec![1, command, DiagnosticStatus::Accepted as u8, 0]
    }

    fn refresh_event(
        command: u8,
        phase: u8,
        timestamp_us: u32,
        first_sequence: u32,
        last_sequence: u32,
        coalesced: u8,
    ) -> Vec<u8> {
        let mut payload = vec![1, phase, command, 1, 1, 0, 2, coalesced - 1];
        for value in [timestamp_us, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15] {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        for value in [0_u16, 0, 8, 18, 0, 0, 8, 18] {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        payload.extend_from_slice(&144_u32.to_be_bytes());
        payload.extend_from_slice(&1_u16.to_be_bytes());
        payload.push(1);
        payload.push(coalesced);
        payload.extend_from_slice(&first_sequence.to_be_bytes());
        payload.extend_from_slice(&last_sequence.to_be_bytes());
        payload.extend_from_slice(&50_000_u32.to_be_bytes());
        payload.extend_from_slice(&49_000_u32.to_be_bytes());
        assert_eq!(payload.len(), crate::protocol::REFRESH_EVENT_SIZE);
        payload
    }

    fn next_wire_frame(stream: &mut TcpStream) -> Frame {
        let mut header = [0_u8; crate::protocol::FRAME_HEADER_SIZE];
        stream.read_exact(&mut header).unwrap();
        let payload_length = u16::from_be_bytes([header[2], header[3]]) as usize;
        let mut payload = vec![0_u8; payload_length];
        stream.read_exact(&mut payload).unwrap();
        Frame {
            frame_type: header[0],
            flags: header[1],
            sequence: u32::from_be_bytes([header[4], header[5], header[6], header[7]]),
            payload,
        }
    }

    fn send_frame(stream: &mut TcpStream, frame_type: FrameType, sequence: u32, payload: &[u8]) {
        stream
            .write_all(&encode_frame(frame_type.as_u8(), payload, sequence, 0).unwrap())
            .unwrap();
    }

    #[test]
    fn full_smoke_suite_writes_frozen_jsonl_schema_and_stops() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let address = listener.local_addr().unwrap();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            assert!(read_line(&stream).starts_with("KNIETTY/3 HELLO diagnostics frame,diag1 "));
            stream
                .write_all(b"KNIETTY/3 ACCEPT 80 24 frame,diag1\n")
                .unwrap();
            let pair_commit = next_wire_frame(&mut stream);
            assert_eq!(pair_commit.frame_type, FrameType::Heartbeat.as_u8());
            assert_eq!(pair_commit.sequence, 0);
            assert!(pair_commit.payload.is_empty());
            let expected_commands = [
                DiagnosticCommand::SessionInfo,
                DiagnosticCommand::Reset,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::Pattern,
                DiagnosticCommand::SetPolarity,
                DiagnosticCommand::SetPolarity,
                DiagnosticCommand::Clean,
                DiagnosticCommand::Stop,
            ];
            for (index, command) in expected_commands.into_iter().enumerate() {
                let frame = next_wire_frame(&mut stream);
                assert_eq!(frame.frame_type, FrameType::ControlRequest.as_u8());
                assert_eq!(frame.sequence, index as u32 + 1);
                assert_eq!(frame.payload[0], command.as_u8());
                let response = if command == DiagnosticCommand::SessionInfo {
                    session_response()
                } else {
                    status_response(command.as_u8())
                };
                send_frame(
                    &mut stream,
                    FrameType::ControlResponse,
                    frame.sequence,
                    &response,
                );
                if !matches!(
                    command,
                    DiagnosticCommand::SessionInfo | DiagnosticCommand::Stop
                ) {
                    send_frame(
                        &mut stream,
                        FrameType::RefreshEvent,
                        frame.sequence,
                        &refresh_event(
                            command.as_u8(),
                            DiagnosticEventPhase::Presented as u8,
                            0xffff_fff0,
                            frame.sequence,
                            frame.sequence,
                            1,
                        ),
                    );
                    send_frame(
                        &mut stream,
                        FrameType::RefreshEvent,
                        frame.sequence,
                        &refresh_event(
                            command.as_u8(),
                            DiagnosticEventPhase::Ready as u8,
                            0x10,
                            frame.sequence,
                            frame.sequence,
                            1,
                        ),
                    );
                }
            }
        });

        let output = temporary_output("smoke");
        let signals = ShutdownSignals::install().unwrap();
        let result = run_diagnostics(
            DiagnosticsConfig {
                host: address.ip().to_string(),
                port: address.port(),
                suite: DiagnosticSuite::Smoke,
                output: output.clone(),
                repetitions: 3,
                settle: Duration::ZERO,
                discovery_timeout: Duration::from_millis(50),
                approval_timeout: Duration::from_secs(2),
                command_timeout: Duration::from_secs(2),
                verbose: false,
                security: SecurityMode::InsecurePlaintext,
            },
            &signals,
        );
        assert_eq!(result.unwrap(), 0);
        server.join().unwrap();

        let contents = fs::read_to_string(&output).unwrap();
        fs::remove_file(&output).unwrap();
        let records: Vec<Value> = contents
            .lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect();
        assert_eq!(records.len(), 41);
        assert_eq!(records[0]["record"], "session");
        assert_eq!(records[0]["suite"], "smoke");
        assert_eq!(records[0]["profile"], 4);
        assert_eq!(records[0]["waveform_100ms"], true);
        assert_eq!(records[0]["ram_ping_pong"], true);
        let expected_host_os = match std::env::consts::OS {
            "macos" => "Darwin",
            "linux" => "Linux",
            other => other,
        };
        assert_eq!(records[0]["host_os"], expected_host_os);
        assert_eq!(records[1]["record"], "response");
        assert_eq!(records[2]["phase"], DiagnosticEventPhase::Presented as u8);
        assert_eq!(records[3]["phase"], DiagnosticEventPhase::Ready as u8);
        assert_eq!(
            records.last().unwrap()["command"],
            DiagnosticCommand::Stop.as_u8()
        );
        for line in contents.lines() {
            let value: Value = serde_json::from_str(line).unwrap();
            assert_eq!(serde_json::to_string(&value).unwrap(), line);
        }
    }

    #[test]
    fn cadence_batch_accepts_one_coalesced_refresh_range() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let address = listener.local_addr().unwrap();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            let first = next_wire_frame(&mut stream);
            let second = next_wire_frame(&mut stream);
            for frame in [&first, &second] {
                send_frame(
                    &mut stream,
                    FrameType::ControlResponse,
                    frame.sequence,
                    &status_response(DiagnosticCommand::Pattern.as_u8()),
                );
            }
            for (phase, timestamp) in [
                (DiagnosticEventPhase::Presented as u8, 100),
                (DiagnosticEventPhase::Ready as u8, 110),
            ] {
                send_frame(
                    &mut stream,
                    FrameType::RefreshEvent,
                    second.sequence,
                    &refresh_event(
                        DiagnosticCommand::Pattern.as_u8(),
                        phase,
                        timestamp,
                        first.sequence,
                        second.sequence,
                        2,
                    ),
                );
            }
        });
        let connection = TcpStream::connect(address).unwrap();
        connection.set_read_timeout(Some(IO_POLL_INTERVAL)).unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut client = DiagnosticClient::new(
            DiagnosticsConfig {
                host: address.ip().to_string(),
                port: address.port(),
                command_timeout: Duration::from_secs(2),
                ..DiagnosticsConfig::default()
            },
            &signals,
        );
        client.connection = Some(TerminalStream::plain(connection));
        client.sequence = 7;
        let requests = [
            PatternRequest {
                pattern: DiagnosticPattern::CellMiddle,
                variant: 1,
                context: object(json!({"sample_index": 1, "variant": 1})),
            },
            PatternRequest {
                pattern: DiagnosticPattern::CellMiddle,
                variant: 0,
                context: object(json!({"sample_index": 2, "variant": 0})),
            },
        ];
        let mut output = Vec::new();
        client
            .send_pattern_batch(
                &mut output,
                &requests,
                Duration::ZERO,
                &object(json!({"interval_ms": 25})),
            )
            .unwrap();
        server.join().unwrap();
        let records: Vec<Value> = String::from_utf8(output)
            .unwrap()
            .lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect();
        assert_eq!(records.len(), 4);
        assert_eq!(records[2]["record"], "refresh");
        assert_eq!(records[3]["coalesced"], 2);
        assert_eq!(records[3]["first_sample_index"], 1);
        assert_eq!(records[3]["last_sample_index"], 2);
    }

    #[test]
    fn diagnostics_require_physical_acceptance_and_diag1_capability() {
        for (response, expected) in [
            (
                b"KNIETTY/3 DENY\n".as_slice(),
                "connection denied on the X4",
            ),
            (
                b"KNIETTY/3 ACCEPT 80 24 frame\n".as_slice(),
                "X4 does not advertise diagnostics protocol diag1",
            ),
        ] {
            let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
            let address = listener.local_addr().unwrap();
            let response = response.to_vec();
            let server = thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                assert!(read_line(&stream).starts_with("KNIETTY/3 HELLO diagnostics "));
                stream.write_all(&response).unwrap();
            });
            let signals = ShutdownSignals::install().unwrap();
            let mut client = DiagnosticClient::new(
                DiagnosticsConfig {
                    host: address.ip().to_string(),
                    port: address.port(),
                    approval_timeout: Duration::from_secs(2),
                    security: SecurityMode::InsecurePlaintext,
                    ..DiagnosticsConfig::default()
                },
                &signals,
            );
            assert_eq!(client.connect().unwrap_err().to_string(), expected);
            server.join().unwrap();
        }
    }

    #[test]
    fn rejected_command_is_recorded_before_the_suite_fails() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let address = listener.local_addr().unwrap();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            let frame = next_wire_frame(&mut stream);
            send_frame(
                &mut stream,
                FrameType::ControlResponse,
                frame.sequence,
                &[
                    1,
                    DiagnosticCommand::Reset.as_u8(),
                    DiagnosticStatus::Rejected as u8,
                    7,
                ],
            );
        });
        let connection = TcpStream::connect(address).unwrap();
        connection.set_read_timeout(Some(IO_POLL_INTERVAL)).unwrap();
        let signals = ShutdownSignals::install().unwrap();
        let mut client = DiagnosticClient::new(
            DiagnosticsConfig {
                command_timeout: Duration::from_secs(2),
                ..DiagnosticsConfig::default()
            },
            &signals,
        );
        client.connection = Some(TerminalStream::plain(connection));
        let mut output = Vec::new();
        let error = client
            .request(
                &mut output,
                DiagnosticCommand::Reset,
                None,
                None,
                true,
                &Context::new(),
            )
            .unwrap_err();
        assert!(error.to_string().contains("was rejected (error 7)"));
        let record: Value = serde_json::from_slice(&output).unwrap();
        assert_eq!(record["status"], DiagnosticStatus::Rejected as u8);
        assert_eq!(record["error"], 7);
        server.join().unwrap();
    }
}

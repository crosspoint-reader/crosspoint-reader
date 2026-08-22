use std::error::Error;
use std::fmt;
use std::fs::{self, DirBuilder, Permissions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{DirBuilderExt, FileTypeExt, MetadataExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use nix::unistd::Uid;
use serde_json::{json, Value};

const REQUEST_PREFIX: &str = "KNIETTY-CONTROL/1 ";
const REQUEST_LIMIT: usize = 128;
const RESPONSE_LIMIT: u64 = 4096;
const SERVER_REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
pub const DEFAULT_CLIENT_TIMEOUT: Duration = Duration::from_secs(20);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DisplayCommand {
    Status,
    Metrics,
    CleanDeferred,
    Clean,
    PolarityNormal,
    PolarityInverted,
}

impl DisplayCommand {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Status => "status",
            Self::Metrics => "metrics",
            Self::CleanDeferred | Self::Clean => "clean",
            Self::PolarityNormal => "polarity normal",
            Self::PolarityInverted => "polarity inverted",
        }
    }

    fn request_line(self) -> String {
        let command = match self {
            Self::CleanDeferred => "clean deferred",
            _ => self.as_str(),
        };
        format!("{REQUEST_PREFIX}{command}\n")
    }
}

fn parse_request(line: &[u8]) -> Result<DisplayCommand, &'static str> {
    let line = std::str::from_utf8(line).map_err(|_| "control request is not UTF-8")?;
    let command = line
        .strip_prefix(REQUEST_PREFIX)
        .ok_or("unsupported local control protocol")?;
    match command {
        "status" => Ok(DisplayCommand::Status),
        "metrics" => Ok(DisplayCommand::Metrics),
        "clean deferred" => Ok(DisplayCommand::CleanDeferred),
        "clean" => Ok(DisplayCommand::Clean),
        "polarity normal" => Ok(DisplayCommand::PolarityNormal),
        "polarity inverted" => Ok(DisplayCommand::PolarityInverted),
        _ => Err("unsupported display command"),
    }
}

fn sanitized_device_id(label: &str) -> String {
    let mut output = String::with_capacity(label.len().min(32));
    for character in label.chars().take(32) {
        if character.is_ascii_alphanumeric() || matches!(character, '-' | '_' | '.') {
            output.push(character);
        } else {
            output.push('_');
        }
    }
    if output.is_empty() {
        output.push_str("x4");
    }
    output
}

fn runtime_directory() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .unwrap_or_else(|| PathBuf::from(format!("/tmp/knietty-{}", Uid::effective().as_raw())))
        .join("knietty")
}

fn ensure_private_directory(path: &Path) -> io::Result<()> {
    if !path.exists() {
        DirBuilder::new().recursive(true).mode(0o700).create(path)?;
    }
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.file_type().is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("control runtime path {} is not a directory", path.display()),
        ));
    }
    if metadata.uid() != Uid::effective().as_raw() {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!(
                "control runtime directory {} has a different owner",
                path.display()
            ),
        ));
    }
    fs::set_permissions(path, Permissions::from_mode(0o700))
}

fn prepare_socket_path(path: &Path) -> io::Result<()> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if !metadata.file_type().is_socket() => Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            format!("refusing to replace non-socket path {}", path.display()),
        )),
        Ok(metadata) if metadata.uid() != Uid::effective().as_raw() => Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("control socket {} has a different owner", path.display()),
        )),
        Ok(_) => match UnixStream::connect(path) {
            Ok(_) => Err(io::Error::new(
                io::ErrorKind::AddrInUse,
                format!("another active bridge owns {}", path.display()),
            )),
            Err(_) => fs::remove_file(path),
        },
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error),
    }
}

struct LocalClient {
    stream: UnixStream,
    request: [u8; REQUEST_LIMIT],
    request_length: usize,
    dispatched: bool,
    deadline: Instant,
    response: Vec<u8>,
    response_offset: usize,
}

impl LocalClient {
    fn new(stream: UnixStream) -> io::Result<Self> {
        stream.set_nonblocking(true)?;
        Ok(Self {
            stream,
            request: [0; REQUEST_LIMIT],
            request_length: 0,
            dispatched: false,
            deadline: Instant::now() + SERVER_REQUEST_TIMEOUT,
            response: Vec::new(),
            response_offset: 0,
        })
    }

    fn queue_response(&mut self, response: Value) {
        self.response = serde_json::to_vec(&response)
            .unwrap_or_else(|_| b"{\"ok\":false,\"error\":\"could not encode response\"}".to_vec());
        self.response.push(b'\n');
        self.response_offset = 0;
    }

    fn queue_error(&mut self, message: impl fmt::Display) {
        self.queue_response(json!({"ok": false, "error": message.to_string()}));
    }

    fn flush_response(&mut self) -> bool {
        while self.response_offset < self.response.len() {
            match self.stream.write(&self.response[self.response_offset..]) {
                Ok(0) => return true,
                Ok(length) => self.response_offset += length,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return false,
                Err(_) => return true,
            }
        }
        !self.response.is_empty()
    }
}

pub struct ControlServer {
    listener: UnixListener,
    path: PathBuf,
    device_id: String,
    client: Option<LocalClient>,
}

impl ControlServer {
    pub fn bind(label: &str) -> io::Result<Self> {
        let directory = runtime_directory();
        ensure_private_directory(&directory)?;
        let device_id = sanitized_device_id(label);
        let path = directory.join(format!("{device_id}.sock"));
        prepare_socket_path(&path)?;
        let listener = UnixListener::bind(&path)?;
        listener.set_nonblocking(true)?;
        fs::set_permissions(&path, Permissions::from_mode(0o600))?;
        Ok(Self {
            listener,
            path,
            device_id,
            client: None,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn device_id(&self) -> &str {
        &self.device_id
    }

    pub fn poll_request(&mut self) -> io::Result<Option<DisplayCommand>> {
        if self
            .client
            .as_mut()
            .is_some_and(LocalClient::flush_response)
        {
            self.client = None;
        }
        if self.client.is_none() {
            match self.listener.accept() {
                Ok((stream, _)) => self.client = Some(LocalClient::new(stream)?),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(None),
                Err(error) => return Err(error),
            }
        }

        let client = self.client.as_mut().expect("accepted client is installed");
        if !client.response.is_empty() || client.dispatched {
            if client.response.is_empty() && Instant::now() >= client.deadline {
                client.queue_error("timed out waiting for the X4 display command");
            }
            return Ok(None);
        }

        loop {
            if client.request_length == client.request.len() {
                client.queue_error("local control request exceeds 128 bytes");
                return Ok(None);
            }
            match client
                .stream
                .read(&mut client.request[client.request_length..])
            {
                Ok(0) => {
                    client.queue_error("local control request ended before newline");
                    return Ok(None);
                }
                Ok(length) => {
                    client.request_length += length;
                    if let Some(newline) = client.request[..client.request_length]
                        .iter()
                        .position(|byte| *byte == b'\n')
                    {
                        if client.request[newline + 1..client.request_length]
                            .iter()
                            .any(|byte| !byte.is_ascii_whitespace())
                        {
                            client.queue_error("local control request contains trailing data");
                            return Ok(None);
                        }
                        let end = newline
                            - usize::from(newline > 0 && client.request[newline - 1] == b'\r');
                        match parse_request(&client.request[..end]) {
                            Ok(command) => {
                                client.dispatched = true;
                                client.deadline = Instant::now() + SERVER_REQUEST_TIMEOUT;
                                return Ok(Some(command));
                            }
                            Err(error) => {
                                client.queue_error(error);
                                return Ok(None);
                            }
                        }
                    }
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(None),
                Err(error) => {
                    client.queue_error(error);
                    return Ok(None);
                }
            }
        }
    }

    pub fn complete(&mut self, result: Value) {
        if let Some(client) = &mut self.client {
            client.queue_response(json!({"ok": true, "result": result}));
        }
        self.flush_client();
    }

    pub fn fail(&mut self, message: impl fmt::Display) {
        if let Some(client) = &mut self.client {
            client.queue_error(message);
        }
        self.flush_client();
    }

    fn flush_client(&mut self) {
        if self
            .client
            .as_mut()
            .is_some_and(LocalClient::flush_response)
        {
            self.client = None;
        }
    }
}

impl Drop for ControlServer {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

#[derive(Debug)]
pub struct ControlClientError(String);

impl fmt::Display for ControlClientError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl Error for ControlClientError {}

fn available_sockets(device: Option<&str>) -> Result<Vec<PathBuf>, ControlClientError> {
    let directory = runtime_directory();
    let entries = match fs::read_dir(&directory) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(error) => return Err(ControlClientError(error.to_string())),
    };
    let requested = device.map(sanitized_device_id);
    let mut sockets = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|error| ControlClientError(error.to_string()))?;
        let path = entry.path();
        let metadata =
            fs::symlink_metadata(&path).map_err(|error| ControlClientError(error.to_string()))?;
        if !metadata.file_type().is_socket()
            || metadata.uid() != Uid::effective().as_raw()
            || metadata.mode() & 0o077 != 0
        {
            continue;
        }
        let stem = path.file_stem().and_then(|value| value.to_str());
        if requested
            .as_deref()
            .map_or(true, |requested| stem == Some(requested))
        {
            sockets.push(path);
        }
    }
    sockets.sort();
    Ok(sockets)
}

pub fn invoke(
    command: DisplayCommand,
    device: Option<&str>,
    timeout: Duration,
) -> Result<Value, ControlClientError> {
    let sockets = available_sockets(device)?;
    let path = match sockets.as_slice() {
        [] => {
            return Err(ControlClientError(match device {
                Some(device) => format!("no active knietty bridge found for {device:?}"),
                None => {
                    "no active knietty bridge found; start `knietty --host auto` first".to_owned()
                }
            }))
        }
        [path] => path,
        _ => {
            let devices = sockets
                .iter()
                .filter_map(|path| path.file_stem()?.to_str())
                .collect::<Vec<_>>()
                .join(", ");
            return Err(ControlClientError(format!(
                "multiple active knietty bridges found ({devices}); pass --device"
            )));
        }
    };
    let mut stream =
        UnixStream::connect(path).map_err(|error| ControlClientError(error.to_string()))?;
    stream
        .set_read_timeout(Some(timeout))
        .map_err(|error| ControlClientError(error.to_string()))?;
    stream
        .set_write_timeout(Some(timeout))
        .map_err(|error| ControlClientError(error.to_string()))?;
    stream
        .write_all(command.request_line().as_bytes())
        .map_err(|error| ControlClientError(error.to_string()))?;

    let mut response = Vec::new();
    stream
        .take(RESPONSE_LIMIT + 1)
        .read_to_end(&mut response)
        .map_err(|error| ControlClientError(error.to_string()))?;
    if response.len() as u64 > RESPONSE_LIMIT {
        return Err(ControlClientError(
            "local control response exceeds 4096 bytes".to_owned(),
        ));
    }
    let response: Value =
        serde_json::from_slice(&response).map_err(|error| ControlClientError(error.to_string()))?;
    if response.get("ok").and_then(Value::as_bool) != Some(true) {
        return Err(ControlClientError(
            response
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("display command failed")
                .to_owned(),
        ));
    }
    response
        .get("result")
        .cloned()
        .ok_or_else(|| ControlClientError("local control response has no result".to_owned()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::thread;

    fn unique_label() -> String {
        static NEXT_ID: AtomicU64 = AtomicU64::new(1);
        format!(
            "control-test-{}-{}",
            std::process::id(),
            NEXT_ID.fetch_add(1, Ordering::Relaxed)
        )
    }

    #[test]
    fn parses_only_the_allowlisted_commands() {
        assert_eq!(
            parse_request(b"KNIETTY-CONTROL/1 status"),
            Ok(DisplayCommand::Status)
        );
        assert_eq!(
            parse_request(b"KNIETTY-CONTROL/1 metrics"),
            Ok(DisplayCommand::Metrics)
        );
        assert_eq!(
            parse_request(b"KNIETTY-CONTROL/1 clean"),
            Ok(DisplayCommand::Clean)
        );
        assert_eq!(
            parse_request(b"KNIETTY-CONTROL/1 clean deferred"),
            Ok(DisplayCommand::CleanDeferred)
        );
        assert_eq!(
            parse_request(b"KNIETTY-CONTROL/1 polarity inverted"),
            Ok(DisplayCommand::PolarityInverted)
        );
        assert!(parse_request(b"KNIETTY-CONTROL/1 register-write").is_err());
        assert!(parse_request(b"clean").is_err());
    }

    #[test]
    fn device_ids_are_bounded_and_path_safe() {
        assert_eq!(sanitized_device_id("knietty-9e54a0"), "knietty-9e54a0");
        assert_eq!(sanitized_device_id("../../x4 name"), ".._.._x4_name");
        assert_eq!(sanitized_device_id(""), "x4");
        assert!(sanitized_device_id(&"a".repeat(80)).len() <= 32);
    }

    #[test]
    fn private_socket_round_trip_waits_for_server_completion() {
        let mut server = ControlServer::bind(&unique_label()).unwrap();
        let path = server.path().to_owned();
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o077, 0);
        assert_eq!(
            fs::metadata(path.parent().unwrap())
                .unwrap()
                .permissions()
                .mode()
                & 0o077,
            0
        );

        let client = thread::spawn(move || {
            let mut stream = UnixStream::connect(path).unwrap();
            stream.write_all(b"KNIETTY-CONTROL/1 clean\n").unwrap();
            let mut response = String::new();
            stream.read_to_string(&mut response).unwrap();
            serde_json::from_str::<Value>(&response).unwrap()
        });

        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            if server.poll_request().unwrap() == Some(DisplayCommand::Clean) {
                break;
            }
            assert!(
                Instant::now() < deadline,
                "local command was not dispatched"
            );
            thread::sleep(Duration::from_millis(2));
        }
        server.complete(json!({"phase": "ready"}));
        let response = client.join().unwrap();
        assert_eq!(response["ok"], true);
        assert_eq!(response["result"]["phase"], "ready");
    }
}

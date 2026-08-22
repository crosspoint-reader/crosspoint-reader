use std::error::Error;
use std::fmt;
use std::io::{self, Read};
use std::net::TcpStream;

use chrono::Local;

pub const PROTOCOL_V1_PREFIX: &str = "KNIETTY/1";
pub const PROTOCOL_V2_PREFIX: &str = "KNIETTY/2";
pub const PROTOCOL_V3_PREFIX: &str = "KNIETTY/3";
pub const HANDSHAKE_LINE_LIMIT: usize = 128;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServerAccept {
    pub version: u8,
    pub cols: u16,
    pub rows: u16,
    pub capabilities: Vec<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum HandshakeError {
    VersionRejected,
    Denied,
    Busy,
    Disconnected,
    LineTooLong,
    NonAscii,
    Unexpected(String),
    InvalidGeometry(String),
    MissingFrameCapability,
}

impl fmt::Display for HandshakeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::VersionRejected => formatter.write_str("X4 rejected this protocol version"),
            Self::Denied => formatter.write_str("connection denied on the X4"),
            Self::Busy => formatter.write_str("X4 is already handling another host"),
            Self::Disconnected => formatter.write_str("X4 disconnected during handshake"),
            Self::LineTooLong => formatter.write_str("X4 handshake exceeded size limit"),
            Self::NonAscii => formatter.write_str("terminal returned a non-ASCII handshake"),
            Self::Unexpected(line) => write!(formatter, "unexpected terminal handshake: {line:?}"),
            Self::InvalidGeometry(line) => {
                write!(
                    formatter,
                    "invalid terminal geometry in handshake: {line:?}"
                )
            }
            Self::MissingFrameCapability => {
                formatter.write_str("X4 accepted protocol v3 without the frame capability")
            }
        }
    }
}

impl Error for HandshakeError {}

fn protocol_version(prefix: &str) -> Option<u8> {
    match prefix {
        PROTOCOL_V1_PREFIX => Some(1),
        PROTOCOL_V2_PREFIX => Some(2),
        PROTOCOL_V3_PREFIX => Some(3),
        _ => None,
    }
}

pub fn parse_server_accept(response: &[u8]) -> Result<ServerAccept, HandshakeError> {
    let line = std::str::from_utf8(response).map_err(|_| HandshakeError::NonAscii)?;
    if !line.is_ascii() {
        return Err(HandshakeError::NonAscii);
    }
    let line = line.trim();
    let fields: Vec<_> = line.split_whitespace().collect();
    let version = fields.first().and_then(|prefix| protocol_version(prefix));
    if let (Some(_), Some(status)) = (version, fields.get(1)) {
        match *status {
            "ERROR" => return Err(HandshakeError::VersionRejected),
            "DENY" => return Err(HandshakeError::Denied),
            "BUSY" => return Err(HandshakeError::Busy),
            _ => {}
        }
    }
    let Some(version) = version else {
        return Err(HandshakeError::Unexpected(line.to_owned()));
    };
    if fields.get(1) != Some(&"ACCEPT") {
        return Err(HandshakeError::Unexpected(line.to_owned()));
    }
    let expected_length = if version == 3 { 5 } else { 4 };
    if fields.len() != expected_length {
        return Err(HandshakeError::Unexpected(line.to_owned()));
    }
    let cols = fields[2]
        .parse::<u16>()
        .ok()
        .filter(|value| *value != 0)
        .ok_or_else(|| HandshakeError::InvalidGeometry(line.to_owned()))?;
    let rows = fields[3]
        .parse::<u16>()
        .ok()
        .filter(|value| *value != 0)
        .ok_or_else(|| HandshakeError::InvalidGeometry(line.to_owned()))?;
    let capabilities = if version == 3 {
        fields[4].split(',').map(str::to_owned).collect()
    } else {
        Vec::new()
    };
    if version == 3 && !capabilities.iter().any(|capability| capability == "frame") {
        return Err(HandshakeError::MissingFrameCapability);
    }
    Ok(ServerAccept {
        version,
        cols,
        rows,
        capabilities,
    })
}

pub fn read_protocol_line(
    connection: &mut TcpStream,
    limit: usize,
) -> Result<Vec<u8>, HandshakeReadError> {
    let mut response = Vec::with_capacity(limit.min(HANDSHAKE_LINE_LIMIT));
    let mut byte = [0_u8; 1];
    while response.len() < limit {
        match connection.read(&mut byte) {
            Ok(0) => return Err(HandshakeReadError::Protocol(HandshakeError::Disconnected)),
            Ok(_) => {
                response.push(byte[0]);
                if byte[0] == b'\n' {
                    return Ok(response);
                }
            }
            Err(error) => return Err(HandshakeReadError::Io(error)),
        }
    }
    Err(HandshakeReadError::Protocol(HandshakeError::LineTooLong))
}

#[derive(Debug)]
pub enum HandshakeReadError {
    Io(io::Error),
    Protocol(HandshakeError),
}

impl fmt::Display for HandshakeReadError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => error.fmt(formatter),
            Self::Protocol(error) => error.fmt(formatter),
        }
    }
}

impl Error for HandshakeReadError {}

pub fn protocol_client_name(hostname: Option<&str>) -> String {
    let hostname = hostname
        .map(str::to_owned)
        .or_else(|| {
            nix::unistd::gethostname()
                .ok()
                .and_then(|name| name.into_string().ok())
        })
        .filter(|name| !name.is_empty())
        .unwrap_or_else(|| "host".to_owned());
    let sanitized: String = hostname
        .chars()
        .map(|character| {
            if character.is_ascii_alphanumeric() || matches!(character, ' ' | '.' | '_' | '-') {
                character
            } else {
                '?'
            }
        })
        .take(32)
        .collect();
    if sanitized.is_empty() {
        "host".to_owned()
    } else {
        sanitized
    }
}

pub fn protocol_host_time() -> (i64, i32) {
    let now = Local::now();
    (now.timestamp(), now.offset().local_minus_utc() / 60)
}

pub fn terminal_hello(version: u8, client_name: &str, epoch: i64, offset_minutes: i32) -> String {
    match version {
        3 => format!(
            "{PROTOCOL_V3_PREFIX} HELLO terminal frame {epoch} {offset_minutes} {client_name}\n"
        ),
        2 => format!("{PROTOCOL_V2_PREFIX} HELLO {epoch} {offset_minutes} {client_name}\n"),
        1 => format!("{PROTOCOL_V1_PREFIX} HELLO {client_name}\n"),
        _ => unreachable!("only protocol versions 1 through 3 are supported"),
    }
}

pub fn diagnostics_hello(client_name: &str, epoch: i64, offset_minutes: i32) -> String {
    format!(
        "{PROTOCOL_V3_PREFIX} HELLO diagnostics frame,diag1 {epoch} {offset_minutes} {client_name}\n"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_acceptance_geometry_and_capabilities() {
        assert_eq!(
            parse_server_accept(b"KNIETTY/1 ACCEPT 50 22\n").unwrap(),
            ServerAccept {
                version: 1,
                cols: 50,
                rows: 22,
                capabilities: Vec::new(),
            }
        );
        assert_eq!(
            parse_server_accept(b"KNIETTY/3 ACCEPT 80 24 frame,diag1\n").unwrap(),
            ServerAccept {
                version: 3,
                cols: 80,
                rows: 24,
                capabilities: vec!["frame".to_owned(), "diag1".to_owned()],
            }
        );
        assert_eq!(
            parse_server_accept(b"KNIETTY/3 ACCEPT 80 24 frame,burst1\n")
                .unwrap()
                .capabilities,
            vec!["frame".to_owned(), "burst1".to_owned()]
        );
    }

    #[test]
    fn rejects_protocol_status_and_malformed_acceptance() {
        assert_eq!(
            parse_server_accept(b"KNIETTY/1 ERROR\n"),
            Err(HandshakeError::VersionRejected)
        );
        assert_eq!(
            parse_server_accept(b"KNIETTY/3 DENY\n"),
            Err(HandshakeError::Denied)
        );
        assert_eq!(
            parse_server_accept(b"KNIETTY/2 BUSY\n"),
            Err(HandshakeError::Busy)
        );
        assert_eq!(
            parse_server_accept(b"KNIETTY/3 ACCEPT 80 24 diag1\n"),
            Err(HandshakeError::MissingFrameCapability)
        );
        assert!(matches!(
            parse_server_accept(b"KNIETTY/3 ACCEPT 0 24 frame\n"),
            Err(HandshakeError::InvalidGeometry(_))
        ));
        assert!(matches!(
            parse_server_accept(b"hello\n"),
            Err(HandshakeError::Unexpected(_))
        ));
    }

    #[test]
    fn client_name_is_ascii_safe_and_bounded() {
        assert_eq!(
            protocol_client_name(Some("workstation/example:name-with-a-very-long-suffix")),
            "workstation?example?name-with-a-"
        );
        assert_eq!(protocol_client_name(Some("máquina")), "m?quina");
    }

    #[test]
    fn hello_lines_match_each_wire_version() {
        assert_eq!(
            terminal_hello(3, "host", 1_700_000_000, -180),
            "KNIETTY/3 HELLO terminal frame 1700000000 -180 host\n"
        );
        assert_eq!(
            terminal_hello(2, "host", 1_700_000_000, -180),
            "KNIETTY/2 HELLO 1700000000 -180 host\n"
        );
        assert_eq!(terminal_hello(1, "host", 0, 0), "KNIETTY/1 HELLO host\n");
        assert_eq!(
            diagnostics_hello("host", 1_700_000_000, -180),
            "KNIETTY/3 HELLO diagnostics frame,diag1 1700000000 -180 host\n"
        );
    }

    #[test]
    fn host_time_is_bounded_to_real_timezone_offsets() {
        let (epoch, offset) = protocol_host_time();
        assert!(epoch > 1_700_000_000);
        assert!((-14 * 60..=14 * 60).contains(&offset));
    }
}

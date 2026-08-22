use std::error::Error;
use std::fmt;

pub const FRAME_HEADER_SIZE: usize = 8;
pub const MAX_FRAME_PAYLOAD: usize = 512;
pub const OPTIONAL_TYPE_MASK: u8 = 0x80;
pub const REFRESH_EVENT_SIZE: usize = 108;
pub const METRICS_RESPONSE_V1_SIZE: usize = 84;
pub const METRICS_RESPONSE_SIZE: usize = 108;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum FrameType {
    TerminalOutput = 0x01,
    TerminalInput = 0x02,
    ControlRequest = 0x03,
    ControlResponse = 0x04,
    RefreshEvent = 0x05,
    Heartbeat = 0x06,
    SessionEnd = 0x07,
    TerminalOutputEnd = 0x80,
}

impl FrameType {
    pub const fn as_u8(self) -> u8 {
        self as u8
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum DiagnosticCommand {
    SessionInfo = 1,
    Reset = 2,
    Pattern = 3,
    SetPolarity = 4,
    Clean = 5,
    Stop = 6,
    Metrics = 7,
}

impl DiagnosticCommand {
    pub const fn as_u8(self) -> u8 {
        self as u8
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum DiagnosticPattern {
    Cell = 1,
    Cursor = 2,
    Row = 3,
    DisjointRows = 4,
    Scroll = 5,
    Checker = 6,
    Full = 7,
    CellMiddle = 8,
    CellBottom = 9,
    AdjacentCells = 10,
    BoundaryUnder = 11,
    BoundaryOver = 12,
    Burst1 = 13,
    Burst2 = 14,
    Burst5 = 15,
    Burst10 = 16,
    Burst25 = 17,
    Burst100 = 18,
}

impl DiagnosticPattern {
    pub const fn as_u8(self) -> u8 {
        self as u8
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum DiagnosticStatus {
    Accepted = 0,
    Rejected = 1,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum DiagnosticEventPhase {
    Presented = 1,
    Ready = 2,
    Failed = 3,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Frame {
    pub frame_type: u8,
    pub flags: u8,
    pub sequence: u32,
    pub payload: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ProtocolError {
    UnsupportedFlags(u8),
    PayloadTooLarge(usize),
    InvalidDiagnosticCommand(&'static str),
    TruncatedDiagnosticResponse,
    InvalidDiagnosticMetadata(&'static str),
    InvalidUtf8Metadata,
    TrailingDiagnosticData,
    InvalidRefreshEventSize(usize),
    UnexpectedFrameType(u8),
    UnknownMandatoryFrameType(u8),
}

impl fmt::Display for ProtocolError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedFlags(flags) => {
                write!(
                    formatter,
                    "protocol v3 frame has unsupported flags 0x{flags:02x}"
                )
            }
            Self::PayloadTooLarge(length) => write!(
                formatter,
                "protocol v3 frame payload is {length} bytes; maximum is {MAX_FRAME_PAYLOAD}"
            ),
            Self::InvalidDiagnosticCommand(message) => formatter.write_str(message),
            Self::TruncatedDiagnosticResponse => {
                formatter.write_str("diagnostic response is shorter than its status header")
            }
            Self::InvalidDiagnosticMetadata(message) => formatter.write_str(message),
            Self::InvalidUtf8Metadata => {
                formatter.write_str("diagnostic build revision is not UTF-8")
            }
            Self::TrailingDiagnosticData => {
                formatter.write_str("diagnostic status response has trailing bytes")
            }
            Self::InvalidRefreshEventSize(length) => write!(
                formatter,
                "diagnostic refresh event is {length} bytes; expected {REFRESH_EVENT_SIZE}"
            ),
            Self::UnexpectedFrameType(frame_type) => write!(
                formatter,
                "unexpected protocol v3 frame type 0x{frame_type:02x}"
            ),
            Self::UnknownMandatoryFrameType(frame_type) => write!(
                formatter,
                "unknown mandatory protocol v3 frame type 0x{frame_type:02x}"
            ),
        }
    }
}

impl Error for ProtocolError {}

pub const fn is_known_frame_type(frame_type: u8) -> bool {
    matches!(frame_type, 0x01..=0x07 | 0x80)
}

pub const fn is_optional_frame_type(frame_type: u8) -> bool {
    frame_type & OPTIONAL_TYPE_MASK != 0
}

pub fn encode_frame(
    frame_type: u8,
    payload: &[u8],
    sequence: u32,
    flags: u8,
) -> Result<Vec<u8>, ProtocolError> {
    if flags != 0 {
        return Err(ProtocolError::UnsupportedFlags(flags));
    }
    if payload.len() > MAX_FRAME_PAYLOAD {
        return Err(ProtocolError::PayloadTooLarge(payload.len()));
    }

    let mut encoded = Vec::with_capacity(FRAME_HEADER_SIZE + payload.len());
    encoded.push(frame_type);
    encoded.push(flags);
    encoded.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    encoded.extend_from_slice(&sequence.to_be_bytes());
    encoded.extend_from_slice(payload);
    Ok(encoded)
}

#[derive(Debug, Default)]
pub struct FrameDecoder {
    buffer: Vec<u8>,
}

impl FrameDecoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn reset(&mut self) {
        self.buffer.clear();
    }

    pub fn feed(&mut self, data: &[u8]) -> Result<Vec<Frame>, ProtocolError> {
        self.buffer.extend_from_slice(data);
        let mut frames = Vec::new();
        let mut consumed = 0;

        while self.buffer.len() - consumed >= FRAME_HEADER_SIZE {
            let header = &self.buffer[consumed..consumed + FRAME_HEADER_SIZE];
            let frame_type = header[0];
            let flags = header[1];
            if flags != 0 {
                return Err(ProtocolError::UnsupportedFlags(flags));
            }
            let payload_length = u16::from_be_bytes([header[2], header[3]]) as usize;
            if payload_length > MAX_FRAME_PAYLOAD {
                return Err(ProtocolError::PayloadTooLarge(payload_length));
            }
            let frame_size = FRAME_HEADER_SIZE + payload_length;
            if self.buffer.len() - consumed < frame_size {
                break;
            }
            let sequence = u32::from_be_bytes([header[4], header[5], header[6], header[7]]);
            let payload_start = consumed + FRAME_HEADER_SIZE;
            let payload_end = consumed + frame_size;
            frames.push(Frame {
                frame_type,
                flags,
                sequence,
                payload: self.buffer[payload_start..payload_end].to_vec(),
            });
            consumed += frame_size;
        }

        if consumed != 0 {
            self.buffer.drain(..consumed);
        }
        Ok(frames)
    }
}

pub fn encode_diagnostic_command(
    command: DiagnosticCommand,
    pattern: Option<DiagnosticPattern>,
    variant: Option<u8>,
) -> Result<Vec<u8>, ProtocolError> {
    match command {
        DiagnosticCommand::Pattern => match (pattern, variant) {
            (Some(pattern), Some(variant @ 0..=1)) => {
                Ok(vec![command.as_u8(), pattern.as_u8(), variant])
            }
            _ => Err(ProtocolError::InvalidDiagnosticCommand(
                "pattern commands require a named pattern and variant 0 or 1",
            )),
        },
        DiagnosticCommand::SetPolarity => match (pattern, variant) {
            (None, Some(variant @ 0..=1)) => Ok(vec![command.as_u8(), variant]),
            _ => Err(ProtocolError::InvalidDiagnosticCommand(
                "polarity commands require variant 0 or 1",
            )),
        },
        _ if pattern.is_some() || variant.is_some() => Err(
            ProtocolError::InvalidDiagnosticCommand("this diagnostic command takes no arguments"),
        ),
        _ => Ok(vec![command.as_u8()]),
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DiagnosticSessionMetadata {
    pub profile: u8,
    pub spi_mhz: u8,
    pub flags: u8,
    pub orientation: u8,
    pub board: u8,
    pub controller: u8,
    pub battery_percent: u8,
    pub rssi_dbm: i8,
    pub columns: u8,
    pub rows: u8,
    pub font: u8,
    pub display_width: u16,
    pub display_height: u16,
    pub free_heap: u32,
    pub minimum_free_heap: u32,
    pub build: String,
    pub freeink: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DiagnosticMetrics {
    pub updates: u32,
    pub windowed: u32,
    pub fallback: u32,
    pub settle: u32,
    pub clean: u32,
    pub last_total_us: u32,
    pub last_waveform_us: u32,
    pub last_queue_us: u32,
    pub last_render_us: u32,
    pub last_transfer_us: u32,
    pub last_plane_us: u32,
    pub last_lut_us: u32,
    pub last_baseline_us: u32,
    pub average_total_us: u32,
    pub minimum_total_us: u32,
    pub maximum_total_us: u32,
    pub last_region_width: u16,
    pub last_region_height: u16,
    pub last_region_bytes: u32,
    pub free_heap: u32,
    pub minimum_free_heap: u32,
    pub rx_bytes: u32,
    pub rx_reads: u32,
    pub burst_ends: u32,
    pub burst_snapshots: u32,
    pub burst_timeouts: u32,
    pub async_tail_updates: u32,
}

impl DiagnosticSessionMetadata {
    pub const FLAG_INVERTED: u8 = 0x01;
    pub const FLAG_FADING_FIX: u8 = 0x02;
    pub const FLAG_ADAPTIVE_REFRESH: u8 = 0x04;
    pub const FLAG_OVERCLOCKED_SPI: u8 = 0x08;
    pub const FLAG_WAVEFORM_100MS: u8 = 0x10;
    pub const FLAG_AUTO_SETTLE_DISABLED: u8 = 0x20;
    pub const FLAG_BALANCED_SUSTAIN: u8 = 0x40;
    pub const FLAG_RAM_PING_PONG: u8 = 0x80;

    pub const fn auto_settle(&self) -> bool {
        self.flags & Self::FLAG_AUTO_SETTLE_DISABLED == 0
    }

    pub const fn inverted(&self) -> bool {
        self.flags & Self::FLAG_INVERTED != 0
    }

    pub const fn fading_fix(&self) -> bool {
        self.flags & Self::FLAG_FADING_FIX != 0
    }

    pub const fn adaptive_refresh(&self) -> bool {
        self.flags & Self::FLAG_ADAPTIVE_REFRESH != 0
    }

    pub const fn overclocked_spi(&self) -> bool {
        self.flags & Self::FLAG_OVERCLOCKED_SPI != 0
    }

    pub const fn balanced_sustain(&self) -> bool {
        self.flags & Self::FLAG_BALANCED_SUSTAIN != 0
    }

    pub const fn waveform_100ms(&self) -> bool {
        self.flags & Self::FLAG_WAVEFORM_100MS != 0
    }

    pub const fn ram_ping_pong(&self) -> bool {
        self.flags & Self::FLAG_RAM_PING_PONG != 0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DiagnosticResponse {
    pub schema: u8,
    pub command: u8,
    pub status: u8,
    pub error: u8,
    pub metadata: Option<DiagnosticSessionMetadata>,
    pub metrics: Option<DiagnosticMetrics>,
}

fn read_u16(payload: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([payload[offset], payload[offset + 1]])
}

fn read_u32(payload: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        payload[offset],
        payload[offset + 1],
        payload[offset + 2],
        payload[offset + 3],
    ])
}

pub fn decode_diagnostic_response(payload: &[u8]) -> Result<DiagnosticResponse, ProtocolError> {
    if payload.len() < 4 {
        return Err(ProtocolError::TruncatedDiagnosticResponse);
    }
    let schema = payload[0];
    let command = payload[1];
    let status = payload[2];
    let error = payload[3];

    if status != DiagnosticStatus::Accepted as u8 {
        if payload.len() != 4 {
            return Err(ProtocolError::TrailingDiagnosticData);
        }
        return Ok(DiagnosticResponse {
            schema,
            command,
            status,
            error,
            metadata: None,
            metrics: None,
        });
    }

    if command == DiagnosticCommand::Metrics.as_u8() {
        if payload.len() != METRICS_RESPONSE_V1_SIZE && payload.len() != METRICS_RESPONSE_SIZE {
            return Err(ProtocolError::InvalidDiagnosticMetadata(
                "diagnostic metrics response has an invalid size",
            ));
        }
        return Ok(DiagnosticResponse {
            schema,
            command,
            status,
            error,
            metadata: None,
            metrics: Some(DiagnosticMetrics {
                updates: read_u32(payload, 4),
                windowed: read_u32(payload, 8),
                fallback: read_u32(payload, 12),
                settle: read_u32(payload, 16),
                clean: read_u32(payload, 20),
                last_total_us: read_u32(payload, 24),
                last_waveform_us: read_u32(payload, 28),
                last_queue_us: read_u32(payload, 32),
                last_render_us: read_u32(payload, 36),
                last_transfer_us: read_u32(payload, 40),
                last_plane_us: read_u32(payload, 44),
                last_lut_us: read_u32(payload, 48),
                last_baseline_us: read_u32(payload, 52),
                average_total_us: read_u32(payload, 56),
                minimum_total_us: read_u32(payload, 60),
                maximum_total_us: read_u32(payload, 64),
                last_region_width: read_u16(payload, 68),
                last_region_height: read_u16(payload, 70),
                last_region_bytes: read_u32(payload, 72),
                free_heap: read_u32(payload, 76),
                minimum_free_heap: read_u32(payload, 80),
                rx_bytes: payload.get(84..88).map_or(0, |bytes| {
                    u32::from_be_bytes(bytes.try_into().expect("metrics field is four bytes"))
                }),
                rx_reads: payload.get(88..92).map_or(0, |bytes| {
                    u32::from_be_bytes(bytes.try_into().expect("metrics field is four bytes"))
                }),
                burst_ends: payload.get(92..96).map_or(0, |bytes| {
                    u32::from_be_bytes(bytes.try_into().expect("metrics field is four bytes"))
                }),
                burst_snapshots: payload.get(96..100).map_or(0, |bytes| {
                    u32::from_be_bytes(bytes.try_into().expect("metrics field is four bytes"))
                }),
                burst_timeouts: payload.get(100..104).map_or(0, |bytes| {
                    u32::from_be_bytes(bytes.try_into().expect("metrics field is four bytes"))
                }),
                async_tail_updates: payload.get(104..108).map_or(0, |bytes| {
                    u32::from_be_bytes(bytes.try_into().expect("metrics field is four bytes"))
                }),
            }),
        });
    }

    if command != DiagnosticCommand::SessionInfo.as_u8() {
        if payload.len() != 4 {
            return Err(ProtocolError::TrailingDiagnosticData);
        }
        return Ok(DiagnosticResponse {
            schema,
            command,
            status,
            error,
            metadata: None,
            metrics: None,
        });
    }

    const FIXED_SIZE: usize = 28;
    if payload.len() < FIXED_SIZE {
        return Err(ProtocolError::InvalidDiagnosticMetadata(
            "diagnostic session metadata is truncated",
        ));
    }
    let build_length = payload[27] as usize;
    let build_start = FIXED_SIZE;
    let build_end = build_start + build_length;
    if build_end >= payload.len() {
        return Err(ProtocolError::InvalidDiagnosticMetadata(
            "diagnostic build revision length is invalid",
        ));
    }
    let freeink_length = payload[build_end] as usize;
    let freeink_start = build_end + 1;
    let freeink_end = freeink_start + freeink_length;
    if freeink_end != payload.len() {
        return Err(ProtocolError::InvalidDiagnosticMetadata(
            "diagnostic FreeInk revision length is invalid",
        ));
    }
    let build = std::str::from_utf8(&payload[build_start..build_end])
        .map_err(|_| ProtocolError::InvalidUtf8Metadata)?
        .to_owned();
    let freeink = std::str::from_utf8(&payload[freeink_start..freeink_end])
        .map_err(|_| ProtocolError::InvalidUtf8Metadata)?
        .to_owned();

    Ok(DiagnosticResponse {
        schema,
        command,
        status,
        error,
        metadata: Some(DiagnosticSessionMetadata {
            profile: payload[4],
            spi_mhz: payload[5],
            flags: payload[6],
            orientation: payload[7],
            board: payload[8],
            controller: payload[9],
            battery_percent: payload[10],
            rssi_dbm: payload[11] as i8,
            columns: payload[12],
            rows: payload[13],
            font: payload[14],
            display_width: read_u16(payload, 15),
            display_height: read_u16(payload, 17),
            free_heap: read_u32(payload, 19),
            minimum_free_heap: read_u32(payload, 23),
            build,
            freeink,
        }),
        metrics: None,
    })
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DiagnosticRefreshEvent {
    pub schema: u8,
    pub phase: u8,
    pub command: u8,
    pub requested_path: u8,
    pub actual_path: u8,
    pub fallback_reason: u8,
    pub flags: u8,
    pub queue_depth: u8,
    pub timestamp_us: u32,
    pub rx_at_us: u32,
    pub parsed_at_us: u32,
    pub queued_at_us: u32,
    pub render_started_at_us: u32,
    pub queue_us: u32,
    pub render_us: u32,
    pub transfer_us: u32,
    pub lut_us: u32,
    pub plane_us: u32,
    pub activation_to_busy_us: u32,
    pub waveform_us: u32,
    pub baseline_us: u32,
    pub power_off_us: u32,
    pub total_us: u32,
    pub logical_x: u16,
    pub logical_y: u16,
    pub logical_width: u16,
    pub logical_height: u16,
    pub aligned_x: u16,
    pub aligned_y: u16,
    pub aligned_width: u16,
    pub aligned_height: u16,
    pub transfer_bytes: u32,
    pub dirty_cells: u16,
    pub dirty_rows: u8,
    pub coalesced: u8,
    pub first_sequence: u32,
    pub last_sequence: u32,
    pub free_heap: u32,
    pub minimum_free_heap: u32,
}

pub fn decode_diagnostic_refresh_event(
    payload: &[u8],
) -> Result<DiagnosticRefreshEvent, ProtocolError> {
    if payload.len() != REFRESH_EVENT_SIZE {
        return Err(ProtocolError::InvalidRefreshEventSize(payload.len()));
    }

    let mut u32_offset = 8;
    let mut next_u32 = || {
        let value = read_u32(payload, u32_offset);
        u32_offset += 4;
        value
    };
    let timestamp_us = next_u32();
    let rx_at_us = next_u32();
    let parsed_at_us = next_u32();
    let queued_at_us = next_u32();
    let render_started_at_us = next_u32();
    let queue_us = next_u32();
    let render_us = next_u32();
    let transfer_us = next_u32();
    let lut_us = next_u32();
    let plane_us = next_u32();
    let activation_to_busy_us = next_u32();
    let waveform_us = next_u32();
    let baseline_us = next_u32();
    let power_off_us = next_u32();
    let total_us = next_u32();

    let mut u16_offset = u32_offset;
    let mut next_u16 = || {
        let value = read_u16(payload, u16_offset);
        u16_offset += 2;
        value
    };
    let logical_x = next_u16();
    let logical_y = next_u16();
    let logical_width = next_u16();
    let logical_height = next_u16();
    let aligned_x = next_u16();
    let aligned_y = next_u16();
    let aligned_width = next_u16();
    let aligned_height = next_u16();

    let transfer_bytes = read_u32(payload, u16_offset);
    let dirty_cells = read_u16(payload, u16_offset + 4);
    let dirty_rows = payload[u16_offset + 6];
    let coalesced = payload[u16_offset + 7];
    let first_sequence = read_u32(payload, u16_offset + 8);
    let last_sequence = read_u32(payload, u16_offset + 12);
    let free_heap = read_u32(payload, u16_offset + 16);
    let minimum_free_heap = read_u32(payload, u16_offset + 20);

    Ok(DiagnosticRefreshEvent {
        schema: payload[0],
        phase: payload[1],
        command: payload[2],
        requested_path: payload[3],
        actual_path: payload[4],
        fallback_reason: payload[5],
        flags: payload[6],
        queue_depth: payload[7],
        timestamp_us,
        rx_at_us,
        parsed_at_us,
        queued_at_us,
        render_started_at_us,
        queue_us,
        render_us,
        transfer_us,
        lut_us,
        plane_us,
        activation_to_busy_us,
        waveform_us,
        baseline_us,
        power_off_us,
        total_us,
        logical_x,
        logical_y,
        logical_width,
        logical_height,
        aligned_x,
        aligned_y,
        aligned_width,
        aligned_height,
        transfer_bytes,
        dirty_cells,
        dirty_rows,
        coalesced,
        first_sequence,
        last_sequence,
        free_heap,
        minimum_free_heap,
    })
}

pub const fn u32_before_or_equal(first: u32, second: u32) -> bool {
    second.wrapping_sub(first) < 0x8000_0000
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex_decode(input: &str) -> Vec<u8> {
        assert_eq!(input.len() % 2, 0);
        input
            .as_bytes()
            .chunks_exact(2)
            .map(|pair| {
                let text = std::str::from_utf8(pair).expect("fixture hex is ASCII");
                u8::from_str_radix(text, 16).expect("fixture hex is valid")
            })
            .collect()
    }

    #[test]
    fn shared_golden_frames_encode_and_decode() {
        let fixtures = include_str!("../fixtures/protocol-v3.frames");
        for line in fixtures
            .lines()
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
        {
            let fields: Vec<_> = line.split('|').collect();
            assert_eq!(fields.len(), 6, "invalid fixture line: {line}");
            let frame_type = u8::from_str_radix(fields[1], 16).unwrap();
            let flags = u8::from_str_radix(fields[2], 16).unwrap();
            let sequence = u32::from_str_radix(fields[3], 16).unwrap();
            let payload = hex_decode(fields[4]);
            let expected = hex_decode(fields[5]);
            assert_eq!(
                encode_frame(frame_type, &payload, sequence, flags).unwrap(),
                expected,
                "fixture {}",
                fields[0]
            );
            assert_eq!(
                FrameDecoder::new().feed(&expected).unwrap(),
                vec![Frame {
                    frame_type,
                    flags,
                    sequence,
                    payload,
                }],
                "fixture {}",
                fields[0]
            );
        }
    }

    #[test]
    fn every_fragment_boundary_and_coalesced_frames_decode() {
        let first = encode_frame(FrameType::TerminalOutput.as_u8(), b"abc", 1, 0).unwrap();
        let second = encode_frame(FrameType::Heartbeat.as_u8(), b"", 2, 0).unwrap();
        let expected = vec![
            Frame {
                frame_type: FrameType::TerminalOutput.as_u8(),
                flags: 0,
                sequence: 1,
                payload: b"abc".to_vec(),
            },
            Frame {
                frame_type: FrameType::Heartbeat.as_u8(),
                flags: 0,
                sequence: 2,
                payload: Vec::new(),
            },
        ];
        for split in 0..=first.len() {
            let mut decoder = FrameDecoder::new();
            let mut decoded = decoder.feed(&first[..split]).unwrap();
            let mut remainder = first[split..].to_vec();
            remainder.extend_from_slice(&second);
            decoded.extend(decoder.feed(&remainder).unwrap());
            assert_eq!(decoded, expected, "fragment boundary {split}");
        }
    }

    #[test]
    fn rejects_nonzero_flags_and_oversized_length() {
        assert!(matches!(
            FrameDecoder::new().feed(&hex_decode("0101000000000001")),
            Err(ProtocolError::UnsupportedFlags(1))
        ));
        assert!(matches!(
            FrameDecoder::new().feed(&hex_decode("0100020100000001")),
            Err(ProtocolError::PayloadTooLarge(513))
        ));
    }

    #[test]
    fn diagnostic_commands_are_named_and_bounded() {
        assert_eq!(
            encode_diagnostic_command(
                DiagnosticCommand::Pattern,
                Some(DiagnosticPattern::Row),
                Some(1)
            )
            .unwrap(),
            vec![3, 3, 1]
        );
        assert!(encode_diagnostic_command(
            DiagnosticCommand::Pattern,
            Some(DiagnosticPattern::Row),
            Some(2)
        )
        .is_err());
    }

    #[test]
    fn decodes_session_metadata() {
        let build = b"1.5.0-test";
        let freeink = b"abc1234";
        let mut payload = vec![
            1,
            1,
            0,
            0,
            0,
            20,
            0xF4,
            3,
            0,
            0,
            87,
            (-51_i8) as u8,
            80,
            24,
            1,
        ];
        payload.extend_from_slice(&800_u16.to_be_bytes());
        payload.extend_from_slice(&480_u16.to_be_bytes());
        payload.extend_from_slice(&50_000_u32.to_be_bytes());
        payload.extend_from_slice(&42_000_u32.to_be_bytes());
        payload.push(build.len() as u8);
        payload.extend_from_slice(build);
        payload.push(freeink.len() as u8);
        payload.extend_from_slice(freeink);

        let response = decode_diagnostic_response(&payload).unwrap();
        let metadata = response.metadata.unwrap();
        assert_eq!(metadata.rssi_dbm, -51);
        assert_eq!(metadata.build, "1.5.0-test");
        assert_eq!(metadata.freeink, "abc1234");
        assert!(!metadata.inverted());
        assert!(!metadata.fading_fix());
        assert!(metadata.adaptive_refresh());
        assert!(!metadata.overclocked_spi());
        assert!(!metadata.auto_settle());
        assert!(metadata.balanced_sustain());
        assert!(metadata.waveform_100ms());
        assert!(metadata.ram_ping_pong());
    }

    #[test]
    fn decodes_read_only_refresh_metrics() {
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
        let legacy = decode_diagnostic_response(&payload)
            .unwrap()
            .metrics
            .unwrap();
        assert_eq!(payload.len(), METRICS_RESPONSE_V1_SIZE);
        assert_eq!(legacy.burst_ends, 0);
        for value in 17_u32..=22 {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        assert_eq!(payload.len(), METRICS_RESPONSE_SIZE);

        let response = decode_diagnostic_response(&payload).unwrap();
        assert!(response.metadata.is_none());
        let metrics = response.metrics.unwrap();
        assert_eq!(metrics.updates, 1);
        assert_eq!(metrics.fallback, 3);
        assert_eq!(metrics.last_baseline_us, 13);
        assert_eq!(metrics.maximum_total_us, 16);
        assert_eq!(metrics.last_region_width, 800);
        assert_eq!(metrics.last_region_height, 432);
        assert_eq!(metrics.last_region_bytes, 42_768);
        assert_eq!(metrics.free_heap, 53_416);
        assert_eq!(metrics.minimum_free_heap, 44_588);
        assert_eq!(metrics.rx_bytes, 17);
        assert_eq!(metrics.rx_reads, 18);
        assert_eq!(metrics.burst_ends, 19);
        assert_eq!(metrics.burst_snapshots, 20);
        assert_eq!(metrics.burst_timeouts, 21);
        assert_eq!(metrics.async_tail_updates, 22);
    }

    #[test]
    fn decodes_refresh_event_and_timestamp_wrap() {
        let mut payload = vec![1, 1, 3, 1, 1, 0, 2, 0];
        for value in 1_u32..=15 {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        for value in 16_u16..=23 {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        payload.extend_from_slice(&24_u32.to_be_bytes());
        payload.extend_from_slice(&25_u16.to_be_bytes());
        payload.push(26);
        payload.push(27);
        for value in 28_u32..=31 {
            payload.extend_from_slice(&value.to_be_bytes());
        }
        assert_eq!(payload.len(), REFRESH_EVENT_SIZE);

        let event = decode_diagnostic_refresh_event(&payload).unwrap();
        assert_eq!(event.timestamp_us, 1);
        assert_eq!(event.minimum_free_heap, 31);
        assert!(u32_before_or_equal(0xffff_fff0, 0x0000_0010));
        assert!(!u32_before_or_equal(100, 99));
    }
}

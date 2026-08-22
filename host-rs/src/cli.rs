use std::ffi::OsString;
use std::fmt;
use std::path::PathBuf;
use std::time::Duration;

use crate::bridge::{
    ProtocolPreference, DEFAULT_APPROVAL_TIMEOUT, DEFAULT_CAPTURE_LIMIT, DEFAULT_MAX_BPS,
    DEFAULT_RETRY_INTERVAL,
};
use crate::control::{DisplayCommand, DEFAULT_CLIENT_TIMEOUT};
use crate::diagnostics::DiagnosticSuite;
use crate::discovery::{DEFAULT_DISCOVERY_TIMEOUT, DEFAULT_WIFI_PORT};
use crate::transport::SecurityMode;

pub const HELP: &str = concat!(
    "knietty Rust host bridge\n\n",
    "Usage:\n",
    "  knietty list [--discovery-timeout SECONDS] [--port PORT]\n",
    "  knietty --list-devices [--discovery-timeout SECONDS] [--port PORT]\n",
    "  knietty connect [OPTIONS]\n",
    "  knietty [OPTIONS]\n",
    "  knietty diagnose --output PATH [OPTIONS]\n",
    "  knietty display status|metrics|clean [--wait] [--json] [--device ID] [--timeout SECONDS]\n",
    "  knietty display polarity normal|inverted [--json] [--device ID] [--timeout SECONDS]\n",
    "  knietty pty-smoke --command COMMAND [--cols 80] [--rows 24] [--term vt100]\n\n",
    "Commands:\n",
    "  list                Discover knietty terminals on the local network\n",
    "  connect             Bridge a host PTY to a Wi-Fi knietty terminal\n",
    "  diagnose            Run a physically approved bounded display test\n",
    "  display             Control an X4 through an active local bridge\n",
    "  pty-smoke           Run one bounded command through the Rust PTY layer\n\n",
    "Compatibility:\n",
    "  --list-devices      Legacy alias for `list`\n",
    "  options without `connect` also start the foreground bridge\n\n",
    "Options:\n",
    "  --host HOST                  X4 IP/hostname, or auto (default: auto)\n",
    "  --discovery-timeout SECONDS  Discovery window (default: 2)\n",
    "  --port PORT                  Discovery UDP port (default: 29380)\n",
    "  --command COMMAND            PTY command (default: tmux, then $SHELL)\n",
    "  --cols COLUMNS               Initial PTY columns (default: 80)\n",
    "  --rows ROWS                  Initial PTY rows (default: 24)\n",
    "  --term TERM                  PTY TERM value (default: vt100)\n",
    "  --protocol auto|3|2|1        Wi-Fi protocol (default: auto)\n",
    "  --insecure-plaintext         Explicit legacy trusted-LAN development mode\n",
    "  --max-bps BYTES              PTY output pacing limit (default: 262144)\n",
    "  --capture-output PATH        Privately capture raw host-to-X4 PTY bytes\n",
    "  --capture-limit BYTES        Stop capture/session at this size (default: 8388608)\n",
    "  --retry-interval SECONDS     Retry delay (default: 1)\n",
    "  --approval-timeout SECONDS   X4 approval deadline (default: 60)\n",
    "  --reconnect                  Reconnect after an established session drops\n",
    "  --local-input                Forward this terminal's keyboard\n",
    "  --no-local-input             Disable local keyboard forwarding\n",
    "  --verbose                    Show connection/retry detail\n",
    "  --suite SUITE                smoke, latency, cadence, or burst\n",
    "  --output PATH                Required JSON Lines diagnostics output\n",
    "  --repetitions COUNT          Latency repetitions, 1-3 (default: 3)\n",
    "  --settle-seconds SECONDS     Cadence quiet interval (default: 1)\n",
    "  --command-timeout SECONDS    Per-command deadline (default: 15)\n",
    "  --device ID                  Select one active local bridge\n",
    "  --wait                       Wait for clean instead of deferring past the prompt\n",
    "  --json                       Print mutation telemetry (status/metrics always print JSON)\n",
    "  --timeout SECONDS            Display/PTY command deadline\n",
    "  -h, --help                   Print help\n",
    "  -V, --version                Print version\n\n",
    "Daemon mode remains deferred to the Linux integration backlog.\n",
);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LocalInputMode {
    Auto,
    Enabled,
    Disabled,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ConnectOptions {
    pub host: String,
    pub port: u16,
    pub cols: u16,
    pub rows: u16,
    pub command: Option<String>,
    pub term: String,
    pub max_bps: usize,
    pub capture_output: Option<PathBuf>,
    pub capture_limit: usize,
    pub retry_interval: Duration,
    pub discovery_timeout: Duration,
    pub approval_timeout: Duration,
    pub reconnect: bool,
    pub local_input: LocalInputMode,
    pub protocol: ProtocolPreference,
    pub verbose: bool,
    pub security: SecurityMode,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DiagnoseOptions {
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

#[derive(Clone, Debug, PartialEq)]
pub struct DisplayOptions {
    pub command: DisplayCommand,
    pub device: Option<String>,
    pub timeout: Duration,
    pub json: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    List {
        timeout: Duration,
        port: u16,
    },
    PtySmoke {
        command: String,
        cols: u16,
        rows: u16,
        term: String,
        timeout: Duration,
    },
    Connect(ConnectOptions),
    Diagnose(DiagnoseOptions),
    Display(DisplayOptions),
    Help,
    Version,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CliError(pub String);

impl fmt::Display for CliError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

fn parse_positive_duration(value: &str, option: &str) -> Result<Duration, CliError> {
    let seconds = value
        .parse::<f64>()
        .map_err(|_| CliError(format!("{option} must be a number")))?;
    if !seconds.is_finite() || seconds <= 0.0 {
        return Err(CliError(format!("{option} must be greater than zero")));
    }
    Duration::try_from_secs_f64(seconds).map_err(|_| CliError(format!("{option} is too large")))
}

fn parse_nonnegative_duration(value: &str, option: &str) -> Result<Duration, CliError> {
    let seconds = value
        .parse::<f64>()
        .map_err(|_| CliError(format!("{option} must be a number")))?;
    if !seconds.is_finite() || seconds < 0.0 {
        return Err(CliError(format!("{option} must be zero or greater")));
    }
    Duration::try_from_secs_f64(seconds).map_err(|_| CliError(format!("{option} is too large")))
}

fn parse_positive_u16(value: &str, option: &str) -> Result<u16, CliError> {
    value
        .parse::<u16>()
        .ok()
        .filter(|value| *value != 0)
        .ok_or_else(|| CliError(format!("{option} must be between 1 and 65535")))
}

fn parse_positive_usize(value: &str, option: &str) -> Result<usize, CliError> {
    value
        .parse::<usize>()
        .ok()
        .filter(|value| *value != 0)
        .ok_or_else(|| CliError(format!("{option} must be greater than zero")))
}

fn parse_connect(arguments: &[String]) -> Result<Action, CliError> {
    let mut options = ConnectOptions {
        host: "auto".to_owned(),
        port: DEFAULT_WIFI_PORT,
        cols: 80,
        rows: 24,
        command: None,
        term: "vt100".to_owned(),
        max_bps: DEFAULT_MAX_BPS,
        capture_output: None,
        capture_limit: DEFAULT_CAPTURE_LIMIT,
        retry_interval: DEFAULT_RETRY_INTERVAL,
        discovery_timeout: DEFAULT_DISCOVERY_TIMEOUT,
        approval_timeout: DEFAULT_APPROVAL_TIMEOUT,
        reconnect: false,
        local_input: LocalInputMode::Auto,
        protocol: ProtocolPreference::Auto,
        verbose: false,
        security: SecurityMode::Tls,
    };
    let mut index = 0;
    while index < arguments.len() {
        let option = &arguments[index];
        match option.as_str() {
            "--reconnect" => options.reconnect = true,
            "--local-input" => options.local_input = LocalInputMode::Enabled,
            "--no-local-input" => options.local_input = LocalInputMode::Disabled,
            "--verbose" => options.verbose = true,
            "--insecure-plaintext" => options.security = SecurityMode::InsecurePlaintext,
            _ => {
                let value = arguments
                    .get(index + 1)
                    .ok_or_else(|| CliError(format!("{option} requires a value")))?;
                match option.as_str() {
                    "--transport" if value == "wifi" => {}
                    "--transport" => {
                        return Err(CliError(
                            "the Rust bridge currently supports only --transport wifi".to_owned(),
                        ))
                    }
                    "--host" => {
                        if value.is_empty() {
                            return Err(CliError("--host must not be empty".to_owned()));
                        }
                        options.host = value.clone();
                    }
                    "--port" => options.port = parse_positive_u16(value, "--port")?,
                    "--cols" => options.cols = parse_positive_u16(value, "--cols")?,
                    "--rows" => options.rows = parse_positive_u16(value, "--rows")?,
                    "--command" => {
                        if value.is_empty() {
                            return Err(CliError("--command must not be empty".to_owned()));
                        }
                        options.command = Some(value.clone());
                    }
                    "--term" => {
                        if value.is_empty() {
                            return Err(CliError("--term must not be empty".to_owned()));
                        }
                        options.term = value.clone();
                    }
                    "--max-bps" => {
                        options.max_bps = parse_positive_usize(value, "--max-bps")?;
                    }
                    "--capture-output" => {
                        if value.is_empty() {
                            return Err(CliError("--capture-output must not be empty".to_owned()));
                        }
                        options.capture_output = Some(PathBuf::from(value));
                    }
                    "--capture-limit" => {
                        options.capture_limit = parse_positive_usize(value, "--capture-limit")?;
                    }
                    "--retry-interval" => {
                        options.retry_interval =
                            parse_positive_duration(value, "--retry-interval")?;
                    }
                    "--discovery-timeout" => {
                        options.discovery_timeout =
                            parse_positive_duration(value, "--discovery-timeout")?;
                    }
                    "--approval-timeout" => {
                        options.approval_timeout =
                            parse_positive_duration(value, "--approval-timeout")?;
                    }
                    "--protocol" => {
                        options.protocol = match value.as_str() {
                            "auto" => ProtocolPreference::Auto,
                            "3" => ProtocolPreference::V3,
                            "2" => ProtocolPreference::V2,
                            "1" => ProtocolPreference::V1,
                            _ => {
                                return Err(CliError(
                                    "--protocol must be auto, 3, 2, or 1".to_owned(),
                                ))
                            }
                        };
                    }
                    _ => return Err(CliError(format!("unsupported option {option:?}"))),
                }
                index += 1;
            }
        }
        index += 1;
    }
    if options.capture_output.is_none() && options.capture_limit != DEFAULT_CAPTURE_LIMIT {
        return Err(CliError(
            "--capture-limit requires --capture-output".to_owned(),
        ));
    }
    if options.security == SecurityMode::Tls
        && matches!(
            options.protocol,
            ProtocolPreference::V1 | ProtocolPreference::V2
        )
    {
        return Err(CliError(
            "legacy protocol v1/v2 requires --insecure-plaintext".to_owned(),
        ));
    }
    Ok(Action::Connect(options))
}

fn parse_diagnose(arguments: &[String]) -> Result<Action, CliError> {
    let mut host = "auto".to_owned();
    let mut port = DEFAULT_WIFI_PORT;
    let mut suite = DiagnosticSuite::Smoke;
    let mut output = None;
    let mut repetitions = 3_u8;
    let mut settle = Duration::from_secs(1);
    let mut discovery_timeout = DEFAULT_DISCOVERY_TIMEOUT;
    let mut approval_timeout = DEFAULT_APPROVAL_TIMEOUT;
    let mut command_timeout = Duration::from_secs(15);
    let mut verbose = false;
    let mut security = SecurityMode::Tls;
    let mut index = 0;
    while index < arguments.len() {
        let option = &arguments[index];
        if option == "--verbose" || option == "--insecure-plaintext" {
            if option == "--verbose" {
                verbose = true;
            } else {
                security = SecurityMode::InsecurePlaintext;
            }
            index += 1;
            continue;
        }
        let value = arguments
            .get(index + 1)
            .ok_or_else(|| CliError(format!("{option} requires a value")))?;
        match option.as_str() {
            "--host" => {
                if value.is_empty() {
                    return Err(CliError("--host must not be empty".to_owned()));
                }
                host = value.clone();
            }
            "--port" => port = parse_positive_u16(value, "--port")?,
            "--suite" => {
                suite = match value.as_str() {
                    "smoke" => DiagnosticSuite::Smoke,
                    "latency" => DiagnosticSuite::Latency,
                    "cadence" => DiagnosticSuite::Cadence,
                    "burst" => DiagnosticSuite::Burst,
                    _ => {
                        return Err(CliError(
                            "--suite must be smoke, latency, cadence, or burst".to_owned(),
                        ))
                    }
                };
            }
            "--output" => {
                if value.is_empty() {
                    return Err(CliError("--output must not be empty".to_owned()));
                }
                output = Some(PathBuf::from(value));
            }
            "--repetitions" => {
                repetitions = value
                    .parse::<u8>()
                    .ok()
                    .filter(|value| (1..=3).contains(value))
                    .ok_or_else(|| CliError("--repetitions must be between 1 and 3".to_owned()))?;
            }
            "--settle-seconds" => {
                settle = parse_nonnegative_duration(value, "--settle-seconds")?;
            }
            "--discovery-timeout" => {
                discovery_timeout = parse_positive_duration(value, "--discovery-timeout")?;
            }
            "--approval-timeout" => {
                approval_timeout = parse_positive_duration(value, "--approval-timeout")?;
            }
            "--command-timeout" => {
                command_timeout = parse_positive_duration(value, "--command-timeout")?;
            }
            _ => return Err(CliError(format!("unsupported option {option:?}"))),
        }
        index += 2;
    }
    Ok(Action::Diagnose(DiagnoseOptions {
        host,
        port,
        suite,
        output: output.ok_or_else(|| CliError("diagnose requires --output".to_owned()))?,
        repetitions,
        settle,
        discovery_timeout,
        approval_timeout,
        command_timeout,
        verbose,
        security,
    }))
}

fn parse_pty_smoke(arguments: &[String]) -> Result<Action, CliError> {
    let mut command = None;
    let mut cols = 80;
    let mut rows = 24;
    let mut term = "vt100".to_owned();
    let mut timeout = Duration::from_secs(5);
    let mut index = 0;
    while index < arguments.len() {
        let option = &arguments[index];
        let value = arguments
            .get(index + 1)
            .ok_or_else(|| CliError(format!("{option} requires a value")))?;
        match option.as_str() {
            "--command" => {
                if value.is_empty() {
                    return Err(CliError("--command must not be empty".to_owned()));
                }
                command = Some(value.clone());
            }
            "--cols" => cols = parse_positive_u16(value, "--cols")?,
            "--rows" => rows = parse_positive_u16(value, "--rows")?,
            "--term" => {
                if value.is_empty() {
                    return Err(CliError("--term must not be empty".to_owned()));
                }
                term = value.clone();
            }
            "--timeout" => timeout = parse_positive_duration(value, "--timeout")?,
            _ => return Err(CliError(format!("unsupported option {option:?}"))),
        }
        index += 2;
    }
    Ok(Action::PtySmoke {
        command: command.ok_or_else(|| CliError("pty-smoke requires --command".to_owned()))?,
        cols,
        rows,
        term,
        timeout,
    })
}

fn parse_display(arguments: &[String]) -> Result<Action, CliError> {
    let (mut command, mut index) = match arguments.first().map(String::as_str) {
        Some("status") => (DisplayCommand::Status, 1),
        Some("metrics") => (DisplayCommand::Metrics, 1),
        Some("clean") => (DisplayCommand::CleanDeferred, 1),
        Some("polarity") => match arguments.get(1).map(String::as_str) {
            Some("normal") => (DisplayCommand::PolarityNormal, 2),
            Some("inverted") => (DisplayCommand::PolarityInverted, 2),
            _ => {
                return Err(CliError(
                    "display polarity requires normal or inverted".to_owned(),
                ))
            }
        },
        Some(command) => return Err(CliError(format!("unsupported display command {command:?}"))),
        None => {
            return Err(CliError(
                "display requires status, metrics, clean, or polarity".to_owned(),
            ))
        }
    };
    let mut device = None;
    let mut timeout = DEFAULT_CLIENT_TIMEOUT;
    let mut json = false;
    while index < arguments.len() {
        let option = &arguments[index];
        match option.as_str() {
            "--wait" if command == DisplayCommand::CleanDeferred => {
                command = DisplayCommand::Clean;
                index += 1;
                continue;
            }
            "--wait" => return Err(CliError("--wait applies only to display clean".to_owned())),
            "--json" => {
                json = true;
                index += 1;
                continue;
            }
            "--device" => {
                let value = arguments
                    .get(index + 1)
                    .ok_or_else(|| CliError("--device requires a value".to_owned()))?;
                if value.is_empty() {
                    return Err(CliError("--device must not be empty".to_owned()));
                }
                device = Some(value.clone());
            }
            "--timeout" => {
                let value = arguments
                    .get(index + 1)
                    .ok_or_else(|| CliError("--timeout requires a value".to_owned()))?;
                timeout = parse_positive_duration(value, "--timeout")?;
            }
            _ => return Err(CliError(format!("unsupported option {option:?}"))),
        }
        index += 2;
    }
    Ok(Action::Display(DisplayOptions {
        command,
        device,
        timeout,
        json,
    }))
}

fn parse_list(arguments: &[String]) -> Result<Action, CliError> {
    let mut timeout = DEFAULT_DISCOVERY_TIMEOUT;
    let mut port = DEFAULT_WIFI_PORT;
    let mut index = 0;
    while index < arguments.len() {
        let option = &arguments[index];
        let value = arguments
            .get(index + 1)
            .ok_or_else(|| CliError(format!("{option} requires a value")))?;
        match option.as_str() {
            "--discovery-timeout" => {
                timeout = parse_positive_duration(value, "--discovery-timeout")?;
            }
            "--port" => port = parse_positive_u16(value, "--port")?,
            _ => return Err(CliError(format!("unsupported option {option:?}"))),
        }
        index += 2;
    }
    Ok(Action::List { timeout, port })
}

pub fn parse<I, T>(arguments: I) -> Result<Action, CliError>
where
    I: IntoIterator<Item = T>,
    T: Into<OsString>,
{
    let mut arguments: Vec<String> = arguments
        .into_iter()
        .map(Into::into)
        .map(|argument| {
            argument
                .into_string()
                .map_err(|_| CliError("arguments must be valid UTF-8".to_owned()))
        })
        .collect::<Result<_, _>>()?;

    if arguments
        .iter()
        .any(|argument| argument == "-h" || argument == "--help")
    {
        return Ok(Action::Help);
    }
    if arguments
        .iter()
        .any(|argument| argument == "-V" || argument == "--version")
    {
        return Ok(Action::Version);
    }
    if arguments.first().map(String::as_str) == Some("pty-smoke") {
        return parse_pty_smoke(&arguments[1..]);
    }
    if arguments.first().map(String::as_str) == Some("diagnose") {
        return parse_diagnose(&arguments[1..]);
    }
    if arguments.first().map(String::as_str) == Some("display") {
        return parse_display(&arguments[1..]);
    }

    let list_alias = arguments
        .iter()
        .any(|argument| argument == "--list-devices");
    arguments.retain(|argument| argument != "--list-devices");
    if matches!(
        arguments.first().map(String::as_str),
        Some("list" | "list-devices")
    ) {
        arguments.remove(0);
        return parse_list(&arguments);
    }
    if list_alias {
        return parse_list(&arguments);
    }
    if arguments.first().map(String::as_str) == Some("connect") {
        arguments.remove(0);
    } else if arguments
        .first()
        .is_some_and(|argument| !argument.starts_with('-'))
    {
        return Err(CliError(format!(
            "unsupported command {:?}",
            arguments.first().expect("first argument exists")
        )));
    }
    parse_connect(&arguments)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_list_command_and_compatibility_alias() {
        let expected = Action::List {
            timeout: Duration::from_secs(3),
            port: 30_000,
        };
        assert_eq!(
            parse(["list", "--discovery-timeout", "3", "--port", "30000"]).unwrap(),
            expected
        );
        assert_eq!(
            parse([
                "--list-devices",
                "--discovery-timeout",
                "3",
                "--port",
                "30000"
            ])
            .unwrap(),
            expected
        );
    }

    #[test]
    fn rejects_invalid_values_and_commands() {
        assert!(parse(["list", "--discovery-timeout", "0"]).is_err());
        assert!(parse(["list", "--port", "65536"]).is_err());
        assert!(parse(["connect", "--max-bps", "0"]).is_err());
        assert!(parse(["connect", "--capture-output", ""]).is_err());
        assert!(parse(["connect", "--capture-limit", "0"]).is_err());
        assert!(parse(["connect", "--capture-limit", "4096"]).is_err());
        assert!(parse(["connect", "--protocol", "2"]).is_err());
        assert!(parse(["connect", "--protocol", "2", "--insecure-plaintext"]).is_ok());
        assert!(parse(["wat"]).is_err());
    }

    #[test]
    fn parses_bounded_pty_smoke_command() {
        assert_eq!(
            parse([
                "pty-smoke",
                "--command",
                "printf ok",
                "--cols",
                "42",
                "--rows",
                "21",
                "--term",
                "vt100",
                "--timeout",
                "2.5"
            ])
            .unwrap(),
            Action::PtySmoke {
                command: "printf ok".to_owned(),
                cols: 42,
                rows: 21,
                term: "vt100".to_owned(),
                timeout: Duration::from_millis(2500),
            }
        );
        assert!(parse(["pty-smoke"]).is_err());
        assert!(parse(["pty-smoke", "--command", "echo ok", "--cols", "0"]).is_err());
    }

    #[test]
    fn parses_connect_and_legacy_invocation() {
        let expected = Action::Connect(ConnectOptions {
            host: "192.0.2.20".to_owned(),
            port: 30_000,
            cols: 42,
            rows: 21,
            command: Some("printf ok".to_owned()),
            term: "xterm-256color".to_owned(),
            max_bps: 32_768,
            capture_output: Some(PathBuf::from("capture.raw")),
            capture_limit: 4096,
            retry_interval: Duration::from_millis(500),
            discovery_timeout: Duration::from_secs(3),
            approval_timeout: Duration::from_secs(10),
            reconnect: true,
            local_input: LocalInputMode::Disabled,
            protocol: ProtocolPreference::V3,
            verbose: true,
            security: SecurityMode::Tls,
        });
        let arguments = [
            "--transport",
            "wifi",
            "--host",
            "192.0.2.20",
            "--port",
            "30000",
            "--cols",
            "42",
            "--rows",
            "21",
            "--command",
            "printf ok",
            "--term",
            "xterm-256color",
            "--max-bps",
            "32768",
            "--capture-output",
            "capture.raw",
            "--capture-limit",
            "4096",
            "--retry-interval",
            "0.5",
            "--discovery-timeout",
            "3",
            "--approval-timeout",
            "10",
            "--reconnect",
            "--no-local-input",
            "--protocol",
            "3",
            "--verbose",
        ];
        assert_eq!(parse(arguments).unwrap(), expected);
        let mut explicit = vec!["connect"];
        explicit.extend(arguments);
        assert_eq!(parse(explicit).unwrap(), expected);
        assert!(matches!(
            parse([] as [&str; 0]).unwrap(),
            Action::Connect(_)
        ));
    }

    #[test]
    fn diagnostics_are_explicit_and_bounded() {
        assert_eq!(
            parse([
                "diagnose",
                "--host",
                "192.0.2.20",
                "--suite",
                "latency",
                "--output",
                "run.jsonl",
                "--repetitions",
                "2",
                "--settle-seconds",
                "0",
                "--command-timeout",
                "4.5",
                "--verbose"
            ])
            .unwrap(),
            Action::Diagnose(DiagnoseOptions {
                host: "192.0.2.20".to_owned(),
                port: DEFAULT_WIFI_PORT,
                suite: DiagnosticSuite::Latency,
                output: PathBuf::from("run.jsonl"),
                repetitions: 2,
                settle: Duration::ZERO,
                discovery_timeout: DEFAULT_DISCOVERY_TIMEOUT,
                approval_timeout: DEFAULT_APPROVAL_TIMEOUT,
                command_timeout: Duration::from_millis(4500),
                verbose: true,
                security: SecurityMode::Tls,
            })
        );
        assert!(parse(["diagnose"]).is_err());
        assert!(parse(["diagnose", "--output", "x", "--repetitions", "4"]).is_err());
        assert!(parse(["diagnose", "--output", "x", "--settle-seconds", "-1"]).is_err());
        assert!(matches!(
            parse(["diagnose", "--output", "x", "--insecure-plaintext"]).unwrap(),
            Action::Diagnose(DiagnoseOptions {
                security: SecurityMode::InsecurePlaintext,
                ..
            })
        ));
    }

    #[test]
    fn display_commands_are_explicit_and_bounded() {
        assert_eq!(
            parse([
                "display",
                "status",
                "--device",
                "knietty-9e54a0",
                "--timeout",
                "3"
            ])
            .unwrap(),
            Action::Display(DisplayOptions {
                command: DisplayCommand::Status,
                device: Some("knietty-9e54a0".to_owned()),
                timeout: Duration::from_secs(3),
                json: false,
            })
        );
        assert_eq!(
            parse(["display", "clean"]).unwrap(),
            Action::Display(DisplayOptions {
                command: DisplayCommand::CleanDeferred,
                device: None,
                timeout: DEFAULT_CLIENT_TIMEOUT,
                json: false,
            })
        );
        assert_eq!(
            parse(["display", "metrics", "--json"]).unwrap(),
            Action::Display(DisplayOptions {
                command: DisplayCommand::Metrics,
                device: None,
                timeout: DEFAULT_CLIENT_TIMEOUT,
                json: true,
            })
        );
        assert_eq!(
            parse(["display", "polarity", "inverted"]).unwrap(),
            Action::Display(DisplayOptions {
                command: DisplayCommand::PolarityInverted,
                device: None,
                timeout: DEFAULT_CLIENT_TIMEOUT,
                json: false,
            })
        );
        assert_eq!(
            parse(["display", "clean", "--wait", "--json"]).unwrap(),
            Action::Display(DisplayOptions {
                command: DisplayCommand::Clean,
                device: None,
                timeout: DEFAULT_CLIENT_TIMEOUT,
                json: true,
            })
        );
        assert!(parse(["display"]).is_err());
        assert!(parse(["display", "polarity", "toggle"]).is_err());
        assert!(parse(["display", "clean", "--timeout", "0"]).is_err());
        assert!(parse(["display", "status", "--wait"]).is_err());
    }
}

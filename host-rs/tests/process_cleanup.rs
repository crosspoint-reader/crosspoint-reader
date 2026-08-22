use std::fs;
use std::io::{BufRead, BufReader};
use std::net::TcpListener;
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use nix::fcntl::{fcntl, FcntlArg, OFlag};
use nix::libc;
use nix::pty::openpty;
use nix::sys::signal::{kill, Signal};
use nix::sys::termios::{tcgetattr, LocalFlags, Termios};
use nix::unistd::{dup, Pid};

fn flags<Fd: std::os::fd::AsFd>(fd: Fd) -> OFlag {
    OFlag::from_bits_truncate(fcntl(fd, FcntlArg::F_GETFL).unwrap())
}

fn assert_termios_equal(left: &Termios, right: &Termios) {
    assert_eq!(left.input_flags, right.input_flags);
    assert_eq!(left.output_flags, right.output_flags);
    assert_eq!(left.control_flags, right.control_flags);
    // Darwin may expose PENDIN transiently after canonical mode is restored.
    assert_eq!(
        left.local_flags - LocalFlags::PENDIN,
        right.local_flags - LocalFlags::PENDIN
    );
    assert_eq!(left.control_chars, right.control_chars);
}

fn wait_for_child(child: &mut std::process::Child, context: &str) -> std::process::ExitStatus {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(status) = child.try_wait().unwrap() {
            return status;
        }
        assert!(Instant::now() < deadline, "{context}");
        thread::sleep(Duration::from_millis(10));
    }
}

#[test]
fn sigterm_during_device_approval_restores_the_callers_terminal() {
    let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
    let address = listener.local_addr().unwrap();
    let server = thread::spawn(move || {
        let (stream, _) = listener.accept().unwrap();
        let mut hello = String::new();
        BufReader::new(stream)
            .read_line(&mut hello)
            .expect("host should send a handshake");
        assert!(hello.starts_with("KNIETTY/3 HELLO terminal frame "));
    });

    let terminal = openpty(None, None).unwrap();
    let original_attributes = tcgetattr(&terminal.slave).unwrap();
    let original_flags = flags(&terminal.slave);
    let child_stdin = Stdio::from(dup(&terminal.slave).unwrap());
    let mut child = Command::new(env!("CARGO_BIN_EXE_knietty"))
        .args([
            "connect",
            "--host",
            &address.ip().to_string(),
            "--port",
            &address.port().to_string(),
            "--protocol",
            "3",
            "--insecure-plaintext",
            "--approval-timeout",
            "10",
            "--local-input",
            "--command",
            "sleep 30",
        ])
        .stdin(child_stdin)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();

    server.join().unwrap();
    let pid = Pid::from_raw(i32::try_from(child.id()).unwrap());
    kill(pid, Signal::SIGTERM).unwrap();
    let status = wait_for_child(&mut child, "Rust host ignored SIGTERM");
    assert_eq!(status.code(), Some(128 + libc::SIGTERM));
    assert_termios_equal(&tcgetattr(&terminal.slave).unwrap(), &original_attributes);
    assert_eq!(flags(&terminal.slave), original_flags);
}

#[test]
fn capture_open_failure_restores_the_callers_terminal_and_preserves_the_file() {
    let capture_path = std::env::temp_dir().join(format!(
        "knietty-existing-capture-{}-{}.raw",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    fs::write(&capture_path, b"sentinel").unwrap();

    let terminal = openpty(None, None).unwrap();
    let original_attributes = tcgetattr(&terminal.slave).unwrap();
    let original_flags = flags(&terminal.slave);
    let child_stdin = Stdio::from(dup(&terminal.slave).unwrap());
    let mut child = Command::new(env!("CARGO_BIN_EXE_knietty"))
        .args([
            "connect",
            "--host",
            "127.0.0.1",
            "--port",
            "9",
            "--local-input",
            "--command",
            "sleep 30",
            "--capture-output",
        ])
        .arg(&capture_path)
        .stdin(child_stdin)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();

    let status = wait_for_child(
        &mut child,
        "Rust host did not exit after capture overwrite refusal",
    );
    assert!(!status.success());
    assert_eq!(fs::read(&capture_path).unwrap(), b"sentinel");
    assert_termios_equal(&tcgetattr(&terminal.slave).unwrap(), &original_attributes);
    assert_eq!(flags(&terminal.slave), original_flags);
    fs::remove_file(capture_path).unwrap();
}

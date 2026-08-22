use std::env;
use std::ffi::OsStr;
use std::fs;
use std::io;
use std::os::fd::{AsFd, AsRawFd, OwnedFd};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use nix::errno::Errno;
use nix::fcntl::{fcntl, FcntlArg, OFlag};
use nix::libc;
use nix::pty::{openpty, OpenptyResult, Winsize};
use nix::sys::signal::{killpg, Signal};
use nix::unistd::{dup, getpgid, getpgrp, Pid};

const SHUTDOWN_GRACE: Duration = Duration::from_secs(2);
const WAIT_POLL_INTERVAL: Duration = Duration::from_millis(10);

fn errno_to_io(error: Errno) -> io::Error {
    io::Error::from_raw_os_error(error as i32)
}

pub fn set_pty_size<Fd: AsFd>(fd: Fd, cols: u16, rows: u16) -> io::Result<()> {
    if cols == 0 || rows == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "PTY rows and columns must be greater than zero",
        ));
    }
    let size = Winsize {
        ws_row: rows,
        ws_col: cols,
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    // SAFETY: fd is borrowed from an AsFd implementor and therefore valid for
    // the duration of the call. TIOCSWINSZ reads exactly one Winsize value.
    let result = unsafe {
        libc::ioctl(
            fd.as_fd().as_raw_fd(),
            libc::TIOCSWINSZ as libc::c_ulong,
            &size,
        )
    };
    if result == -1 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

pub fn get_pty_size<Fd: AsFd>(fd: Fd) -> io::Result<(u16, u16)> {
    let mut size = Winsize {
        ws_row: 0,
        ws_col: 0,
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    // SAFETY: fd is borrowed from an AsFd implementor and therefore valid for
    // the duration of the call. TIOCGWINSZ initializes one Winsize value.
    let result = unsafe {
        libc::ioctl(
            fd.as_fd().as_raw_fd(),
            libc::TIOCGWINSZ as libc::c_ulong,
            &mut size,
        )
    };
    if result == -1 {
        Err(io::Error::last_os_error())
    } else {
        Ok((size.ws_col, size.ws_row))
    }
}

fn executable_in_path(name: &str, path: Option<&OsStr>) -> bool {
    path.into_iter()
        .flat_map(env::split_paths)
        .map(|directory| directory.join(name))
        .any(|candidate| {
            fs::metadata(candidate)
                .map(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
                .unwrap_or(false)
        })
}

fn choose_default_command(tmux_available: bool, shell: Option<&OsStr>) -> String {
    if tmux_available {
        "tmux new-session -A -s knietty".to_owned()
    } else {
        shell
            .filter(|shell| !shell.is_empty())
            .unwrap_or_else(|| OsStr::new("/bin/sh"))
            .to_string_lossy()
            .into_owned()
    }
}

pub fn default_command() -> String {
    choose_default_command(
        executable_in_path("tmux", env::var_os("PATH").as_deref()),
        env::var_os("SHELL").as_deref(),
    )
}

fn set_nonblocking<Fd: AsFd>(fd: Fd) -> io::Result<()> {
    let flags = fcntl(&fd, FcntlArg::F_GETFL).map_err(errno_to_io)?;
    let flags = OFlag::from_bits_truncate(flags);
    fcntl(&fd, FcntlArg::F_SETFL(flags | OFlag::O_NONBLOCK)).map_err(errno_to_io)?;
    Ok(())
}

fn signal_process_group(process_group: Pid, signal: Signal) -> io::Result<()> {
    match killpg(process_group, signal) {
        Ok(()) | Err(Errno::ESRCH) => Ok(()),
        Err(error) => Err(errno_to_io(error)),
    }
}

fn wait_until(child: &mut Child, timeout: Duration) -> io::Result<Option<ExitStatus>> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Some(status));
        }
        if Instant::now() >= deadline {
            return Ok(None);
        }
        thread::sleep(WAIT_POLL_INTERVAL);
    }
}

pub struct PtySession {
    master: Option<OwnedFd>,
    child: Option<Child>,
    process_group: Pid,
}

impl PtySession {
    pub fn spawn(command: &str, cols: u16, rows: u16, term: &str) -> io::Result<Self> {
        if command.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "PTY command must not be empty",
            ));
        }
        if term.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "TERM must not be empty",
            ));
        }

        let winsize = Winsize {
            ws_row: rows,
            ws_col: cols,
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        let OpenptyResult { master, slave } = openpty(Some(&winsize), None).map_err(errno_to_io)?;
        set_nonblocking(&master)?;

        let child_stdin = Stdio::from(dup(&slave).map_err(errno_to_io)?);
        let child_stdout = Stdio::from(dup(&slave).map_err(errno_to_io)?);
        let child_stderr = Stdio::from(slave);
        let mut child_command = Command::new("/bin/sh");
        child_command
            .args(["-lc", command])
            .env("TERM", term)
            .env("COLUMNS", cols.to_string())
            .env("LINES", rows.to_string())
            .stdin(child_stdin)
            .stdout(child_stdout)
            .stderr(child_stderr);

        // SAFETY: only async-signal-safe syscalls are made between fork and
        // exec. setsid creates an isolated process group/session. Command has
        // already installed the requested slave PTY as fd 0 when pre_exec runs,
        // so TIOCSCTTY assigns that valid descriptor as the controlling TTY.
        unsafe {
            child_command.pre_exec(|| {
                if libc::setsid() == -1 {
                    return Err(io::Error::last_os_error());
                }
                if libc::ioctl(libc::STDIN_FILENO, libc::TIOCSCTTY as libc::c_ulong, 0) == -1 {
                    return Err(io::Error::last_os_error());
                }
                Ok(())
            });
        }

        let child = child_command.spawn()?;
        let child_pid = i32::try_from(child.id()).map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "child PID does not fit in pid_t",
            )
        })?;
        Ok(Self {
            master: Some(master),
            child: Some(child),
            process_group: Pid::from_raw(child_pid),
        })
    }

    pub fn master(&self) -> io::Result<&OwnedFd> {
        self.master.as_ref().ok_or_else(|| {
            io::Error::new(io::ErrorKind::BrokenPipe, "PTY master is already closed")
        })
    }

    pub fn child_id(&self) -> Option<u32> {
        self.child.as_ref().map(Child::id)
    }

    pub fn process_group(&self) -> Pid {
        self.process_group
    }

    pub fn process_group_is_isolated(&self) -> io::Result<bool> {
        let child_group = getpgid(Some(self.process_group)).map_err(errno_to_io)?;
        Ok(child_group == self.process_group && child_group != getpgrp())
    }

    pub fn resize(&self, cols: u16, rows: u16) -> io::Result<()> {
        set_pty_size(self.master()?, cols, rows)?;
        self.redraw()
    }

    pub fn redraw(&self) -> io::Result<()> {
        signal_process_group(self.process_group, Signal::SIGWINCH)
    }

    pub fn read(&self, buffer: &mut [u8]) -> io::Result<usize> {
        nix::unistd::read(self.master()?, buffer).map_err(errno_to_io)
    }

    pub fn write(&self, buffer: &[u8]) -> io::Result<usize> {
        nix::unistd::write(self.master()?, buffer).map_err(errno_to_io)
    }

    pub fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        match self.child.as_mut() {
            Some(child) => child.try_wait(),
            None => Ok(None),
        }
    }

    pub fn wait_timeout(&mut self, timeout: Duration) -> io::Result<Option<ExitStatus>> {
        match self.child.as_mut() {
            Some(child) => wait_until(child, timeout),
            None => Ok(None),
        }
    }

    pub fn close(&mut self) -> io::Result<Option<ExitStatus>> {
        self.master.take();
        let Some(child) = self.child.as_mut() else {
            return Ok(None);
        };
        if let Some(status) = child.try_wait()? {
            self.child.take();
            return Ok(Some(status));
        }

        signal_process_group(self.process_group, Signal::SIGHUP)?;
        if let Some(status) = wait_until(child, SHUTDOWN_GRACE)? {
            self.child.take();
            return Ok(Some(status));
        }
        signal_process_group(self.process_group, Signal::SIGTERM)?;
        if let Some(status) = wait_until(child, SHUTDOWN_GRACE)? {
            self.child.take();
            return Ok(Some(status));
        }
        signal_process_group(self.process_group, Signal::SIGKILL)?;
        let status = child.wait()?;
        self.child.take();
        Ok(Some(status))
    }
}

impl Drop for PtySession {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

pub fn exit_status_code(status: ExitStatus) -> i32 {
    status
        .code()
        .or_else(|| status.signal().map(|signal| 128 + signal))
        .unwrap_or(1)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn collect_output(session: &mut PtySession, timeout: Duration) -> io::Result<Vec<u8>> {
        let deadline = Instant::now() + timeout;
        let mut output = Vec::new();
        let mut buffer = [0_u8; 256];
        loop {
            match session.read(&mut buffer) {
                Ok(0) => break,
                Ok(length) => output.extend_from_slice(&buffer[..length]),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
                Err(error) if error.raw_os_error() == Some(libc::EIO) => break,
                Err(error) => return Err(error),
            }
            if session.try_wait()?.is_some() {
                match session.read(&mut buffer) {
                    Ok(length) if length != 0 => output.extend_from_slice(&buffer[..length]),
                    Ok(_) => {}
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
                    Err(error) if error.raw_os_error() == Some(libc::EIO) => {}
                    Err(error) => return Err(error),
                }
                break;
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "PTY test timed out",
                ));
            }
            thread::sleep(Duration::from_millis(5));
        }
        Ok(output)
    }

    #[test]
    fn portable_window_size_round_trip() {
        let OpenptyResult { master, slave: _ } = openpty(None, None).unwrap();
        set_pty_size(&master, 42, 21).unwrap();
        assert_eq!(get_pty_size(&master).unwrap(), (42, 21));
    }

    #[test]
    fn default_command_prefers_tmux_then_shell() {
        assert_eq!(
            choose_default_command(true, Some(OsStr::new("/bin/zsh"))),
            "tmux new-session -A -s knietty"
        );
        assert_eq!(
            choose_default_command(false, Some(OsStr::new("/bin/zsh"))),
            "/bin/zsh"
        );
        assert_eq!(choose_default_command(false, None), "/bin/sh");
    }

    #[test]
    fn spawned_child_receives_terminal_environment() {
        let mut session = PtySession::spawn(
            "printf '%s:%s:%s' \"$TERM\" \"$COLUMNS\" \"$LINES\"",
            42,
            21,
            "vt100",
        )
        .unwrap();
        let output = collect_output(&mut session, Duration::from_secs(2)).unwrap();
        assert!(String::from_utf8_lossy(&output).contains("vt100:42:21"));
        assert!(session.process_group_is_isolated().unwrap_or(true));
    }

    #[test]
    fn active_session_resizes_and_notifies_its_process_group() {
        let mut session = PtySession::spawn("exec sleep 30", 80, 24, "vt100").unwrap();
        session.resize(100, 30).unwrap();
        assert_eq!(get_pty_size(session.master().unwrap()).unwrap(), (100, 30));
        assert!(session.try_wait().unwrap().is_none());
    }

    #[test]
    fn ctrl_c_signals_only_the_pty_child_group() {
        let mut session = PtySession::spawn("exec sleep 30", 80, 24, "vt100").unwrap();
        assert!(session.process_group_is_isolated().unwrap());
        session.write(b"\x03").unwrap();
        let status = session
            .wait_timeout(Duration::from_secs(2))
            .unwrap()
            .expect("child did not receive Ctrl+C");
        assert!(matches!(status.signal(), Some(signal) if signal == Signal::SIGINT as i32));
    }

    #[test]
    fn dropping_session_reaps_the_child() {
        let child_pid = {
            let session = PtySession::spawn("exec sleep 30", 80, 24, "vt100").unwrap();
            session.child_id().unwrap()
        };
        let child_pid = Pid::from_raw(child_pid as i32);
        assert_eq!(nix::sys::signal::kill(child_pid, None), Err(Errno::ESRCH));
    }
}

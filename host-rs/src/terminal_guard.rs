use std::io;
use std::os::fd::{AsFd, OwnedFd};

use nix::errno::Errno;
use nix::fcntl::{fcntl, FcntlArg, OFlag};
use nix::sys::termios::{cfmakeraw, tcgetattr, tcsetattr, SetArg, Termios};
use nix::unistd::dup;

fn errno_to_io(error: Errno) -> io::Error {
    io::Error::from_raw_os_error(error as i32)
}

fn get_flags<Fd: AsFd>(fd: Fd) -> io::Result<OFlag> {
    fcntl(fd, FcntlArg::F_GETFL)
        .map(OFlag::from_bits_truncate)
        .map_err(errno_to_io)
}

fn set_flags<Fd: AsFd>(fd: Fd, flags: OFlag) -> io::Result<()> {
    fcntl(fd, FcntlArg::F_SETFL(flags))
        .map(|_| ())
        .map_err(errno_to_io)
}

pub struct LocalTerminalGuard {
    fd: OwnedFd,
    saved_attributes: Option<Termios>,
    saved_flags: Option<OFlag>,
}

impl LocalTerminalGuard {
    pub fn enable<Fd: AsFd>(fd: Fd) -> io::Result<Self> {
        let fd = dup(fd).map_err(errno_to_io)?;
        let saved_attributes = tcgetattr(&fd).map_err(errno_to_io)?;
        let saved_flags = get_flags(&fd)?;
        let mut raw_attributes = saved_attributes.clone();
        cfmakeraw(&mut raw_attributes);
        tcsetattr(&fd, SetArg::TCSANOW, &raw_attributes).map_err(errno_to_io)?;
        if let Err(error) = set_flags(&fd, saved_flags | OFlag::O_NONBLOCK) {
            let _ = tcsetattr(&fd, SetArg::TCSANOW, &saved_attributes);
            return Err(error);
        }
        Ok(Self {
            fd,
            saved_attributes: Some(saved_attributes),
            saved_flags: Some(saved_flags),
        })
    }

    pub fn fd(&self) -> &OwnedFd {
        &self.fd
    }

    pub fn restore(&mut self) -> io::Result<()> {
        let mut first_error = None;
        if let Some(attributes) = self.saved_attributes.take() {
            if let Err(error) = tcsetattr(&self.fd, SetArg::TCSANOW, &attributes) {
                first_error = Some(errno_to_io(error));
            }
        }
        if let Some(flags) = self.saved_flags.take() {
            if let Err(error) = set_flags(&self.fd, flags) {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
        match first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }
}

impl Drop for LocalTerminalGuard {
    fn drop(&mut self) {
        let _ = self.restore();
    }
}

#[cfg(test)]
mod tests {
    use std::panic::{catch_unwind, AssertUnwindSafe};

    use nix::pty::openpty;
    use nix::sys::termios::LocalFlags;

    use super::*;

    fn assert_termios_equal(left: &Termios, right: &Termios) {
        assert_eq!(left.input_flags, right.input_flags);
        assert_eq!(left.output_flags, right.output_flags);
        assert_eq!(left.control_flags, right.control_flags);
        // Darwin sets PENDIN as transient kernel state when canonical input is
        // restored while the PTY master remains open. This is transient kernel
        // state, not a user-configurable mode to preserve.
        assert_eq!(
            left.local_flags - LocalFlags::PENDIN,
            right.local_flags - LocalFlags::PENDIN
        );
        assert_eq!(left.control_chars, right.control_chars);
    }

    #[test]
    fn raw_mode_and_blocking_state_restore_on_drop() {
        let pair = openpty(None, None).unwrap();
        let original_attributes = tcgetattr(&pair.slave).unwrap();
        let original_flags = get_flags(&pair.slave).unwrap();
        {
            let guard = LocalTerminalGuard::enable(&pair.slave).unwrap();
            let active = tcgetattr(guard.fd()).unwrap();
            assert!(!active.local_flags.contains(LocalFlags::ICANON));
            assert!(!active.local_flags.contains(LocalFlags::ISIG));
            assert!(get_flags(guard.fd()).unwrap().contains(OFlag::O_NONBLOCK));
        }
        assert_termios_equal(&tcgetattr(&pair.slave).unwrap(), &original_attributes);
        assert_eq!(get_flags(&pair.slave).unwrap(), original_flags);
    }

    #[test]
    fn raw_mode_restores_during_unwind() {
        let pair = openpty(None, None).unwrap();
        let original_attributes = tcgetattr(&pair.slave).unwrap();
        let original_flags = get_flags(&pair.slave).unwrap();
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _guard = LocalTerminalGuard::enable(&pair.slave).unwrap();
            panic!("exercise terminal guard unwind");
        }));
        assert!(result.is_err());
        assert_termios_equal(&tcgetattr(&pair.slave).unwrap(), &original_attributes);
        assert_eq!(get_flags(&pair.slave).unwrap(), original_flags);
    }
}

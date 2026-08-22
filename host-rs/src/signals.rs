use std::io;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use signal_hook::consts::signal::{SIGHUP, SIGINT, SIGQUIT, SIGTERM};
use signal_hook::flag;
use signal_hook::low_level;
use signal_hook::SigId;

pub struct ShutdownSignals {
    signal: Arc<AtomicUsize>,
    registrations: Vec<SigId>,
}

impl ShutdownSignals {
    pub fn install() -> io::Result<Self> {
        let signal = Arc::new(AtomicUsize::new(0));
        let mut registrations = Vec::with_capacity(4);
        for signal_number in [SIGHUP, SIGINT, SIGQUIT, SIGTERM] {
            match flag::register_usize(signal_number, Arc::clone(&signal), signal_number as usize) {
                Ok(registration) => registrations.push(registration),
                Err(error) => {
                    for registration in registrations {
                        low_level::unregister(registration);
                    }
                    return Err(error);
                }
            }
        }
        Ok(Self {
            signal,
            registrations,
        })
    }

    pub fn received(&self) -> Option<i32> {
        match self.signal.load(Ordering::Relaxed) {
            0 => None,
            signal => i32::try_from(signal).ok(),
        }
    }
}

impl Drop for ShutdownSignals {
    fn drop(&mut self) {
        for registration in self.registrations.drain(..) {
            low_level::unregister(registration);
        }
    }
}

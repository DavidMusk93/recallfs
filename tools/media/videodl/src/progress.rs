//! Byte-counting progress meter usable for both downloads and uploads.
//!
//! Renders a single updating line on a TTY and one line per 10% step when the
//! output is redirected (logs stay readable).

use std::io::{self, IsTerminal, Read, Write};
use std::time::{Duration, Instant};

pub struct Meter {
    label: String,
    done: u64,
    total: Option<u64>,
    start: Instant,
    last_emit: Instant,
    next_step: u64,
    tty: bool,
    start_offset: u64,
}

impl Meter {
    pub fn new(label: impl Into<String>, total: Option<u64>, start_offset: u64) -> Self {
        let now = Instant::now();
        Self {
            label: label.into(),
            done: start_offset,
            total,
            start: now,
            last_emit: now - Duration::from_secs(1),
            next_step: 10,
            tty: io::stderr().is_terminal(),
            start_offset,
        }
    }

    pub fn add(&mut self, n: u64) {
        self.done += n;
        self.maybe_emit();
    }

    pub fn finish(&mut self) {
        let elapsed = self.start.elapsed().as_secs_f64().max(0.001);
        let moved = self.done.saturating_sub(self.start_offset);
        let speed = moved as f64 / elapsed / (1024.0 * 1024.0);
        if self.tty {
            let total = self
                .total
                .map(|t| format!(" / {}", human(t)))
                .unwrap_or_default();
            let _ = write!(
                io::stderr(),
                "\r{:<28} 100.0% {}{}  {:.1} MiB/s\n",
                self.label,
                human(self.done),
                total,
                speed
            );
        } else {
            let _ = writeln!(
                io::stderr(),
                "{:<28} done {} in {:.1}s ({:.1} MiB/s)",
                self.label,
                human(self.done),
                elapsed,
                speed
            );
        }
        let _ = io::stderr().flush();
    }

    fn maybe_emit(&mut self) {
        if !self.tty {
            if let Some(total) = self.total {
                if let Some(pct) = self.done.saturating_mul(100).checked_div(total) {
                    if pct >= self.next_step {
                        let _ = writeln!(
                            io::stderr(),
                            "{:<28} {:>3}% {}/{}",
                            self.label,
                            pct,
                            human(self.done),
                            human(total)
                        );
                        self.next_step = (pct / 10 + 1) * 10;
                    }
                }
            }
            return;
        }
        if self.last_emit.elapsed() < Duration::from_millis(500) {
            return;
        }
        self.last_emit = Instant::now();
        let elapsed = self.start.elapsed().as_secs_f64().max(0.001);
        let moved = self.done.saturating_sub(self.start_offset);
        let speed = moved as f64 / elapsed / (1024.0 * 1024.0);
        match self.total {
            Some(total) if total > 0 => {
                let pct = (self.done as f64 / total as f64 * 100.0).min(100.0);
                let eta = if moved > 0 {
                    let remaining = total.saturating_sub(self.done) as f64;
                    let rate = moved as f64 / elapsed;
                    if rate > 0.0 {
                        format!("{:>5}s", (remaining / rate) as u64)
                    } else {
                        "    -".to_string()
                    }
                } else {
                    "    -".to_string()
                };
                let _ = write!(
                    io::stderr(),
                    "\r{:<28} {:5.1}% {}/{} {:>6.1} MiB/s eta {}",
                    self.label,
                    pct,
                    human(self.done),
                    human(total),
                    speed,
                    eta
                );
            }
            _ => {
                let _ = write!(
                    io::stderr(),
                    "\r{:<28} {} {:>6.1} MiB/s",
                    self.label,
                    human(self.done),
                    speed
                );
            }
        }
        let _ = io::stderr().flush();
        if let Some(total) = self.total {
            if total > 0 && self.done >= total {
                let _ = writeln!(io::stderr());
            }
        }
    }
}

/// Cloneable handle so a reader can own the meter while the caller keeps
/// access to it (reqwest needs `Read + Send + 'static` for streamed bodies).
#[derive(Clone)]
pub struct Handle(std::sync::Arc<std::sync::Mutex<Meter>>);

impl Handle {
    pub fn new(label: impl Into<String>, total: Option<u64>, start_offset: u64) -> Self {
        Self(std::sync::Arc::new(std::sync::Mutex::new(Meter::new(
            label,
            total,
            start_offset,
        ))))
    }

    pub fn add(&self, n: u64) {
        if let Ok(mut m) = self.0.lock() {
            m.add(n);
        }
    }

    pub fn finish(&self) {
        if let Ok(mut m) = self.0.lock() {
            m.finish();
        }
    }

    pub fn reader<R>(&self, inner: R) -> MeterReader<R> {
        MeterReader {
            inner,
            handle: self.clone(),
        }
    }
}

/// `Read` adapter that feeds a [`Meter`].
pub struct MeterReader<R> {
    inner: R,
    handle: Handle,
}

impl<R: Read> Read for MeterReader<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        if n > 0 {
            self.handle.add(n as u64);
        }
        Ok(n)
    }
}

pub fn human(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit + 1 < UNITS.len() {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{bytes} B")
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn human_units() {
        assert_eq!(human(0), "0 B");
        assert_eq!(human(1023), "1023 B");
        assert_eq!(human(1024), "1.0 KiB");
        assert_eq!(human(70 * 1024 * 1024), "70.0 MiB");
    }

    #[test]
    fn meter_reader_counts_bytes() {
        let data = vec![7u8; 4096];
        let meter = Handle::new("test", None, 0);
        let mut reader = meter.reader(&data[..]);
        let mut sink = Vec::new();
        let n = io::copy(&mut reader, &mut sink).unwrap();
        assert_eq!(n, 4096);
        assert_eq!(sink.len(), 4096);
    }
}

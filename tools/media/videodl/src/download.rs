//! Resumable byte-range download with retries and an MP4 sanity gate.

use crate::progress::{human, Handle};
use anyhow::{anyhow, bail, Context, Result};
use reqwest::blocking::{Client, Response};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

#[derive(Debug, Clone)]
pub struct Outcome {
    pub bytes: u64,
    pub skipped: bool,
}

/// Download `url` into `dest`, resuming from a leftover `.part` file.
///
/// The partial file survives failures, so a retry only re-fetches the missing
/// suffix instead of the whole video.
pub fn fetch(
    client: &Client,
    url: &str,
    dest: &Path,
    force: bool,
    retries: u32,
) -> Result<Outcome> {
    if dest.exists() && !force {
        let size = fs::metadata(dest)?.len();
        if size > 0 && verify_mp4(dest).is_ok() {
            return Ok(Outcome {
                bytes: size,
                skipped: true,
            });
        }
        // Broken leftover: start clean rather than appending to garbage.
        fs::remove_file(dest).ok();
    }
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)?;
    }
    let part = part_path(dest);
    let label = dest
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "download".into());

    let mut last_error: Option<anyhow::Error> = None;
    for attempt in 0..=retries {
        match attempt_once(client, url, &part, &label) {
            Ok(bytes) => {
                fs::rename(&part, dest)
                    .with_context(|| format!("rename {} -> {}", part.display(), dest.display()))?;
                verify_mp4(dest)?;
                return Ok(Outcome {
                    bytes,
                    skipped: false,
                });
            }
            Err(e) => {
                let resume_at = fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
                last_error = Some(e);
                if attempt < retries {
                    eprintln!(
                        "  retry {}/{} after error (resume at {}): {}",
                        attempt + 1,
                        retries,
                        human(resume_at),
                        last_error.as_ref().unwrap()
                    );
                    std::thread::sleep(Duration::from_millis(750 * (1 << attempt)));
                }
            }
        }
    }
    Err(last_error.unwrap_or_else(|| anyhow!("download failed without an error")))
}

fn attempt_once(client: &Client, url: &str, part: &Path, label: &str) -> Result<u64> {
    let existing = fs::metadata(part).map(|m| m.len()).unwrap_or(0);
    let mut req = client.get(url);
    if existing > 0 {
        req = req.header(reqwest::header::RANGE, format!("bytes={existing}-"));
    }
    let resp = req.send().with_context(|| format!("GET {url}"))?;
    let status = resp.status();

    if status == reqwest::StatusCode::RANGE_NOT_SATISFIABLE {
        // Nothing left to fetch: the part file is already the whole body.
        return Ok(existing);
    }
    if !status.is_success() {
        bail!("HTTP {status} for {url}");
    }

    let resumed = status == reqwest::StatusCode::PARTIAL_CONTENT && existing > 0;
    let total = total_size(&resp, if resumed { Some(existing) } else { None });
    let mut file = if resumed {
        OpenOptions::new().append(true).open(part)?
    } else {
        File::create(part)?
    };
    let start_offset = if resumed { existing } else { 0 };
    if !resumed {
        file.set_len(0).ok();
    }

    let meter = Handle::new(label, total, start_offset);
    let mut reader = resp;
    let mut buf = vec![0u8; 256 * 1024];
    let mut written = start_offset;
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n])?;
        written += n as u64;
        meter.add(n as u64);
    }
    file.flush()?;
    file.sync_all()?;
    meter.finish();

    if let Some(total) = total {
        if written != total {
            bail!("short read: wrote {written} of {total} bytes");
        }
    }
    Ok(written)
}

fn total_size(resp: &Response, offset: Option<u64>) -> Option<u64> {
    if let Some(cr) = resp
        .headers()
        .get(reqwest::header::CONTENT_RANGE)
        .and_then(|v| v.to_str().ok())
    {
        if let Some((_, total)) = cr.rsplit_once('/') {
            if let Ok(total) = total.trim().parse::<u64>() {
                return Some(total);
            }
        }
    }
    let len = resp
        .headers()
        .get(reqwest::header::CONTENT_LENGTH)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.parse::<u64>().ok())?;
    Some(len + offset.unwrap_or(0))
}

pub fn part_path(dest: &Path) -> PathBuf {
    let mut name = dest
        .file_name()
        .map(|n| n.to_os_string())
        .unwrap_or_default();
    name.push(".part");
    dest.with_file_name(name)
}

/// A file is accepted only when it carries an ISO-BMFF `ftyp` box close to the
/// head and is large enough to be a video, so a truncated or HTML error page
/// never lands in the drive.
pub fn verify_mp4(path: &Path) -> Result<()> {
    let size = fs::metadata(path)?.len();
    if size < 64 * 1024 {
        bail!("{} is only {} - refusing", path.display(), human(size));
    }
    let mut head = [0u8; 16];
    File::open(path)?.read_exact(&mut head)?;
    let brand_ok = &head[4..8] == b"ftyp";
    if !brand_ok {
        bail!("{} has no MP4 ftyp box", path.display());
    }
    Ok(())
}

/// HLS fallback: X only serves `m3u8` for some renditions; remux with ffmpeg.
pub fn fetch_hls(url: &str, dest: &Path, proxy: Option<&str>, force: bool) -> Result<Outcome> {
    if dest.exists() && !force && verify_mp4(dest).is_ok() {
        return Ok(Outcome {
            bytes: fs::metadata(dest)?.len(),
            skipped: true,
        });
    }
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut cmd = Command::new("ffmpeg");
    cmd.args(["-hide_banner", "-loglevel", "warning", "-y", "-i", url])
        .args(["-c", "copy", "-movflags", "+faststart"])
        .arg(dest);
    if let Some(proxy) = proxy {
        cmd.env("HTTP_PROXY", proxy)
            .env("HTTPS_PROXY", proxy)
            .env("http_proxy", proxy)
            .env("https_proxy", proxy);
    }
    let out = cmd
        .output()
        .context("ffmpeg is required for HLS-only renditions but could not be started")?;
    if !out.status.success() {
        bail!(
            "ffmpeg failed ({}): {}",
            out.status,
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    verify_mp4(dest)?;
    Ok(Outcome {
        bytes: fs::metadata(dest)?.len(),
        skipped: false,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn tmpdir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("videodl-test-{name}-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn part_path_appends_suffix() {
        assert_eq!(
            part_path(Path::new("/a/b.mp4")),
            PathBuf::from("/a/b.mp4.part")
        );
    }

    #[test]
    fn verify_rejects_tiny_and_html_files() {
        let dir = tmpdir("verify");
        let small = dir.join("small.mp4");
        fs::write(&small, b"ftyp").unwrap();
        assert!(verify_mp4(&small).is_err());

        let html = dir.join("page.mp4");
        let mut f = File::create(&html).unwrap();
        f.write_all(&vec![b'x'; 200 * 1024]).unwrap();
        assert!(verify_mp4(&html).is_err());

        let ok = dir.join("ok.mp4");
        let mut f = File::create(&ok).unwrap();
        f.write_all(&[0, 0, 0, 32]).unwrap();
        f.write_all(b"ftypisom").unwrap();
        f.write_all(&vec![0u8; 200 * 1024]).unwrap();
        assert!(verify_mp4(&ok).is_ok());
        fs::remove_dir_all(&dir).ok();
    }
}

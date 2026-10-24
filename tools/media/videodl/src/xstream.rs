//! Minimal client for an xstream instance + its ostore drive.
//!
//! Auth is a two-step handshake:
//!
//! ```text
//! cookie xstream_auth=ok  -->  GET /api/files/config
//!                                |  { base_url: "/ostore", token: "<bearer>" }
//!                                v
//!                          PUT /ostore/api/files/<path>   (stream, exact path)
//! ```
//!
//! The passcode printed in the web UI is derived from the date
//! (`rn` + YY + MM + DD + day%2), so it is checked locally as a
//! did-you-type-it-right gate rather than sent to the server.

use crate::progress::Handle;
use anyhow::{anyhow, bail, Context, Result};
use percent_encoding::{utf8_percent_encode, AsciiSet, NON_ALPHANUMERIC};
use reqwest::blocking::{Body, Client as HttpClient};
use reqwest::header::{ACCEPT, CONTENT_TYPE, COOKIE};
use serde::Deserialize;
use std::fs::{self, File};
use std::path::Path;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// Keep RFC 3986 unreserved characters readable in remote paths.
const SEG: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'-')
    .remove(b'.')
    .remove(b'_')
    .remove(b'~');

#[derive(Debug, Clone, Deserialize)]
pub struct Entry {
    pub path: String,
    #[serde(default)]
    pub size: u64,
    #[serde(rename = "type")]
    pub kind: String,
}

impl Entry {
    pub fn is_dir(&self) -> bool {
        self.kind == "dir"
    }
}

#[derive(Debug, Deserialize)]
struct ListResponse {
    #[serde(default)]
    entries: Vec<Entry>,
}

#[derive(Debug, Deserialize)]
struct ConfigResponse {
    base_url: String,
    token: String,
}

#[derive(Debug, Deserialize)]
pub struct PutResponse {
    pub path: String,
    #[serde(default)]
    pub size: u64,
    #[serde(default)]
    pub sha256: String,
}

pub struct Client {
    http: HttpClient,
    ostore: String,
    token: String,
}

impl Client {
    /// Fetch the ostore base URL + bearer token using the `xstream_auth` cookie.
    pub fn connect(base: &str, cookie: &str) -> Result<Self> {
        let base = base.trim_end_matches('/');
        // reqwest's blocking client defaults to a 30s *whole-request* timeout,
        // which aborts any upload bigger than ~100 MiB. Requests set their own
        // size-derived budget instead (see `put`), so disable the client default.
        let http = HttpClient::builder()
            .connect_timeout(Duration::from_secs(20))
            .timeout(None)
            .user_agent(format!("videodl/{}", env!("CARGO_PKG_VERSION")))
            .build()
            .context("build HTTP client")?;
        let url = format!("{base}/api/files/config");
        let resp = http
            .get(&url)
            .header(COOKIE, cookie)
            .header(ACCEPT, "application/json")
            .send()
            .with_context(|| format!("GET {url}"))?;
        let status = resp.status();
        let body = resp.text().unwrap_or_default();
        if status == reqwest::StatusCode::UNAUTHORIZED {
            bail!("{url} -> 401: cookie '{cookie}' rejected (passcode expired or rotated)");
        }
        if !status.is_success() {
            bail!("{url} -> HTTP {status}: {}", body.trim());
        }
        let cfg: ConfigResponse =
            serde_json::from_str(&body).with_context(|| format!("decode {url} response"))?;
        let ostore = if cfg.base_url.starts_with("http") {
            cfg.base_url.trim_end_matches('/').to_string()
        } else {
            format!("{base}{}", cfg.base_url.trim_end_matches('/'))
        };
        Ok(Self {
            http,
            ostore,
            token: cfg.token,
        })
    }

    pub fn list(&self, path: &str) -> Result<Vec<Entry>> {
        let url = format!(
            "{}/api/list?path={}",
            self.ostore,
            utf8_percent_encode(path.trim_matches('/'), SEG)
        );
        let resp = self
            .http
            .get(&url)
            .bearer_auth(&self.token)
            .header(ACCEPT, "application/json")
            .send()
            .with_context(|| format!("GET {url}"))?;
        let status = resp.status();
        let body = resp.text().unwrap_or_default();
        if status == reqwest::StatusCode::NOT_FOUND {
            return Ok(Vec::new());
        }
        if !status.is_success() {
            bail!("{url} -> HTTP {status}: {}", body.trim());
        }
        let parsed: ListResponse =
            serde_json::from_str(&body).with_context(|| format!("decode listing of {path}"))?;
        Ok(parsed.entries)
    }

    pub fn stat(&self, path: &str) -> Result<Option<Entry>> {
        let url = format!(
            "{}/api/stat?path={}",
            self.ostore,
            utf8_percent_encode(path.trim_matches('/'), SEG)
        );
        let resp = self
            .http
            .get(&url)
            .bearer_auth(&self.token)
            .header(ACCEPT, "application/json")
            .send()
            .with_context(|| format!("GET {url}"))?;
        if resp.status() == reqwest::StatusCode::NOT_FOUND {
            return Ok(None);
        }
        let status = resp.status();
        let body = resp.text().unwrap_or_default();
        if !status.is_success() {
            bail!("{url} -> HTTP {status}: {}", body.trim());
        }
        Ok(serde_json::from_str::<Entry>(&body).ok())
    }

    pub fn mkdir(&self, path: &str) -> Result<()> {
        let url = format!("{}/api/mkdir", self.ostore);
        let resp = self
            .http
            .post(&url)
            .bearer_auth(&self.token)
            .json(&serde_json::json!({ "path": path.trim_matches('/') }))
            .send()
            .with_context(|| format!("POST {url}"))?;
        let status = resp.status();
        if !status.is_success() {
            let body = resp.text().unwrap_or_default();
            bail!("mkdir {path} -> HTTP {status}: {}", body.trim());
        }
        Ok(())
    }

    /// Stream one local file to an exact remote path (parents auto-created).
    ///
    /// The per-request timeout is derived from the file size assuming a floor of
    /// 64 KiB/s, so a slow uplink is not punished while a dead connection still
    /// fails in bounded time. Failures retry from scratch (ostore has no
    /// resumable upload) after removing the partial object.
    pub fn put(&self, remote_path: &str, local: &Path, retries: u32) -> Result<PutResponse> {
        let size = fs::metadata(local)?.len();
        let budget = Duration::from_secs(60) + Duration::from_secs(size / (64 * 1024));
        let mut last = None;
        for attempt in 0..=retries {
            match self.put_once(remote_path, local, size, budget) {
                Ok(resp) => return Ok(resp),
                Err(e) => {
                    eprintln!(
                        "  upload attempt {}/{} failed: {e:#}",
                        attempt + 1,
                        retries + 1
                    );
                    last = Some(e);
                    // A half-written object must not survive a retry.
                    let _ = self.delete(remote_path);
                    if attempt < retries {
                        std::thread::sleep(Duration::from_millis(1_000 * (1 << attempt)));
                    }
                }
            }
        }
        Err(last.unwrap_or_else(|| anyhow!("upload failed without an error")))
    }

    fn put_once(
        &self,
        remote_path: &str,
        local: &Path,
        size: u64,
        budget: Duration,
    ) -> Result<PutResponse> {
        let url = format!(
            "{}/api/files/{}",
            self.ostore,
            utf8_percent_encode(remote_path.trim_matches('/'), SEG)
        );
        let file = File::open(local)?;
        let label = local
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| "upload".into());
        let meter = Handle::new(label, Some(size), 0);
        let body = Body::sized(meter.reader(file), size);
        let resp = self
            .http
            .put(&url)
            .bearer_auth(&self.token)
            .header(CONTENT_TYPE, "application/octet-stream")
            .header(ACCEPT, "application/json")
            .timeout(budget)
            .body(body)
            .send()
            .with_context(|| format!("PUT {url}"))?;
        let status = resp.status();
        let text = resp.text().unwrap_or_default();
        meter.finish();
        if !status.is_success() {
            bail!("upload {remote_path} -> HTTP {status}: {}", text.trim());
        }
        serde_json::from_str::<PutResponse>(&text)
            .with_context(|| format!("decode upload response: {}", text.trim()))
    }

    pub fn delete(&self, path: &str) -> Result<()> {
        let url = format!(
            "{}/api/files/{}",
            self.ostore,
            utf8_percent_encode(path.trim_matches('/'), SEG)
        );
        let resp = self
            .http
            .delete(&url)
            .bearer_auth(&self.token)
            .send()
            .with_context(|| format!("DELETE {url}"))?;
        let status = resp.status();
        if !status.is_success() {
            let body = resp.text().unwrap_or_default();
            bail!("delete {path} -> HTTP {status}: {}", body.trim());
        }
        Ok(())
    }
}

/// Join a remote directory and a file name without double slashes.
pub fn join_remote(dir: &str, name: &str) -> String {
    let dir = dir.trim_matches('/');
    if dir.is_empty() {
        name.to_string()
    } else {
        format!("{dir}/{}", name.trim_start_matches('/'))
    }
}

/// The passcode the xstream login page accepts on `date`: `rn` + YY + MM + DD + day%2.
pub fn daily_passcode(utc: (i32, u32, u32)) -> String {
    let (year, month, day) = utc;
    format!(
        "rn{:02}{:02}{:02}{}",
        year.rem_euclid(100),
        month,
        day,
        day % 2
    )
}

/// Current UTC date as (year, month, day), without pulling in a date crate.
pub fn utc_today() -> (i32, u32, u32) {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    civil_from_days((secs / 86_400) as i64)
}

/// Howard Hinnant's days-from-civil inverse.
fn civil_from_days(days: i64) -> (i32, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = if m <= 2 { y + 1 } else { y };
    (year as i32, m as u32, d as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn join_handles_slashes() {
        assert_eq!(join_remote("a/b", "c.mp4"), "a/b/c.mp4");
        assert_eq!(join_remote("/a/", "/c.mp4"), "a/c.mp4");
        assert_eq!(join_remote("", "c.mp4"), "c.mp4");
    }

    #[test]
    fn passcode_matches_ui_formula() {
        // The login page computed rn2610240 on 2026-10-24.
        assert_eq!(daily_passcode((2026, 10, 24)), "rn2610240");
        assert_eq!(daily_passcode((2026, 1, 3)), "rn2601031");
    }

    #[test]
    fn civil_from_days_known_dates() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(civil_from_days(20_000), (2024, 10, 4));
    }

    #[test]
    fn entry_type_mapping() {
        let e: Entry =
            serde_json::from_str(r#"{"name":"a","path":"d/a","size":3,"type":"file","mtime":"x"}"#)
                .unwrap();
        assert!(!e.is_dir());
        assert_eq!(e.size, 3);
    }

    #[test]
    fn path_encoding_keeps_cjk() {
        let enc = utf8_percent_encode("目录/视频 1.mp4", SEG).to_string();
        assert_eq!(enc, "%E7%9B%AE%E5%BD%95%2F%E8%A7%86%E9%A2%91%201.mp4");
        assert_eq!(
            utf8_percent_encode("a-b_c.mp4", SEG).to_string(),
            "a-b_c.mp4"
        );
    }
}

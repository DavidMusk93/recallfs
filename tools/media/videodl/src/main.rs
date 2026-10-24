//! videodl - download X/Twitter videos and push them to an xstream ostore drive.
//!
//! ```text
//! manifest (title | date | url)      video.twimg.com         xstream ostore
//!         |                                |                       |
//!         v                                v                       v
//!   resolve (fxtwitter) --variants--> fetch/resume --verify--> PUT /api/files/<path>
//! ```

mod download;
mod manifest;
mod progress;
mod twitter;
mod xstream;

use anyhow::{bail, Context, Result};
use clap::{Args, Parser, Subcommand};
use reqwest::blocking::Client;
use serde_json::json;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

const DEFAULT_BASE: &str = "https://cc.guohuasun.com";
const DEFAULT_COOKIE: &str = "xstream_auth=ok";

#[derive(Parser, Debug)]
#[command(
    name = "videodl",
    version,
    about = "Download X/Twitter videos and upload them to an xstream ostore drive",
    long_about = None
)]
struct Cli {
    /// HTTP/SOCKS proxy for downloads and metadata lookups
    #[arg(long, global = true, env = "VIDEODL_PROXY")]
    proxy: Option<String>,
    /// xstream base URL
    #[arg(long, global = true, default_value = DEFAULT_BASE)]
    base: String,
    /// xstream auth cookie value
    #[arg(long, global = true, default_value = DEFAULT_COOKIE)]
    cookie: String,
    /// xstream passcode (defaults to today's date-derived code)
    #[arg(long, global = true)]
    passcode: Option<String>,
    /// Per-request connect timeout in seconds
    #[arg(long, global = true, default_value_t = 20)]
    connect_timeout: u64,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Resolve post metadata and print the chosen rendition as JSON
    Resolve(TargetArgs),
    /// Download videos into a local directory
    Download(DownloadArgs),
    /// Download videos, then upload them to the drive
    Sync(SyncArgs),
    /// Upload existing local files
    Upload(UploadArgs),
    /// List a remote directory
    Ls {
        /// Directory on the drive
        #[arg(long, default_value = "")]
        path: String,
    },
    /// Delete remote paths (a directory only when it is already empty)
    Rm {
        /// Remote paths, e.g. `261024-shenqiao/bad.mp4`
        paths: Vec<String>,
    },
}

#[derive(Args, Debug)]
struct TargetArgs {
    /// Post URLs or numeric status ids
    targets: Vec<String>,
    /// Manifest files with `N. title | YYYY-MM-DD | url` lines
    #[arg(long, short = 'f')]
    manifest: Vec<PathBuf>,
}

#[derive(Args, Debug)]
struct DownloadArgs {
    targets: Vec<String>,
    #[arg(long, short = 'f')]
    manifest: Vec<PathBuf>,
    /// Local output directory
    #[arg(long, short = 'o', default_value = ".tmp/videos")]
    out: PathBuf,
    /// Skip renditions taller than this
    #[arg(long, default_value_t = 1080)]
    max_height: u32,
    /// Re-download and re-upload even if the artifacts already exist
    #[arg(long)]
    force: bool,
    /// Attempts per file
    #[arg(long, default_value_t = 3)]
    retries: u32,
}

#[derive(Args, Debug)]
struct SyncArgs {
    targets: Vec<String>,
    #[arg(long, short = 'f')]
    manifest: Vec<PathBuf>,
    #[arg(long, short = 'o', default_value = ".tmp/videos")]
    out: PathBuf,
    /// Remote directory on the drive
    #[arg(long)]
    remote_dir: String,
    #[arg(long, default_value_t = 1080)]
    max_height: u32,
    #[arg(long)]
    force: bool,
    #[arg(long, default_value_t = 3)]
    retries: u32,
}

#[derive(Args, Debug)]
struct UploadArgs {
    files: Vec<PathBuf>,
    #[arg(long)]
    remote_dir: String,
    #[arg(long)]
    force: bool,
}

#[derive(Debug)]
struct PlanItem {
    entry: Option<manifest::Entry>,
    url: String,
}

fn main() {
    if let Err(err) = run() {
        eprintln!("error: {err:#}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let cli = Cli::parse();
    let client = build_client(cli.proxy.as_deref(), cli.connect_timeout)?;

    match &cli.cmd {
        Cmd::Resolve(args) => {
            let items = collect_targets(&args.targets, &args.manifest)?;
            let mut out = Vec::new();
            let mut failed = 0usize;
            for item in items {
                let Some(id) = manifest::status_id(&item.url) else {
                    out.push(json!({ "url": item.url, "error": "no status id in url" }));
                    failed += 1;
                    continue;
                };
                let resolved = match twitter::resolve(&client, &id, 3) {
                    Ok(r) => r,
                    Err(e) => {
                        out.push(json!({
                            "status_id": id,
                            "url": item.url,
                            "error": format!("{e:#}"),
                        }));
                        failed += 1;
                        continue;
                    }
                };
                let variants: Vec<_> = resolved
                    .best_variants(None)
                    .into_iter()
                    .map(|(i, v)| {
                        json!({
                            "video": i + 1,
                            "container": match v.container {
                                twitter::Container::Mp4 => "mp4",
                                twitter::Container::Hls => "m3u8",
                            },
                            "height": v.height,
                            "bitrate": v.bitrate,
                            "url": v.url,
                        })
                    })
                    .collect();
                out.push(json!({
                    "status_id": resolved.id,
                    "author": resolved.author_handle,
                    "author_name": resolved.author_name,
                    "created": resolved.created_date,
                    "thumbnail": resolved.videos.first().and_then(|v| v.thumbnail.clone()),
                    "text": resolved.text,
                    "title": item.entry.as_ref().and_then(|e| e.title.clone()),
                    "date": item.entry.as_ref().and_then(|e| e.date.clone()),
                    "duration_s": resolved.videos.first().and_then(|v| v.duration_s),
                    "variants": variants,
                }));
            }
            println!("{}", serde_json::to_string_pretty(&out)?);
            if failed > 0 {
                bail!("{failed} target(s) could not be resolved");
            }
        }
        Cmd::Ls { path } => {
            let xs = connect_xstream(&cli)?;
            let entries = xs.list(path)?;
            if entries.is_empty() {
                println!("(empty) {}", xstream::join_remote(path, ""));
            }
            for e in entries {
                let kind = if e.is_dir() { "dir " } else { "file" };
                println!("{kind} {:>12}  {}", progress::human(e.size), e.path);
            }
        }
        Cmd::Rm { paths } => {
            let xs = connect_xstream(&cli)?;
            for path in paths {
                xs.delete(path)?;
                println!("deleted {path}");
            }
        }
        Cmd::Download(args) => {
            let items = collect_targets(&args.targets, &args.manifest)?;
            let report = download_all(
                &client,
                &items,
                &args.out,
                args.max_height,
                args.force,
                args.retries,
                cli.proxy.as_deref(),
            )?;
            print_table(&report.rows, None);
            report.exit_if_failed()?;
        }
        Cmd::Sync(args) => {
            let items = collect_targets(&args.targets, &args.manifest)?;
            let xs = connect_xstream(&cli)?;
            xs.mkdir(&args.remote_dir)?;
            let mut report = download_all(
                &client,
                &items,
                &args.out,
                args.max_height,
                args.force,
                args.retries,
                cli.proxy.as_deref(),
            )?;
            let mut uploaded = Vec::new();
            for row in &report.rows {
                let local = Path::new(&row.local);
                let name = local
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_default();
                let remote = xstream::join_remote(&args.remote_dir, &name);
                let existing = if args.force { None } else { xs.stat(&remote)? };
                if let Some(e) = &existing {
                    if e.size == row.bytes {
                        uploaded.push((name.clone(), remote.clone(), row.bytes, true));
                        continue;
                    }
                }
                match xs.put(&remote, local, args.retries) {
                    Ok(put) if put.size == row.bytes => {
                        uploaded.push((name, remote, put.size, false))
                    }
                    Ok(put) => {
                        report.failures.push((
                            row.id.clone(),
                            remote.clone(),
                            format!(
                                "server stored {} bytes, local file is {}",
                                put.size, row.bytes
                            ),
                        ));
                    }
                    Err(e) => {
                        report
                            .failures
                            .push((row.id.clone(), remote.clone(), format!("{e:#}")));
                    }
                }
            }
            print_table(&report.rows, Some(&uploaded));
            report.exit_if_failed()?;
        }
        Cmd::Upload(args) => {
            let xs = connect_xstream(&cli)?;
            xs.mkdir(&args.remote_dir)?;
            let mut verified = 0usize;
            for file in &args.files {
                if !file.exists() {
                    bail!("missing local file {}", file.display());
                }
                download::verify_mp4(file)?;
                let name = file
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_default();
                let remote = xstream::join_remote(&args.remote_dir, &name);
                let size = fs::metadata(file)?.len();
                if !args.force {
                    if let Some(e) = xs.stat(&remote)? {
                        if e.size == size {
                            println!("skip {remote} (already {} bytes)", size);
                            verified += 1;
                            continue;
                        }
                    }
                }
                let put = xs.put(&remote, file, 3)?;
                if put.size != size {
                    bail!(
                        "{remote}: server reports {} bytes, local {}",
                        put.size,
                        size
                    );
                }
                println!(
                    "put  {}  {}  sha256={}",
                    put.path,
                    progress::human(put.size),
                    if put.sha256.is_empty() {
                        "-"
                    } else {
                        &put.sha256
                    }
                );
                verified += 1;
            }
            println!("{verified} file(s) on the drive under {}", args.remote_dir);
        }
    }
    Ok(())
}

fn build_client(proxy: Option<&str>, connect_timeout: u64) -> Result<Client> {
    let mut builder = Client::builder()
        .connect_timeout(Duration::from_secs(connect_timeout))
        .user_agent(format!(
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) videodl/{}",
            env!("CARGO_PKG_VERSION")
        ));
    if let Some(proxy) = proxy {
        builder = builder
            .proxy(reqwest::Proxy::all(proxy).with_context(|| format!("invalid proxy {proxy}"))?);
    }
    builder.build().context("build HTTP client")
}

fn connect_xstream(cli: &Cli) -> Result<xstream::Client> {
    let today = xstream::utc_today();
    let expected = xstream::daily_passcode(today);
    match &cli.passcode {
        Some(given) if given != &expected => eprintln!(
            "note: passcode {given} != today's UI passcode {expected}; using the cookie as-is"
        ),
        None => eprintln!("note: today's UI passcode is {expected}"),
        _ => {}
    }
    xstream::Client::connect(&cli.base, &cli.cookie)
}

fn collect_targets(targets: &[String], manifests: &[PathBuf]) -> Result<Vec<PlanItem>> {
    let mut items = Vec::new();
    for m in manifests {
        let text =
            fs::read_to_string(m).with_context(|| format!("read manifest {}", m.display()))?;
        let parsed = manifest::parse(&text);
        if parsed.is_empty() {
            bail!("manifest {} has no usable entries", m.display());
        }
        for entry in parsed {
            items.push(PlanItem {
                url: entry.url.clone(),
                entry: Some(entry),
            });
        }
    }
    for t in targets {
        items.push(PlanItem {
            entry: None,
            url: t.clone(),
        });
    }
    if items.is_empty() {
        bail!("no targets: pass status URLs/ids or --manifest <file>");
    }
    Ok(items)
}

#[derive(Default)]
struct Report {
    rows: Vec<Row>,
    /// (label, source, reason) for every target that could not be fetched.
    failures: Vec<(String, String, String)>,
}

impl Report {
    fn exit_if_failed(&self) -> Result<()> {
        if self.failures.is_empty() {
            return Ok(());
        }
        eprintln!();
        eprintln!("{:<20} {:<60} reason", "status", "source");
        for (label, source, reason) in &self.failures {
            eprintln!("{label:<20} {source:<60} {reason}");
        }
        bail!(
            "{} target(s) failed; successful ones are already downloaded/uploaded",
            self.failures.len()
        )
    }
}

struct Row {
    id: String,
    title: String,
    local: String,
    bytes: u64,
    videos: usize,
    skipped: bool,
}

#[allow(clippy::too_many_arguments)]
fn download_all(
    client: &Client,
    items: &[PlanItem],
    out: &Path,
    max_height: u32,
    force: bool,
    retries: u32,
    proxy: Option<&str>,
) -> Result<Report> {
    fs::create_dir_all(out)?;
    let mut report = Report::default();
    for (idx, item) in items.iter().enumerate() {
        let Some(id) = manifest::status_id(&item.url) else {
            report.failures.push((
                "?".to_string(),
                item.url.clone(),
                "no status id in url".into(),
            ));
            continue;
        };
        let resolved = match twitter::resolve(client, &id, retries) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("[{}/{}] {id} unresolved: {e:#}", idx + 1, items.len());
                report
                    .failures
                    .push((id, item.url.clone(), format!("{e:#}")));
                continue;
            }
        };
        let stem = file_stem(item.entry.as_ref(), &resolved);
        let best = resolved.best_variants(Some(max_height));
        if best.is_empty() {
            report
                .failures
                .push((id, item.url.clone(), "no downloadable rendition".into()));
            continue;
        }
        println!(
            "[{}/{}] {id} {} - {} rendition(s)",
            idx + 1,
            items.len(),
            if resolved.text.is_empty() {
                stem.clone()
            } else {
                first_line(&resolved.text, 60)
            },
            best.len()
        );
        let mut total = 0u64;
        let mut skipped_all = true;
        for (n, (_video_idx, variant)) in best.iter().enumerate() {
            let dest = out.join(format!(
                "{}{}.mp4",
                stem,
                if n == 0 {
                    String::new()
                } else {
                    format!("-{}", n + 1)
                }
            ));
            let outcome = match variant.container {
                twitter::Container::Mp4 => {
                    download::fetch(client, &variant.url, &dest, force, retries)?
                }
                twitter::Container::Hls => download::fetch_hls(&variant.url, &dest, proxy, force)?,
            };
            total += outcome.bytes;
            skipped_all &= outcome.skipped;
            println!(
                "  {} {} {}",
                if outcome.skipped { "have" } else { "got " },
                dest.display(),
                progress::human(outcome.bytes)
            );
        }
        report.rows.push(Row {
            id,
            local: out
                .join(format!("{stem}.mp4"))
                .to_string_lossy()
                .to_string(),
            title: stem,
            bytes: total,
            videos: best.len(),
            skipped: skipped_all,
        });
    }
    Ok(report)
}

fn print_table(rows: &[Row], uploaded: Option<&Vec<(String, String, u64, bool)>>) {
    println!();
    println!(
        "{:<20} {:<44} {:>10} {:>4}  state",
        "status", "file", "size", "vids"
    );
    for row in rows {
        println!(
            "{:<20} {:<44} {:>10} {:>4}  {}",
            row.id,
            truncate_left(&row.title, 44),
            progress::human(row.bytes),
            row.videos,
            if row.skipped { "cached" } else { "downloaded" }
        );
    }
    if let Some(uploaded) = uploaded {
        println!();
        println!("{:<60} {:>10}  state", "remote path", "size");
        let mut by_dir: BTreeMap<String, usize> = BTreeMap::new();
        for (_, remote, size, skipped) in uploaded {
            *by_dir
                .entry(
                    remote
                        .rsplit_once('/')
                        .map(|(d, _)| d.to_string())
                        .unwrap_or_default(),
                )
                .or_default() += 1;
            println!(
                "{:<60} {:>10}  {}",
                remote,
                progress::human(*size),
                if *skipped {
                    "already there"
                } else {
                    "uploaded"
                }
            );
        }
        for (dir, count) in by_dir {
            println!("{dir}: {count} file(s)");
        }
    }
}

/// `01-title-20260830`, or `author-id` when no manifest metadata is available.
fn file_stem(entry: Option<&manifest::Entry>, resolved: &twitter::Resolved) -> String {
    let date = entry
        .and_then(|e| e.date.clone())
        .or_else(|| resolved.created_date.clone());
    let date_part = date.as_deref().map(compact_date);
    match entry {
        Some(e) if e.title.is_some() || e.index.is_some() => {
            let mut parts: Vec<String> = Vec::new();
            if let Some(index) = e.index {
                parts.push(format!("{index:02}"));
            }
            if let Some(title) = &e.title {
                parts.push(sanitize(title, 60));
            }
            if let Some(date) = date_part {
                parts.push(date);
            }
            if parts.is_empty() {
                fallback_stem(resolved)
            } else {
                parts.join("-")
            }
        }
        _ => {
            let mut stem = fallback_stem(resolved);
            if let Some(date) = date_part {
                stem = format!("{date}-{stem}");
            }
            stem
        }
    }
}

fn fallback_stem(resolved: &twitter::Resolved) -> String {
    let author = if resolved.author_handle.is_empty() {
        "x".to_string()
    } else {
        sanitize(&resolved.author_handle, 40)
    };
    sanitize(&format!("{author}-{}", resolved.id), 80)
}

fn compact_date(date: &str) -> String {
    date.chars().filter(|c| c.is_ascii_digit()).collect()
}

/// Drop path separators, quotes and control characters; keep CJK intact.
fn sanitize(input: &str, max_chars: usize) -> String {
    let mut out = String::with_capacity(input.len());
    let mut last_dash = false;
    for ch in input.chars() {
        let mapped = match ch {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' | '#' | '\u{7f}' => '-',
            c if c.is_control() => ' ',
            c => c,
        };
        if mapped == '-' {
            if last_dash || out.is_empty() {
                continue;
            }
            last_dash = true;
            out.push('-');
            continue;
        }
        last_dash = false;
        out.push(mapped);
    }
    let trimmed = out.trim().trim_matches('-').to_string();
    let limited: String = trimmed.chars().take(max_chars).collect();
    let limited = limited.trim_end().trim_end_matches('-').to_string();
    if limited.is_empty() {
        "video".to_string()
    } else {
        limited
    }
}

fn first_line(text: &str, max_chars: usize) -> String {
    let line = text.lines().next().unwrap_or("").trim();
    let cleaned: String = line
        .split_whitespace()
        .filter(|w| !w.starts_with("http"))
        .collect::<Vec<_>>()
        .join(" ");
    cleaned.chars().take(max_chars).collect()
}

fn truncate_left(s: &str, max_chars: usize) -> String {
    let chars: Vec<char> = s.chars().collect();
    if chars.len() <= max_chars {
        return s.to_string();
    }
    let tail: String = chars[chars.len() - (max_chars - 1)..].iter().collect();
    format!("…{tail}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use twitter::{Container, Resolved, Variant, VideoAsset};

    fn resolved(handle: &str, id: &str, date: Option<&str>) -> Resolved {
        Resolved {
            id: id.to_string(),
            author_handle: handle.to_string(),
            author_name: String::new(),
            created_date: date.map(str::to_string),
            text: String::new(),
            videos: vec![VideoAsset {
                thumbnail: None,
                duration_s: None,
                variants: vec![Variant {
                    url: "https://video.twimg.com/vid/avc1/1280x720/a.mp4".into(),
                    bitrate: 1,
                    container: Container::Mp4,
                    height: Some(720),
                }],
            }],
        }
    }

    #[test]
    fn stem_uses_manifest_metadata() {
        let entry = manifest::Entry {
            index: Some(3),
            title: Some("芭提雅的夜色".into()),
            date: Some("2026-08-27".into()),
            url: "https://x.com/u/status/9".into(),
        };
        let r = resolved("u", "9", Some("2026-08-27"));
        assert_eq!(file_stem(Some(&entry), &r), "03-芭提雅的夜色-20260827");
    }

    #[test]
    fn stem_falls_back_to_author_and_id() {
        let r = resolved("ShenqiaoQuella", "2093971248846143696", Some("2026-08-30"));
        assert_eq!(
            file_stem(None, &r),
            "20260830-ShenqiaoQuella-2093971248846143696"
        );
    }

    #[test]
    fn stem_without_date_or_handle() {
        let r = resolved("", "42", None);
        assert_eq!(file_stem(None, &r), "x-42");
    }

    #[test]
    fn sanitize_keeps_cjk_and_drops_path_chars() {
        assert_eq!(sanitize("a/b:c*d?\"e\"", 40), "a-b-c-d-e");
        assert_eq!(sanitize("芭提雅 泳池派对", 40), "芭提雅 泳池派对");
        assert_eq!(sanitize("///", 40), "video");
        assert_eq!(sanitize(&"x".repeat(100), 10), "xxxxxxxxxx");
    }

    #[test]
    fn compact_date_strips_separators() {
        assert_eq!(compact_date("2026-08-30"), "20260830");
    }

    #[test]
    fn first_line_strips_urls() {
        assert_eq!(
            first_line("我的作品《芭提雅泳池派对》全片 https://t.co/x", 40),
            "我的作品《芭提雅泳池派对》全片"
        );
    }

    #[test]
    fn manifests_and_targets_keep_order() {
        let dir = std::env::temp_dir().join(format!("videodl-manifest-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("list.md");
        std::fs::write(
            &file,
            "1. 甲 | 2026-01-01 | https://x.com/u/status/1\n2. 乙 | 2026-01-02 | https://x.com/u/status/2\n",
        )
        .unwrap();
        let items = collect_targets(&["https://x.com/u/status/3".to_string()], &[file]).unwrap();
        let urls: Vec<_> = items.iter().map(|i| i.url.as_str()).collect();
        assert_eq!(
            urls,
            vec![
                "https://x.com/u/status/1",
                "https://x.com/u/status/2",
                "https://x.com/u/status/3"
            ]
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn empty_targets_is_error() {
        assert!(collect_targets(&[], &[]).is_err());
    }
}

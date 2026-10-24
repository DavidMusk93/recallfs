//! Resolve an X/Twitter status into downloadable MP4 variants.
//!
//! X killed the unauthenticated `cdn.syndication.twimg.com` lookup (it now
//! returns `TweetTombstone`), so metadata comes from a public mirror:
//!
//! ```text
//! GET https://api.fxtwitter.com/status/<id>   -> tweet.media.videos[].formats[]
//!        | fallback
//! GET https://api.vxtwitter.com/Twitter/status/<id> -> mediaURLs[]
//! ```
//!
//! Mirrors only describe; bytes always come from `video.twimg.com`.

use anyhow::{anyhow, bail, Context, Result};
use reqwest::blocking::Client;
use serde_json::Value;
use std::time::Duration;

/// Where a variant lives; progressive MP4 is preferred, HLS is a last resort.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Container {
    Mp4,
    Hls,
}

#[derive(Debug, Clone)]
pub struct Variant {
    pub url: String,
    pub bitrate: u64,
    pub container: Container,
    pub height: Option<u32>,
}

#[derive(Debug, Clone)]
pub struct VideoAsset {
    pub thumbnail: Option<String>,
    pub duration_s: Option<f64>,
    pub variants: Vec<Variant>,
}

#[derive(Debug, Clone)]
pub struct Resolved {
    pub id: String,
    pub author_handle: String,
    pub author_name: String,
    pub created_date: Option<String>,
    pub text: String,
    pub videos: Vec<VideoAsset>,
}

/// One rendition advertised by an HLS master playlist.
#[derive(Debug, Clone)]
pub struct HlsRendition {
    pub url: String,
    pub height: Option<u32>,
    pub bandwidth: u64,
    pub audio_group: Option<String>,
}

/// One audio group advertised by an HLS master playlist.
#[derive(Debug, Clone)]
pub struct HlsAudio {
    pub group: String,
    pub url: String,
}

#[derive(Debug, Clone, Default)]
pub struct HlsMaster {
    pub renditions: Vec<HlsRendition>,
    pub audios: Vec<HlsAudio>,
}

impl HlsMaster {
    pub fn best(&self, max_height: Option<u32>) -> Option<&HlsRendition> {
        self.renditions
            .iter()
            .filter(|r| max_height.is_none_or(|cap| r.height.is_none_or(|h| h <= cap)))
            .max_by_key(|r| (r.height.unwrap_or(0), r.bandwidth))
    }

    pub fn audio_for(&self, rendition: &HlsRendition) -> Option<&HlsAudio> {
        let group = rendition.audio_group.as_deref()?;
        self.audios.iter().find(|a| a.group == group)
    }
}

/// What we actually download for one video.
#[derive(Debug, Clone)]
pub enum Choice {
    Mp4(Variant),
    Hls(HlsRendition),
}

impl Choice {
    pub fn height(&self) -> Option<u32> {
        match self {
            Choice::Mp4(v) => v.height,
            Choice::Hls(r) => r.height,
        }
    }

    pub fn bandwidth(&self) -> u64 {
        match self {
            Choice::Mp4(v) => v.bitrate,
            Choice::Hls(r) => r.bandwidth,
        }
    }

    pub fn label(&self) -> String {
        let kind = match self {
            Choice::Mp4(_) => "mp4",
            Choice::Hls(_) => "hls",
        };
        format!(
            "{kind} {} @ {:.2} Mbps",
            self.height()
                .map(|h| format!("{h}p"))
                .unwrap_or_else(|| "?".into()),
            self.bandwidth() as f64 / 1_000_000.0
        )
    }
}

/// Choose the best rendition per video across both transport lists.
///
/// X publishes the same encode ladder twice (progressive MP4 + HLS). HLS is
/// used only when it is strictly taller, because it costs an ffmpeg remux; this
/// keeps the common case a single plain MP4 download.
pub fn choose(
    video: &VideoAsset,
    master: Option<&HlsMaster>,
    max_height: Option<u32>,
) -> Option<Choice> {
    let mp4 = pick_variant(video, max_height).filter(|v| v.container == Container::Mp4);
    let hls = master.and_then(|m| m.best(max_height));
    match (mp4, hls) {
        (Some(mp4), Some(hls)) => {
            if hls.height.unwrap_or(0) > mp4.height.unwrap_or(0) {
                Some(Choice::Hls(hls.clone()))
            } else {
                Some(Choice::Mp4(mp4.clone()))
            }
        }
        (Some(mp4), None) => Some(Choice::Mp4(mp4.clone())),
        (None, Some(hls)) => Some(Choice::Hls(hls.clone())),
        (None, None) => None,
    }
}

/// Fetch and parse an HLS master playlist.
pub fn fetch_hls_master(client: &Client, master_url: &str) -> Result<HlsMaster> {
    let body = get_text(client, master_url, 2)?;
    let base = reqwest::Url::parse(master_url).context("parse master playlist url")?;
    let mut master = HlsMaster::default();
    let mut pending: Option<(Option<u32>, u64, Option<String>)> = None;
    for line in body.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("#EXT-X-MEDIA:") {
            let attrs = parse_attrs(rest);
            if attrs.get("TYPE").map(String::as_str) == Some("AUDIO") {
                if let (Some(group), Some(uri)) = (attrs.get("GROUP-ID"), attrs.get("URI")) {
                    master.audios.push(HlsAudio {
                        group: group.clone(),
                        url: join_url(&base, uri),
                    });
                }
            }
            continue;
        }
        if let Some(rest) = line.strip_prefix("#EXT-X-STREAM-INF:") {
            let attrs = parse_attrs(rest);
            let height = attrs
                .get("RESOLUTION")
                .and_then(|r| r.split_once('x').map(|(_, h)| h.to_string()))
                .and_then(|h| h.parse::<u32>().ok());
            let bandwidth = attrs
                .get("AVERAGE-BANDWIDTH")
                .or_else(|| attrs.get("BANDWIDTH"))
                .and_then(|b| b.parse::<u64>().ok())
                .unwrap_or(0);
            pending = Some((height, bandwidth, attrs.get("AUDIO").cloned()));
            continue;
        }
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        if let Some((height, bandwidth, audio_group)) = pending.take() {
            master.renditions.push(HlsRendition {
                url: join_url(&base, line),
                height,
                bandwidth,
                audio_group,
            });
        }
    }
    if master.renditions.is_empty() {
        bail!("no renditions in HLS master {master_url}");
    }
    Ok(master)
}

fn parse_attrs(input: &str) -> std::collections::HashMap<String, String> {
    let mut out = std::collections::HashMap::new();
    let mut key = String::new();
    let mut value = String::new();
    let mut in_value = false;
    let mut in_quotes = false;
    for ch in input.chars() {
        match ch {
            '"' => in_quotes = !in_quotes,
            '=' if !in_value && !in_quotes => {
                in_value = true;
            }
            ',' if !in_quotes => {
                if !key.is_empty() {
                    out.insert(key.trim().to_string(), value.trim().to_string());
                }
                key.clear();
                value.clear();
                in_value = false;
            }
            c => {
                if in_value {
                    value.push(c);
                } else {
                    key.push(c);
                }
            }
        }
    }
    if !key.is_empty() {
        out.insert(key.trim().to_string(), value.trim().to_string());
    }
    out
}

fn join_url(base: &reqwest::Url, reference: &str) -> String {
    base.join(reference)
        .map(|u| u.to_string())
        .unwrap_or_else(|_| reference.to_string())
}

fn get_text(client: &Client, url: &str, retries: u32) -> Result<String> {
    let mut last = None;
    for attempt in 0..=retries {
        match client.get(url).send() {
            Ok(resp) => {
                let status = resp.status();
                let body = resp.text().unwrap_or_default();
                if status.is_success() {
                    return Ok(body);
                }
                last = Some(anyhow!("{url} -> HTTP {status}"));
                if status.is_client_error() && status.as_u16() != 429 {
                    break;
                }
            }
            Err(e) => last = Some(anyhow!("{url}: {e}")),
        }
        if attempt < retries {
            std::thread::sleep(Duration::from_millis(400 * (1 << attempt)));
        }
    }
    Err(last.unwrap_or_else(|| anyhow!("{url}: no attempt made")))
}

/// Prefer the tallest MP4 that stays within `max_height`, breaking ties on
/// bitrate. HLS only wins when no progressive MP4 variant exists at all,
/// because HLS needs an external remuxer.
pub fn pick_variant(video: &VideoAsset, max_height: Option<u32>) -> Option<&Variant> {
    let mp4: Vec<&Variant> = video
        .variants
        .iter()
        .filter(|v| v.container == Container::Mp4)
        .collect();
    let eligible: Vec<&Variant> = match max_height {
        Some(cap) => mp4
            .iter()
            .copied()
            .filter(|v| v.height.is_none_or(|h| h <= cap))
            .collect(),
        None => mp4.clone(),
    };
    let pool = if eligible.is_empty() { mp4 } else { eligible };
    if let Some(best) = pool
        .iter()
        .max_by_key(|v| (v.height.unwrap_or(0), v.bitrate))
        .copied()
    {
        return Some(best);
    }
    video
        .variants
        .iter()
        .find(|v| v.container == Container::Hls)
}

/// Fetch metadata for one status id.
pub fn resolve(client: &Client, status_id: &str, retries: u32) -> Result<Resolved> {
    let primary = format!("https://api.fxtwitter.com/status/{status_id}");
    match get_json(client, &primary, retries).and_then(|v| parse_fx(&v)) {
        Ok(r) => Ok(r),
        Err(err) => {
            let fallback = format!("https://api.vxtwitter.com/Twitter/status/{status_id}");
            get_json(client, &fallback, retries)
                .and_then(|v| parse_vx(&v))
                .map_err(|e2| {
                    anyhow!("metadata lookup failed; fxtwitter: {err:#}; vxtwitter: {e2:#}")
                })
        }
    }
}

fn get_json(client: &Client, url: &str, retries: u32) -> Result<Value> {
    let mut last = None;
    for attempt in 0..=retries {
        match client.get(url).send() {
            Ok(resp) => {
                let status = resp.status();
                let body = resp.text().unwrap_or_default();
                if status.is_success() {
                    return serde_json::from_str(&body)
                        .with_context(|| format!("invalid JSON from {url}"));
                }
                last = Some(anyhow!("{url} -> HTTP {status}: {}", truncate(&body, 200)));
                // 4xx (except 429) will not get better by retrying.
                if status.is_client_error() && status.as_u16() != 429 {
                    break;
                }
            }
            Err(e) => last = Some(anyhow!("{url}: {e}")),
        }
        if attempt < retries {
            std::thread::sleep(Duration::from_millis(500 * (1 << attempt)));
        }
    }
    Err(last.unwrap_or_else(|| anyhow!("{url}: no attempt made")))
}

fn parse_fx(v: &Value) -> Result<Resolved> {
    if let Some(code) = v.get("code").and_then(Value::as_i64) {
        if code != 200 {
            bail!(
                "fxtwitter code {code}: {}",
                v.get("message").and_then(Value::as_str).unwrap_or("")
            );
        }
    }
    let tweet = v
        .get("tweet")
        .ok_or_else(|| anyhow!("no tweet in response"))?;
    let id = tweet
        .get("id")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let author = tweet.get("author");
    let author_handle = author
        .and_then(|a| a.get("screen_name"))
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let author_name = author
        .and_then(|a| a.get("name"))
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let text = tweet
        .get("text")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let created_date = tweet
        .get("created_at")
        .and_then(Value::as_str)
        .and_then(parse_created_date);

    let mut videos = Vec::new();
    let raw_videos = tweet
        .get("media")
        .and_then(|m| m.get("videos"))
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    for raw in raw_videos {
        let duration_s = raw.get("duration").and_then(Value::as_f64);
        let thumbnail = raw
            .get("thumbnail_url")
            .and_then(Value::as_str)
            .map(str::to_string);
        let mut variants = Vec::new();
        for f in raw
            .get("formats")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default()
        {
            let Some(url) = f.get("url").and_then(Value::as_str) else {
                continue;
            };
            let container = match url.split('?').next().unwrap_or("") {
                u if u.ends_with(".m3u8") => Container::Hls,
                u if u.ends_with(".mp4") => Container::Mp4,
                _ => continue,
            };
            variants.push(Variant {
                height: height_from_url(url),
                bitrate: f.get("bitrate").and_then(Value::as_u64).unwrap_or(0),
                container,
                url: url.to_string(),
            });
        }
        // `formats` and `variants` describe the same renditions; keep one copy.
        variants.dedup_by(|a, b| a.url == b.url);
        if !variants.is_empty() {
            videos.push(VideoAsset {
                thumbnail,
                duration_s,
                variants,
            });
        }
    }
    if videos.is_empty() {
        bail!("tweet {id} has no video media");
    }
    Ok(Resolved {
        id,
        author_handle,
        author_name,
        created_date,
        text,
        videos,
    })
}

fn parse_vx(v: &Value) -> Result<Resolved> {
    let id = v
        .get("tweetID")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let text = v
        .get("text")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let urls: Vec<&str> = v
        .get("mediaURLs")
        .and_then(Value::as_array)
        .map(|a| a.iter().filter_map(Value::as_str).collect())
        .unwrap_or_default();
    let ext = v.get("media_extended").and_then(Value::as_array);
    let mut videos = Vec::new();
    for (i, url) in urls.iter().enumerate() {
        let is_video = url.contains("/video.twimg.com/") || url.ends_with(".m3u8");
        if !is_video {
            continue;
        }
        let container = if url.contains(".m3u8") {
            Container::Hls
        } else {
            Container::Mp4
        };
        let duration_s = ext
            .and_then(|a| a.get(i))
            .and_then(|m| m.get("duration_millis"))
            .and_then(Value::as_i64)
            .map(|ms| ms as f64 / 1000.0);
        let thumbnail = ext
            .and_then(|a| a.get(i))
            .and_then(|m| m.get("thumbnail_url"))
            .and_then(Value::as_str)
            .map(str::to_string);
        videos.push(VideoAsset {
            thumbnail,
            duration_s,
            variants: vec![Variant {
                height: height_from_url(url),
                bitrate: 1,
                container,
                url: url.to_string(),
            }],
        });
    }
    if videos.is_empty() {
        bail!("vxtwitter response carries no video URL");
    }
    Ok(Resolved {
        id,
        author_handle: String::new(),
        author_name: String::new(),
        created_date: None,
        text,
        videos,
    })
}

/// `/vid/avc1/1280x720/abc.mp4` -> 720
pub fn height_from_url(url: &str) -> Option<u32> {
    let path = url.split('?').next().unwrap_or(url);
    for seg in path.split('/') {
        if let Some((w, h)) = seg.split_once('x') {
            if w.chars().all(|c| c.is_ascii_digit()) && h.chars().all(|c| c.is_ascii_digit()) {
                if let (Ok(w), Ok(h)) = (w.parse::<u32>(), h.parse::<u32>()) {
                    if w >= 100 && h >= 100 {
                        return Some(h);
                    }
                }
            }
        }
    }
    None
}

/// `Sun Aug 30 07:57:04 +0000 2026` -> `2026-08-30`
pub fn parse_created_date(raw: &str) -> Option<String> {
    let parts: Vec<&str> = raw.split_whitespace().collect();
    if parts.len() < 4 {
        return None;
    }
    let month = match parts[1] {
        "Jan" => "01",
        "Feb" => "02",
        "Mar" => "03",
        "Apr" => "04",
        "May" => "05",
        "Jun" => "06",
        "Jul" => "07",
        "Aug" => "08",
        "Sep" => "09",
        "Oct" => "10",
        "Nov" => "11",
        "Dec" => "12",
        _ => return None,
    };
    let day: u32 = parts[2].parse().ok()?;
    let year: u32 = parts.last()?.parse().ok()?;
    Some(format!("{year:04}-{month}-{day:02}"))
}

fn truncate(s: &str, n: usize) -> String {
    s.chars().take(n).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn asset(variants: Vec<Variant>) -> VideoAsset {
        VideoAsset {
            thumbnail: None,
            duration_s: None,
            variants,
        }
    }

    fn mp4(url: &str, bitrate: u64) -> Variant {
        Variant {
            url: url.to_string(),
            bitrate,
            container: Container::Mp4,
            height: height_from_url(url),
        }
    }

    #[test]
    fn height_parsing() {
        assert_eq!(
            height_from_url(
                "https://video.twimg.com/amplify_video/1/vid/avc1/1280x720/a.mp4?tag=29"
            ),
            Some(720)
        );
        assert_eq!(height_from_url("https://video.twimg.com/x/pl/a.m3u8"), None);
    }

    #[test]
    fn picks_tallest_mp4_under_cap() {
        let v = asset(vec![
            mp4("https://video.twimg.com/vid/avc1/480x270/a.mp4", 256_000),
            mp4("https://video.twimg.com/vid/avc1/1280x720/b.mp4", 2_176_000),
            mp4(
                "https://video.twimg.com/vid/avc1/1920x1080/c.mp4",
                4_000_000,
            ),
        ]);
        assert_eq!(pick_variant(&v, None).unwrap().height, Some(1080));
        assert_eq!(pick_variant(&v, Some(720)).unwrap().height, Some(720));
    }

    #[test]
    fn falls_back_to_hls_only_when_no_mp4() {
        let v = asset(vec![Variant {
            url: "https://video.twimg.com/v/pl/a.m3u8".into(),
            bitrate: 0,
            container: Container::Hls,
            height: None,
        }]);
        assert_eq!(pick_variant(&v, None).unwrap().container, Container::Hls);
    }

    #[test]
    fn cap_larger_than_available_keeps_best() {
        let v = asset(vec![mp4(
            "https://video.twimg.com/vid/avc1/640x360/a.mp4",
            832_000,
        )]);
        assert_eq!(pick_variant(&v, Some(2160)).unwrap().height, Some(360));
    }

    #[test]
    fn parses_hls_master_attrs() {
        let text = concat!(
            "#EXTM3U\n",
            "#EXT-X-MEDIA:NAME=\"Audio\",TYPE=AUDIO,GROUP-ID=\"audio-128000\",AUTOSELECT=YES,URI=\"/a/pl/mp4a/128k/x.m3u8\"\n",
            "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=1022814,BANDWIDTH=1844582,RESOLUTION=1280x720,CODECS=\"mp4a.40.2,avc1.640020\",AUDIO=\"audio-128000\"\n",
            "/a/pl/avc1/1280x720/v.m3u8\n"
        );
        let base = reqwest::Url::parse("https://video.twimg.com/amplify_video/1/pl/m.m3u8?tag=29")
            .unwrap();
        let mut master = HlsMaster::default();
        let mut pending = None;
        for line in text.lines() {
            if let Some(rest) = line.strip_prefix("#EXT-X-MEDIA:") {
                let a = parse_attrs(rest);
                if a.get("TYPE").map(String::as_str) == Some("AUDIO") {
                    master.audios.push(HlsAudio {
                        group: a["GROUP-ID"].clone(),
                        url: join_url(&base, &a["URI"]),
                    });
                }
            } else if let Some(rest) = line.strip_prefix("#EXT-X-STREAM-INF:") {
                let a = parse_attrs(rest);
                let h = a["RESOLUTION"]
                    .split_once('x')
                    .map(|(_, h)| h.parse::<u32>().unwrap());
                pending = Some((
                    h,
                    a["AVERAGE-BANDWIDTH"].parse::<u64>().unwrap(),
                    a.get("AUDIO").cloned(),
                ));
            } else if !line.starts_with('#') && !line.is_empty() {
                let (height, bandwidth, audio_group) = pending.take().unwrap();
                master.renditions.push(HlsRendition {
                    url: join_url(&base, line),
                    height,
                    bandwidth,
                    audio_group,
                });
            }
        }
        assert_eq!(master.audios.len(), 1);
        assert_eq!(master.audios[0].group, "audio-128000");
        assert!(master.audios[0].url.ends_with("/a/pl/mp4a/128k/x.m3u8"));
        let best = master.best(None).unwrap();
        assert_eq!(best.height, Some(720));
        assert_eq!(best.bandwidth, 1_022_814);
        assert_eq!(master.audio_for(best).unwrap().group, "audio-128000");
    }

    #[test]
    fn choose_prefers_taller_hls_only() {
        let v = asset(vec![mp4(
            "https://video.twimg.com/v/vid/avc1/1280x720/a.mp4",
            2_176_000,
        )]);
        let same_ladder = HlsMaster {
            renditions: vec![HlsRendition {
                url: "https://video.twimg.com/v/pl/avc1/1280x720/x.m3u8".into(),
                height: Some(720),
                bandwidth: 1_022_814,
                audio_group: None,
            }],
            audios: vec![],
        };
        assert!(matches!(
            choose(&v, Some(&same_ladder), None),
            Some(Choice::Mp4(_))
        ));

        let taller = HlsMaster {
            renditions: vec![HlsRendition {
                url: "https://video.twimg.com/v/pl/avc1/1920x1080/x.m3u8".into(),
                height: Some(1080),
                bandwidth: 5_000_000,
                audio_group: None,
            }],
            audios: vec![],
        };
        let picked = choose(&v, Some(&taller), None).unwrap();
        assert!(matches!(picked, Choice::Hls(_)));
        assert_eq!(picked.height(), Some(1080));
    }

    #[test]
    fn choose_respects_height_cap() {
        let v = asset(vec![mp4(
            "https://video.twimg.com/v/vid/avc1/1280x720/a.mp4",
            2_176_000,
        )]);
        let taller = HlsMaster {
            renditions: vec![HlsRendition {
                url: "https://video.twimg.com/v/pl/avc1/1920x1080/x.m3u8".into(),
                height: Some(1080),
                bandwidth: 5_000_000,
                audio_group: None,
            }],
            audios: vec![],
        };
        assert!(matches!(
            choose(&v, Some(&taller), Some(720)),
            Some(Choice::Mp4(_))
        ));
    }

    #[test]
    fn created_date_parsing() {
        assert_eq!(
            parse_created_date("Sun Aug 30 07:57:04 +0000 2026").as_deref(),
            Some("2026-08-30")
        );
        assert_eq!(parse_created_date("nonsense"), None);
    }

    #[test]
    fn parses_fx_payload() {
        let payload = serde_json::json!({
            "code": 200,
            "message": "OK",
            "tweet": {
                "id": "2093971248846143696",
                "text": "我的作品《芭提雅泳池派对》全片",
                "created_at": "Sun Aug 30 07:57:04 +0000 2026",
                "author": {"screen_name": "ShenqiaoQuella", "name": "Miss Shen"},
                "media": {"videos": [{
                    "duration": 258.166,
                    "formats": [
                        {"url": "https://video.twimg.com/a/pl/x.m3u8?tag=29", "container": "m3u8"},
                        {"url": "https://video.twimg.com/a/vid/avc1/640x360/b.mp4?tag=29", "bitrate": 832000, "container": "mp4"},
                        {"url": "https://video.twimg.com/a/vid/avc1/1280x720/c.mp4?tag=29", "bitrate": 2176000, "container": "mp4"}
                    ]
                }]}
            }
        });
        let r = parse_fx(&payload).unwrap();
        assert_eq!(r.author_handle, "ShenqiaoQuella");
        assert_eq!(r.created_date.as_deref(), Some("2026-08-30"));
        assert_eq!(r.videos.len(), 1);
        let best = choose(&r.videos[0], None, None).expect("choice");
        assert_eq!(best.height(), Some(720));
        assert!(matches!(best, Choice::Mp4(_)));
    }

    #[test]
    fn fx_payload_without_video_is_error() {
        let payload = serde_json::json!({"code": 200, "tweet": {"id": "1", "text": "hi"}});
        assert!(parse_fx(&payload).is_err());
    }
}

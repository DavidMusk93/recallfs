//! Download one public Douyin video with the local Chrome login cookies.
//! Default cap is 1080 lines. `ratio=default` is the full master and is not
//! requested unless `--max-height` is raised above 1080.

use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

const UA: &str = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36";
const SESSION_NAMES: &[&str] = &[
    "sessionid",
    "sessionid_ss",
    "sid_guard",
    "sid_tt",
    "uid_tt",
    "uid_tt_ss",
    "ttwid",
    "odin_tt",
    "passport_csrf_token",
    "UIFID",
    "s_v_web_id",
];

fn main() {
    if let Err(err) = run() {
        eprintln!("douyin-dl: {err}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), String> {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.iter().any(|a| a == "--self-test") {
        self_test()?;
        println!("self-test ok");
        return Ok(());
    }
    if args.iter().any(|a| a == "--check-cookies") {
        let cookies = load_chrome_cookies()?;
        let session = cookies.iter().any(|c| c.name == "sessionid" && c.value.len() > 8);
        let ttwid = cookies.iter().any(|c| c.name == "ttwid" && c.value.len() > 8);
        println!(
            "chrome douyin cookies: {}  sessionid: {}  ttwid: {}",
            cookies.len(),
            if session { "yes" } else { "no" },
            if ttwid { "yes" } else { "no" }
        );
        if !session {
            return Err("Chrome 里没有抖音 sessionid。先在 Chrome 登录 douyin.com".into());
        }
        return Ok(());
    }

    let mut url = None;
    let mut output: Option<PathBuf> = None;
    let mut max_height: u32 = 1080;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-o" => {
                i += 1;
                output = Some(PathBuf::from(args.get(i).ok_or("`-o` 缺少路径")?));
            }
            "--max-height" => {
                i += 1;
                max_height = args
                    .get(i)
                    .ok_or("`--max-height` 缺少数字")?
                    .parse()
                    .map_err(|_| "`--max-height` 不是整数")?;
            }
            other if other.starts_with('-') => return Err(format!("未知参数 {other}")),
            other => url = Some(other.to_string()),
        }
        i += 1;
    }
    let url = url.ok_or("用法: douyin-dl <视频 URL 或 aweme id> [-o 文件] [--max-height 1080]")?;
    let aweme = aweme_id(&url).ok_or("认不出 aweme id。传 douyin.com/video/<数字> 或纯数字 id")?;

    let cookies = load_chrome_cookies()?;
    if !cookies.iter().any(|c| c.name == "sessionid" && !c.value.is_empty()) {
        return Err("Chrome 里没有抖音 sessionid。先在 Chrome 登录 douyin.com".into());
    }

    let tmp = TempDir::new()?;
    let jar = tmp.path.join("cookies.txt");
    write_cookie_jar(&jar, &cookies)?;

    let detail_path = tmp.path.join("detail.json");
    let detail_url = format!(
        "https://aweme.snssdk.com/aweme/v1/aweme/detail/?aweme_id={aweme}&aid=1128&device_platform=android"
    );
    curl_to(
        &detail_url,
        &detail_path,
        &jar,
        &format!("https://www.douyin.com/video/{aweme}"),
        &[],
    )?;
    let detail = fs::read_to_string(&detail_path).map_err(|e| e.to_string())?;
    let video = video_id(&detail).ok_or("详情里没有 video id")?;
    let title = file_title(&json_field(&detail, "desc").unwrap_or_default());
    let dest = output.unwrap_or_else(|| default_output(&title, &aweme));
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }

    let mut best: Option<Probe> = None;
    for ratio in ratios_for(max_height) {
        let play = format!(
            "https://aweme.snssdk.com/aweme/v1/play/?video_id={video}&ratio={ratio}&line=0"
        );
        match probe_ratio(&tmp, &jar, &aweme, &play, ratio) {
            Ok(hit) => {
                println!(
                    "probe {ratio}: {}x{}  {} bytes",
                    hit.width, hit.height, hit.total_bytes
                );
                if hit.height == 0 || hit.height > max_height {
                    println!("  跳过：高度 {} 超过上限 {max_height}", hit.height);
                    continue;
                }
                let replace = match &best {
                    None => true,
                    Some(cur) => (hit.height, hit.width) > (cur.height, cur.width),
                };
                if replace {
                    best = Some(hit);
                }
            }
            Err(err) => println!("probe {ratio}: {err}"),
        }
    }
    let best = best.ok_or(format!("没有高度不超过 {max_height} 的成片"))?;
    println!(
        "选择 {}  {}x{}  {} bytes",
        best.ratio, best.width, best.height, best.total_bytes
    );

    let part = dest.with_extension("mp4.part");
    if best.complete_path.is_some() && best.total_bytes == best.sample_len {
        fs::copy(best.complete_path.as_ref().unwrap(), &part).map_err(|e| e.to_string())?;
    } else {
        curl_to(
            &best.url,
            &part,
            &jar,
            &format!("https://www.douyin.com/video/{aweme}"),
            &["--retry", "3", "--retry-delay", "1"],
        )?;
    }
    let (w, h) = read_mp4_size(&part)?.ok_or("下到的文件里没有视频轨")?;
    if h == 0 || h > max_height {
        let _ = fs::remove_file(&part);
        return Err(format!("成片是 {w}x{h}，超过 --max-height {max_height}，已删除"));
    }
    fs::rename(&part, &dest).map_err(|e| e.to_string())?;
    let bytes = fs::metadata(&dest).map_err(|e| e.to_string())?.len();
    println!("{}x{}  {bytes} bytes", w, h);
    println!("{}", dest.display());
    Ok(())
}

struct Probe {
    ratio: String,
    url: String,
    width: u32,
    height: u32,
    total_bytes: u64,
    sample_len: u64,
    complete_path: Option<PathBuf>,
}

fn probe_ratio(tmp: &TempDir, jar: &Path, aweme: &str, play: &str, ratio: &str) -> Result<Probe, String> {
    let head = tmp.path.join(format!("head-{ratio}.bin"));
    let headers = tmp.path.join(format!("head-{ratio}.hdr"));
    let mut args = vec![
        "-sS".into(),
        "-L".into(),
        "--fail".into(),
        "--range".into(),
        "0-2097151".into(),
        "--dump-header".into(),
        headers.display().to_string(),
        "--output".into(),
        head.display().to_string(),
        "-A".into(),
        UA.into(),
        "-e".into(),
        format!("https://www.douyin.com/video/{aweme}"),
        "-b".into(),
        jar.display().to_string(),
        "-w".into(),
        "\n%{url_effective}\n%{http_code}\n".into(),
    ];
    // The unlabeled master is about 1.2GB. Never pull it while probing.
    if ratio == "default" {
        args.extend(["--max-filesize".into(), "8388608".into()]);
    }
    args.push(play.to_string());
    let (code, stdout, stderr) = run_cmd("curl", &args.iter().map(String::as_str).collect::<Vec<_>>())?;
    if code != 0 {
        return Err(format!("curl {code}: {}", stderr.trim()));
    }
    let sample_len = fs::metadata(&head).map_err(|e| e.to_string())?.len();
    if sample_len > 8 * 1024 * 1024 {
        let _ = fs::remove_file(&head);
        return Err("Range 没有生效，探针超过 8MB，已丢弃".into());
    }
    let hdr = fs::read_to_string(&headers).unwrap_or_default();
    let total_bytes = last_content_range_total(&hdr).unwrap_or(sample_len);
    let (width, height) = read_mp4_size(&head)?.unwrap_or((0, 0));
    let url = stdout
        .lines()
        .map(str::trim)
        .filter(|l| l.starts_with("http"))
        .last()
        .unwrap_or(play)
        .to_string();
    let complete = sample_len == total_bytes && total_bytes > 0;
    Ok(Probe {
        ratio: ratio.to_string(),
        url,
        width,
        height,
        total_bytes,
        sample_len,
        complete_path: if complete { Some(head) } else { None },
    })
}

fn ratios_for(max_height: u32) -> Vec<&'static str> {
    if max_height > 1080 {
        vec!["default", "1080p", "720p", "540p"]
    } else {
        vec!["1080p", "720p", "540p"]
    }
}

fn default_output(title: &str, aweme: &str) -> PathBuf {
    let home = env::var("HOME").unwrap_or_else(|_| ".".into());
    PathBuf::from(home)
        .join("Downloads")
        .join(format!("{title}-{aweme}-1080p.mp4"))
}

fn aweme_id(input: &str) -> Option<String> {
    if let Some(rest) = input.split("/video/").nth(1) {
        let id: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
        if id.len() >= 8 {
            return Some(id);
        }
    }
    let trimmed = input.trim();
    if trimmed.len() >= 8 && trimmed.chars().all(|c| c.is_ascii_digit()) {
        return Some(trimmed.to_string());
    }
    None
}

fn file_title(desc: &str) -> String {
    let line = desc.lines().next().unwrap_or("").trim();
    let cut = line.split(['，', ',', '。']).next().unwrap_or(line).trim();
    let raw: String = cut.chars().take(40).collect();
    let cleaned: String = raw
        .chars()
        .map(|c| match c {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' => '_',
            c if c.is_control() => '_',
            c => c,
        })
        .collect();
    let cleaned = cleaned.trim().trim_matches('.').to_string();
    if cleaned.is_empty() {
        "douyin".into()
    } else {
        cleaned
    }
}

fn json_field(json: &str, key: &str) -> Option<String> {
    let pat = format!("\"{key}\":\"");
    let start = json.find(&pat)? + pat.len();
    parse_json_string(&json[start..]).map(|(s, _)| s)
}

fn video_id(json: &str) -> Option<String> {
    let key = "\"url_key\":\"";
    let start = json.find(key)? + key.len();
    let (raw, _) = parse_json_string(&json[start..])?;
    let id = raw.split('_').next().unwrap_or("");
    if id.starts_with('v') && id.len() > 8 {
        Some(id.to_string())
    } else {
        None
    }
}

fn parse_json_string(input: &str) -> Option<(String, usize)> {
    let b = input.as_bytes();
    let mut i = 0;
    let mut out = String::new();
    while i < b.len() {
        match b[i] {
            b'"' => return Some((out, i + 1)),
            b'\\' => {
                i += 1;
                if i >= b.len() {
                    return None;
                }
                match b[i] {
                    b'"' => out.push('"'),
                    b'\\' => out.push('\\'),
                    b'/' => out.push('/'),
                    b'n' => out.push('\n'),
                    b'r' => out.push('\r'),
                    b't' => out.push('\t'),
                    b'u' => {
                        if i + 5 > b.len() {
                            return None;
                        }
                        let hex = std::str::from_utf8(&b[i + 1..i + 5]).ok()?;
                        let cp = u32::from_str_radix(hex, 16).ok()?;
                        out.push(char::from_u32(cp)?);
                        i += 4;
                    }
                    c => out.push(c as char),
                }
                i += 1;
            }
            _ => {
                let rest = std::str::from_utf8(&b[i..]).ok()?;
                let ch = rest.chars().next()?;
                out.push(ch);
                i += ch.len_utf8();
            }
        }
    }
    None
}

fn last_content_range_total(headers: &str) -> Option<u64> {
    let mut found = None;
    for line in headers.lines() {
        let lower = line.to_ascii_lowercase();
        let Some(rest) = lower.strip_prefix("content-range:") else {
            continue;
        };
        let rest = rest.trim();
        let Some(total) = rest.rsplit('/').next() else {
            continue;
        };
        if let Ok(n) = total.trim().parse::<u64>() {
            found = Some(n);
        }
    }
    found
}

fn read_mp4_size(path: &Path) -> Result<Option<(u32, u32)>, String> {
    let mut file = File::open(path).map_err(|e| e.to_string())?;
    let mut buf = Vec::new();
    (&mut file).take(2 * 1024 * 1024).read_to_end(&mut buf).map_err(|e| e.to_string())?;
    if let Some(wh) = tkhd_size(&buf) {
        return Ok(Some(wh));
    }
    let len = fs::metadata(path).map_err(|e| e.to_string())?.len();
    if len > 2 * 1024 * 1024 {
        let start = len - 2 * 1024 * 1024;
        let mut tail = vec![0u8; (len - start) as usize];
        use std::io::{Seek, SeekFrom};
        file.seek(SeekFrom::Start(start)).map_err(|e| e.to_string())?;
        file.read_exact(&mut tail).map_err(|e| e.to_string())?;
        if let Some(wh) = tkhd_size(&tail) {
            return Ok(Some(wh));
        }
    }
    Ok(None)
}

fn tkhd_size(data: &[u8]) -> Option<(u32, u32)> {
    let mut best = None;
    let mut start = 0;
    while let Some(rel) = data[start..].windows(4).position(|w| w == b"tkhd") {
        let i = start + rel;
        if data.len() >= i + 88 && data[i + 4] == 0 {
            let w = u32::from_be_bytes(data[i + 80..i + 84].try_into().ok()?) >> 16;
            let h = u32::from_be_bytes(data[i + 84..i + 88].try_into().ok()?) >> 16;
            if w > 0 && h > 0 && best.map(|(bw, bh)| w * h > bw * bh).unwrap_or(true) {
                best = Some((w, h));
            }
        } else if data.len() >= i + 100 && data[i + 4] == 1 {
            let w = u32::from_be_bytes(data[i + 92..i + 96].try_into().ok()?) >> 16;
            let h = u32::from_be_bytes(data[i + 96..i + 100].try_into().ok()?) >> 16;
            if w > 0 && h > 0 && best.map(|(bw, bh)| w * h > bw * bh).unwrap_or(true) {
                best = Some((w, h));
            }
        }
        start = i + 4;
    }
    best
}

struct Cookie {
    domain: String,
    name: String,
    value: String,
}

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new() -> Result<Self, String> {
        let n = SystemTime::now().duration_since(UNIX_EPOCH).map_err(|e| e.to_string())?.as_nanos();
        let path = env::temp_dir().join(format!("douyin-dl-{n}"));
        fs::create_dir_all(&path).map_err(|e| e.to_string())?;
        Ok(Self { path })
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn chrome_cookie_db() -> Result<PathBuf, String> {
    let home = env::var("HOME").map_err(|_| "没有 HOME")?;
    let p = PathBuf::from(home).join("Library/Application Support/Google/Chrome/Default/Cookies");
    if p.is_file() {
        Ok(p)
    } else {
        Err(format!("找不到 Chrome Cookie 库 {}", p.display()))
    }
}

fn load_chrome_cookies() -> Result<Vec<Cookie>, String> {
    let src = chrome_cookie_db()?;
    let tmp = TempDir::new()?;
    let copy = tmp.path.join("Cookies");
    // Chrome keeps the latest rows in the WAL beside the database. A plain
    // copy of the main file misses the login that just happened.
    let (code, _, stderr) = run_cmd(
        "sqlite3",
        &[&src.display().to_string(), &format!(".backup {}", copy.display())],
    )?;
    if code != 0 {
        return Err(format!("复制 Cookie 库失败: {}", stderr.trim()));
    }
    let meta = sqlite_one(
        &copy,
        "SELECT value FROM meta WHERE key = 'version'",
    )?;
    let version: i64 = meta.trim().parse().unwrap_or(0);
    let sql = "SELECT host_key, name, hex(encrypted_value) FROM cookies \
               WHERE host_key LIKE '%douyin%' OR host_key LIKE '%douyinvod%' \
               OR host_key LIKE '%snssdk%' OR host_key LIKE '%iesdouyin%' OR host_key LIKE '%amemv%'";
    let rows = sqlite_query(&copy, sql)?;
    let password = chrome_safe_storage_password()?;
    let key = pbkdf2_sha1_16(&password, b"saltysalt", 1003);
    let mut out = Vec::new();
    let mut saw_v10 = 0u32;
    let mut decrypted = 0u32;
    for row in &rows {
        let mut parts = row.split("@@@");
        let domain = parts.next().unwrap_or("").to_string();
        let name = parts.next().unwrap_or("").to_string();
        let hex_enc = parts.next().unwrap_or("");
        if domain.is_empty() || name.is_empty() || hex_enc.is_empty() {
            continue;
        }
        let raw = decode_hex(hex_enc).map_err(|e| format!("cookie hex: {e}"))?;
        if raw.starts_with(b"v10") {
            saw_v10 += 1;
        }
        let value = match decrypt_chrome_cookie(&raw, &key, version >= 24) {
            Some(v) => {
                decrypted += 1;
                v
            }
            None => continue,
        };
        if value.is_empty() || value.contains(['\t', '\n', '\r']) {
            continue;
        }
        out.push(Cookie {
            domain,
            name,
            value,
        });
    }
    if out.is_empty() {
        return Err(format!(
            "Chrome 里没有解出抖音 Cookie（行 {}，v10 {saw_v10}，解出 {decrypted}，库版本 {version}）",
            rows.len()
        ));
    }
    Ok(out)
}

fn chrome_safe_storage_password() -> Result<Vec<u8>, String> {
    let (code, stdout, _) = run_cmd(
        "security",
        &[
            "find-generic-password",
            "-w",
            "-a",
            "Chrome",
            "-s",
            "Chrome Safe Storage",
        ],
    )?;
    if code != 0 {
        return Err("读不到 Chrome Safe Storage。需要本机钥匙串授权".into());
    }
    let mut bytes = stdout.into_bytes();
    while bytes.last().is_some_and(|b| *b == b'\n' || *b == b'\r') {
        bytes.pop();
    }
    if bytes.is_empty() {
        return Err("Chrome Safe Storage 密码是空的".into());
    }
    Ok(bytes)
}

fn decrypt_chrome_cookie(encrypted: &[u8], key: &[u8; 16], hash_prefix: bool) -> Option<String> {
    let ct = encrypted.strip_prefix(b"v10")?;
    if ct.len() % 16 != 0 || ct.is_empty() {
        return None;
    }
    let iv = [b' '; 16];
    let plain = aes128_cbc_decrypt(ct, key, &iv)?;
    let plain = unpad_pkcs7(&plain)?;
    let bytes = if hash_prefix {
        plain.get(32..)?
    } else {
        plain.as_slice()
    };
    String::from_utf8(bytes.to_vec()).ok()
}

fn write_cookie_jar(path: &Path, cookies: &[Cookie]) -> Result<(), String> {
    let mut lines = String::from("# Netscape HTTP Cookie File\n");
    let mut seen = std::collections::BTreeSet::new();
    let mirrors = [".douyin.com", ".snssdk.com", ".douyinvod.com", ".iesdouyin.com"];
    let emit = |lines: &mut String, seen: &mut std::collections::BTreeSet<(String, String)>, domain: &str, name: &str, value: &str| {
        let key = (domain.to_string(), name.to_string());
        if !seen.insert(key) {
            return;
        }
        let flag = if domain.starts_with('.') { "TRUE" } else { "FALSE" };
        lines.push_str(&format!("{domain}\t{flag}\t/\tTRUE\t0\t{name}\t{value}\n"));
    };
    for c in cookies {
        emit(&mut lines, &mut seen, &c.domain, &c.name, &c.value);
        if SESSION_NAMES.contains(&c.name.as_str()) {
            for domain in mirrors {
                emit(&mut lines, &mut seen, domain, &c.name, &c.value);
            }
        }
    }
    let mut file = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .mode(0o600)
        .open(path)
        .map_err(|e| e.to_string())?;
    file.write_all(lines.as_bytes()).map_err(|e| e.to_string())?;
    Ok(())
}

fn sqlite_one(db: &Path, sql: &str) -> Result<String, String> {
    let rows = sqlite_query(db, sql)?;
    Ok(rows.into_iter().next().unwrap_or_default())
}

fn sqlite_query(db: &Path, sql: &str) -> Result<Vec<String>, String> {
    let (code, stdout, stderr) = run_cmd(
        "sqlite3",
        &["-batch", "-separator", "@@@", &db.display().to_string(), sql],
    )?;
    if code != 0 {
        return Err(format!("sqlite3: {}", stderr.trim()));
    }
    Ok(stdout.lines().map(|s| s.to_string()).filter(|s| !s.is_empty()).collect())
}

fn curl_to(url: &str, dest: &Path, jar: &Path, referer: &str, extra: &[&str]) -> Result<(), String> {
    let mut args = vec![
        "-L".to_string(),
        "--fail".into(),
        "-A".into(),
        UA.into(),
        "-e".into(),
        referer.into(),
        "-b".into(),
        jar.display().to_string(),
        "-o".into(),
        dest.display().to_string(),
    ];
    args.extend(extra.iter().map(|s| (*s).to_string()));
    args.push(url.to_string());
    let refs: Vec<&str> = args.iter().map(String::as_str).collect();
    let (code, _, stderr) = run_cmd("curl", &refs)?;
    if code != 0 {
        return Err(format!("curl {code}: {}", stderr.trim()));
    }
    Ok(())
}

fn run_cmd(bin: &str, args: &[&str]) -> Result<(i32, String, String), String> {
    let out = Command::new(bin)
        .args(args)
        .output()
        .map_err(|e| format!("无法运行 {bin}: {e}"))?;
    let code = out.status.code().unwrap_or(1);
    let stdout = String::from_utf8_lossy(&out.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&out.stderr).into_owned();
    Ok((code, stdout, stderr))
}

fn decode_hex(s: &str) -> Result<Vec<u8>, String> {
    if s.len() % 2 != 0 {
        return Err("奇数长度".into());
    }
    let mut out = Vec::with_capacity(s.len() / 2);
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        let hi = hex_val(b[i])?;
        let lo = hex_val(b[i + 1])?;
        out.push((hi << 4) | lo);
        i += 2;
    }
    Ok(out)
}

fn hex_val(b: u8) -> Result<u8, String> {
    match b {
        b'0'..=b'9' => Ok(b - b'0'),
        b'a'..=b'f' => Ok(b - b'a' + 10),
        b'A'..=b'F' => Ok(b - b'A' + 10),
        _ => Err("非法 hex".into()),
    }
}

fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h = [
        0x67452301u32,
        0xEFCDAB89,
        0x98BADCFE,
        0x10325476,
        0xC3D2E1F0,
    ];
    let bit_len = (data.len() as u64).saturating_mul(8);
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bit_len.to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[i * 4..i * 4 + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for i in 0..80 {
            let (f, k) = if i < 20 {
                ((b & c) | ((!b) & d), 0x5A827999)
            } else if i < 40 {
                (b ^ c ^ d, 0x6ED9EBA1)
            } else if i < 60 {
                ((b & c) | (b & d) | (c & d), 0x8F1BBCDC)
            } else {
                (b ^ c ^ d, 0xCA62C1D6)
            };
            let temp = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(w[i]);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = temp;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }
    let mut out = [0u8; 20];
    for (i, word) in h.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

fn hmac_sha1(key: &[u8], msg: &[u8]) -> [u8; 20] {
    let mut k = [0u8; 64];
    if key.len() > 64 {
        k[..20].copy_from_slice(&sha1(key));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let mut ipad = [0u8; 64];
    let mut opad = [0u8; 64];
    for i in 0..64 {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    let mut inner = Vec::with_capacity(64 + msg.len());
    inner.extend_from_slice(&ipad);
    inner.extend_from_slice(msg);
    let ih = sha1(&inner);
    let mut outer = Vec::with_capacity(84);
    outer.extend_from_slice(&opad);
    outer.extend_from_slice(&ih);
    sha1(&outer)
}

fn pbkdf2_sha1_16(password: &[u8], salt: &[u8], iters: u32) -> [u8; 16] {
    let mut block = Vec::with_capacity(salt.len() + 4);
    block.extend_from_slice(salt);
    block.extend_from_slice(&1u32.to_be_bytes());
    let mut u = hmac_sha1(password, &block);
    let mut t = u;
    for _ in 1..iters {
        u = hmac_sha1(password, &u);
        for i in 0..20 {
            t[i] ^= u[i];
        }
    }
    let mut out = [0u8; 16];
    out.copy_from_slice(&t[..16]);
    out
}

const SBOX: [u8; 256] = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
];

const INV_SBOX: [u8; 256] = [
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
];

fn gmul(mut a: u8, mut b: u8) -> u8 {
    let mut p = 0u8;
    for _ in 0..8 {
        if b & 1 != 0 {
            p ^= a;
        }
        let hi = a & 0x80;
        a = a.wrapping_shl(1);
        if hi != 0 {
            a ^= 0x1b;
        }
        b >>= 1;
    }
    p
}

fn expand_key(key: &[u8; 16]) -> [[u8; 16]; 11] {
    let mut w = [0u32; 44];
    for i in 0..4 {
        w[i] = u32::from_be_bytes(key[i * 4..i * 4 + 4].try_into().unwrap());
    }
    const RCON: [u32; 10] = [
        0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x20000000, 0x40000000, 0x80000000,
        0x1b000000, 0x36000000,
    ];
    for i in 4..44 {
        let mut temp = w[i - 1];
        if i % 4 == 0 {
            let rot = temp.rotate_left(8);
            let b = rot.to_be_bytes();
            let sub = u32::from_be_bytes([
                SBOX[b[0] as usize],
                SBOX[b[1] as usize],
                SBOX[b[2] as usize],
                SBOX[b[3] as usize],
            ]);
            temp = sub ^ RCON[i / 4 - 1];
        }
        w[i] = w[i - 4] ^ temp;
    }
    let mut out = [[0u8; 16]; 11];
    for r in 0..11 {
        for c in 0..4 {
            out[r][c * 4..c * 4 + 4].copy_from_slice(&w[r * 4 + c].to_be_bytes());
        }
    }
    out
}

fn add_round(state: &mut [u8; 16], key: &[u8; 16]) {
    for i in 0..16 {
        state[i] ^= key[i];
    }
}

fn shift_rows(state: &mut [u8; 16]) {
    for r in 1..4 {
        let mut row = [0u8; 4];
        for c in 0..4 {
            row[c] = state[r + 4 * c];
        }
        for c in 0..4 {
            state[r + 4 * c] = row[(c + r) % 4];
        }
    }
}

fn inv_shift_rows(state: &mut [u8; 16]) {
    for r in 1..4 {
        let mut row = [0u8; 4];
        for c in 0..4 {
            row[c] = state[r + 4 * c];
        }
        for c in 0..4 {
            state[r + 4 * c] = row[(c + 4 - r) % 4];
        }
    }
}

fn mix_columns(state: &mut [u8; 16]) {
    for c in 0..4 {
        let i = c * 4;
        let t = [state[i], state[i + 1], state[i + 2], state[i + 3]];
        state[i] = gmul(t[0], 2) ^ gmul(t[1], 3) ^ t[2] ^ t[3];
        state[i + 1] = t[0] ^ gmul(t[1], 2) ^ gmul(t[2], 3) ^ t[3];
        state[i + 2] = t[0] ^ t[1] ^ gmul(t[2], 2) ^ gmul(t[3], 3);
        state[i + 3] = gmul(t[0], 3) ^ t[1] ^ t[2] ^ gmul(t[3], 2);
    }
}

fn inv_mix_columns(state: &mut [u8; 16]) {
    for c in 0..4 {
        let i = c * 4;
        let t = [state[i], state[i + 1], state[i + 2], state[i + 3]];
        state[i] = gmul(t[0], 14) ^ gmul(t[1], 11) ^ gmul(t[2], 13) ^ gmul(t[3], 9);
        state[i + 1] = gmul(t[0], 9) ^ gmul(t[1], 14) ^ gmul(t[2], 11) ^ gmul(t[3], 13);
        state[i + 2] = gmul(t[0], 13) ^ gmul(t[1], 9) ^ gmul(t[2], 14) ^ gmul(t[3], 11);
        state[i + 3] = gmul(t[0], 11) ^ gmul(t[1], 13) ^ gmul(t[2], 9) ^ gmul(t[3], 14);
    }
}

fn aes128_encrypt_block(block: &[u8; 16], key: &[u8; 16]) -> [u8; 16] {
    let rk = expand_key(key);
    let mut state = *block;
    add_round(&mut state, &rk[0]);
    for round in 1..10 {
        for b in &mut state {
            *b = SBOX[*b as usize];
        }
        shift_rows(&mut state);
        mix_columns(&mut state);
        add_round(&mut state, &rk[round]);
    }
    for b in &mut state {
        *b = SBOX[*b as usize];
    }
    shift_rows(&mut state);
    add_round(&mut state, &rk[10]);
    state
}

fn aes128_decrypt_block(block: &[u8; 16], key: &[u8; 16]) -> [u8; 16] {
    let rk = expand_key(key);
    let mut state = *block;
    add_round(&mut state, &rk[10]);
    for round in (1..10).rev() {
        inv_shift_rows(&mut state);
        for b in &mut state {
            *b = INV_SBOX[*b as usize];
        }
        add_round(&mut state, &rk[round]);
        inv_mix_columns(&mut state);
    }
    inv_shift_rows(&mut state);
    for b in &mut state {
        *b = INV_SBOX[*b as usize];
    }
    add_round(&mut state, &rk[0]);
    state
}

fn aes128_cbc_decrypt(data: &[u8], key: &[u8; 16], iv: &[u8; 16]) -> Option<Vec<u8>> {
    if data.len() % 16 != 0 {
        return None;
    }
    let mut prev = *iv;
    let mut out = Vec::with_capacity(data.len());
    for chunk in data.chunks(16) {
        let block: [u8; 16] = chunk.try_into().ok()?;
        let dec = aes128_decrypt_block(&block, key);
        for i in 0..16 {
            out.push(dec[i] ^ prev[i]);
        }
        prev = block;
    }
    Some(out)
}

fn unpad_pkcs7(data: &[u8]) -> Option<Vec<u8>> {
    let n = *data.last()? as usize;
    if n == 0 || n > 16 || n > data.len() {
        return None;
    }
    if data[data.len() - n..].iter().any(|b| *b as usize != n) {
        return None;
    }
    Some(data[..data.len() - n].to_vec())
}

fn self_test() -> Result<(), String> {
    let empty = sha1(b"");
    check_hex(&empty, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 empty")?;
    check_hex(&sha1(b"abc"), "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 abc")?;
    let dk = pbkdf2_sha1_16(b"peanuts", b"saltysalt", 1003);
    check_hex(&dk, "d9a09d499b4e1b7461f28e67972c6dbd", "pbkdf2")?;
    let key = [
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    ];
    let pt = [
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    ];
    let ct = aes128_encrypt_block(&pt, &key);
    check_hex(&ct, "69c4e0d86a7b0430d8cdb78070b4c55a", "aes encrypt")?;
    let back = aes128_decrypt_block(&ct, &key);
    if back != pt {
        return Err("aes decrypt".into());
    }
    let mut padded = pt.to_vec();
    padded.extend_from_slice(&[16; 16]);
    let iv = [0x10u8; 16];
    let mut cbc = Vec::new();
    let mut prev = iv;
    for chunk in padded.chunks(16) {
        let mut block = [0u8; 16];
        for i in 0..16 {
            block[i] = chunk[i] ^ prev[i];
        }
        let enc = aes128_encrypt_block(&block, &key);
        cbc.extend_from_slice(&enc);
        prev = enc;
    }
    let dec = aes128_cbc_decrypt(&cbc, &key, &iv).ok_or("cbc")?;
    let unp = unpad_pkcs7(&dec).ok_or("pkcs7")?;
    if unp != pt {
        return Err("cbc roundtrip".into());
    }
    let id = aweme_id("https://www.douyin.com/video/7696114106826280242?x=1").ok_or("id")?;
    if id != "7696114106826280242" {
        return Err("aweme id".into());
    }
    if file_title("人生不是每句都是punchline，后面不要") != "人生不是每句都是punchline" {
        return Err("title".into());
    }
    if ratios_for(1080).contains(&"default") || !ratios_for(2160).contains(&"default") {
        return Err("ratios".into());
    }
    let sample = json_field(r#"{"desc":"hello\n世界"}"#, "desc").ok_or("json")?;
    if sample != "hello\n世界" {
        return Err(format!("json string {sample:?}"));
    }
    let vid = video_id(r#"{"url_key":"v0d00fg10000db70rdnog65gre8p9n4g_h264_540p_1"}"#).ok_or("vid")?;
    if vid != "v0d00fg10000db70rdnog65gre8p9n4g" {
        return Err("video id".into());
    }
    let mut box_bytes = vec![0u8; 92];
    box_bytes[4..8].copy_from_slice(b"tkhd");
    box_bytes[84..88].copy_from_slice(&(1920u32 << 16).to_be_bytes());
    box_bytes[88..92].copy_from_slice(&(1080u32 << 16).to_be_bytes());
    if tkhd_size(&box_bytes) != Some((1920, 1080)) {
        return Err("tkhd".into());
    }
    if last_content_range_total("HTTP/1.1 206\r\nContent-Range: bytes 0-10/57\r\n") != Some(57) {
        return Err("range".into());
    }
    Ok(())
}

fn check_hex(bytes: &[u8], expect: &str, label: &str) -> Result<(), String> {
    let got: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
    if got != expect {
        return Err(format!("{label}: {got} != {expect}"));
    }
    Ok(())
}

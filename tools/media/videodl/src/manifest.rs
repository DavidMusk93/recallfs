//! Manifest parsing: a human-curated list of video posts.
//!
//! The list format is intentionally loose so a note copied out of a chat still
//! works. Each non-empty line is one entry:
//!
//! ```text
//! 1. 芭提雅泳池派对 | 2026-08-30 | 约4:18 | https://x.com/user/status/123
//! https://x.com/user/status/456
//! ```
//!
//! * index   -> leading `N.` / `N)` / `N、`
//! * title   -> first `|` field that is not a date and not the URL
//! * date    -> first `YYYY-MM-DD` field
//! * url     -> first `http(s)://` token on the line

/// One line of a manifest.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub index: Option<u32>,
    pub title: Option<String>,
    pub date: Option<String>,
    pub url: String,
}

/// Parse a manifest into entries, dropping blank lines and comments.
pub fn parse(text: &str) -> Vec<Entry> {
    text.lines().filter_map(parse_line).collect::<Vec<_>>()
}

fn parse_line(raw: &str) -> Option<Entry> {
    let line = raw.trim();
    if line.is_empty() || line.starts_with('#') || line.starts_with("//") {
        return None;
    }

    let url = extract_url(line)?;
    let fields: Vec<&str> = line.split('|').map(str::trim).collect();

    let date = fields.iter().find_map(|f| {
        if is_date(f) {
            Some((*f).to_string())
        } else {
            None
        }
    });

    // The first field is the leading "N. title" chunk. A field is a candidate
    // title when it is neither the URL, nor a date, nor a pure duration field.
    let mut index = None;
    let mut title = None;
    for (pos, field) in fields.iter().enumerate() {
        let mut field = *field;
        if pos == 0 {
            if let Some((n, rest)) = split_index(field) {
                index = Some(n);
                field = rest;
            }
        }
        if field.is_empty() || field.contains("http") || is_date(field) || is_duration(field) {
            continue;
        }
        title = Some(field.trim().to_string());
        break;
    }

    Some(Entry {
        index,
        title,
        date,
        url,
    })
}

/// `12. foo` / `12) foo` / `12、foo` -> (12, "foo")
fn split_index(field: &str) -> Option<(u32, &str)> {
    let digits: String = field.chars().take_while(|c| c.is_ascii_digit()).collect();
    if digits.is_empty() {
        return None;
    }
    let rest = &field[digits.len()..];
    let rest = rest
        .strip_prefix('.')
        .or_else(|| rest.strip_prefix(')'))
        .or_else(|| rest.strip_prefix('、'))
        .or_else(|| rest.strip_prefix('．'))
        .or_else(|| rest.strip_prefix('）'))?;
    let n: u32 = digits.parse().ok()?;
    Some((n, rest.trim()))
}

fn extract_url(line: &str) -> Option<String> {
    let start = line.find("http://").or_else(|| line.find("https://"))?;
    let tail = &line[start..];
    let end = tail
        .find(|c: char| {
            c.is_whitespace() || matches!(c, '|' | '）' | ')' | '，' | ',' | '。' | ']' | '>')
        })
        .unwrap_or(tail.len());
    let url = tail[..end].trim_end_matches(['.', ',', ';']).to_string();
    if url.len() > 8 {
        Some(url)
    } else {
        None
    }
}

fn is_date(field: &str) -> bool {
    let b = field.as_bytes();
    b.len() == 10
        && b[4] == b'-'
        && b[7] == b'-'
        && b[..4].iter().all(u8::is_ascii_digit)
        && b[5..7].iter().all(u8::is_ascii_digit)
        && b[8..].iter().all(u8::is_ascii_digit)
}

/// "约4:18", "4:18", "约 1:02:33", "4分18秒"
fn is_duration(field: &str) -> bool {
    let t = field.trim_start_matches(['约', '~']).trim();
    let has_colon = t.contains(':')
        && t.split(':')
            .all(|p| !p.is_empty() && p.chars().all(|c| c.is_ascii_digit()));
    has_colon || t.ends_with('秒') || t.ends_with('分')
}

/// `https://x.com/<user>/status/<id>(/video/1)?` -> `<id>`
pub fn status_id(target: &str) -> Option<String> {
    let t = target.trim();
    if !t.is_empty() && t.chars().all(|c| c.is_ascii_digit()) {
        return Some(t.to_string());
    }
    // Accept both `/status/` and the legacy `/statuses/` form.
    let start = ["/status/", "/statuses/"]
        .iter()
        .filter_map(|m| t.find(m).map(|i| i + m.len()))
        .min()?;
    let digits: String = t[start..]
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect();
    if digits.is_empty() {
        None
    } else {
        Some(digits)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_piped_list_line() {
        let e = parse("1. 芭提雅泳池派对 | 2026-08-30 | 约4:18 | https://x.com/ShenqiaoQuella/status/2093971248846143696");
        let e = e.into_iter().next().expect("entry");
        assert_eq!(e.index, Some(1));
        assert_eq!(e.title.as_deref(), Some("芭提雅泳池派对"));
        assert_eq!(e.date.as_deref(), Some("2026-08-30"));
        assert_eq!(
            e.url,
            "https://x.com/ShenqiaoQuella/status/2093971248846143696"
        );
    }

    #[test]
    fn parses_bare_url_line() {
        let e = parse("https://x.com/u/status/42")
            .into_iter()
            .next()
            .expect("entry");
        assert_eq!(e.index, None);
        assert_eq!(e.title, None);
        assert_eq!(e.date, None);
        assert_eq!(e.url, "https://x.com/u/status/42");
    }

    #[test]
    fn skips_blank_and_comments() {
        let v = parse("\n# note\n\nhttps://x.com/u/status/1\n");
        assert_eq!(v.len(), 1);
    }

    #[test]
    fn trailing_punctuation_trimmed() {
        let e = parse("7. 标题 | https://x.com/u/status/7 。")
            .into_iter()
            .next()
            .expect("entry");
        assert_eq!(e.url, "https://x.com/u/status/7");
        assert_eq!(e.title.as_deref(), Some("标题"));
    }

    #[test]
    fn duration_and_date_are_not_titles() {
        let e = parse("2. 按摩 | 约3:22 | 2026-08-30 | https://x.com/u/status/2")
            .into_iter()
            .next()
            .expect("entry");
        assert_eq!(e.title.as_deref(), Some("按摩"));
    }

    #[test]
    fn extracts_status_id_from_variants() {
        assert_eq!(
            status_id("https://x.com/u/status/123/video/1").as_deref(),
            Some("123")
        );
        assert_eq!(
            status_id("https://twitter.com/u/statuses/456").as_deref(),
            Some("456")
        );
        assert_eq!(status_id("789").as_deref(), Some("789"));
        assert_eq!(status_id("https://x.com/u").as_deref(), None);
    }

    #[test]
    fn split_index_variants() {
        assert_eq!(split_index("3. foo"), Some((3, "foo")));
        assert_eq!(split_index("4) foo"), Some((4, "foo")));
        assert_eq!(split_index("5、foo"), Some((5, "foo")));
        assert_eq!(split_index("foo"), None);
    }
}

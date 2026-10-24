# videodl

Rust applet for pulling videos out of X/Twitter posts and pushing them onto an
xstream (ostore) drive. Built because the existing pipeline is Python
(`yt-dlp` + Cobalt tunnels) and every download task was re-deriving the same
steps by hand.

```text
manifest (N. title | date | url)
        |
        v
resolve via api.fxtwitter.com  -->  best MP4 rendition per video
        |                                    |
        |                                    v
        |                       resumable byte-range download (.part)
        |                                    |
        v                                    v
  ostore PUT /api/files/<dir>/<name>  <--  ftyp/size sanity gate
```

## Why these design choices

| Decision | Reason |
| --- | --- |
| Compare the HLS master ladder against the progressive MP4 ladder, take the taller one | X advertises the same encode twice; the MP4 list can stop at 720p while an HLS rendition exists above it. `resolve` prints both so "is this the best copy?" is answerable without downloading. |
| Report the artifact's own measured height/bitrate (ffprobe), not the API label | fxtwitter's `bitrate` field is a nominal ladder name: post 2093971248846143696 is labelled `2176000` but is encoded at ~1.03 Mbps. Measured numbers are what the viewer sees. |
| Metadata from `api.fxtwitter.com`, fallback `api.vxtwitter.com` | `cdn.syndication.twimg.com` now returns `TweetTombstone` for unauthenticated lookups; bytes still come from `video.twimg.com`. |
| Progressive MP4 only, HLS via ffmpeg as fallback | X's `vid/avc1/.../x.mp4` renditions already carry AAC audio, so no remux is needed in the common path. |
| Explicit `--proxy` (else env) | Downloads from this network need the local xray proxy; leaving it implicit made results host-dependent. |
| Resume through `<file>.part` + `Range` | Failures are normal on these long transfers; re-fetching 70 MiB from zero is the difference between "works" and "works eventually". |
| Reject files without an `ftyp` box or under 64 KiB | Prevents HTML error pages and truncated downloads from silently landing on the drive. |
| Upload with `PUT /api/files/<path>` (not multipart) | ostore streams the body to an exact path and auto-creates parents; no temp file juggling, and the response carries the server-side size + sha256 used as an upload assertion. |
| Passcode checked locally | The xstream login page derives the passcode from the date (`rn` + YY + MM + DD + day%2) and only sets the `xstream_auth=ok` cookie, so there is nothing to send to the server. |

## Build

```bash
cd tools/media/videodl
# Build output goes under .tmp/ per repo policy, so the applet directory stays
# source-only.
CARGO_TARGET_DIR=../../../.tmp/videodl/target cargo build --release
CARGO_TARGET_DIR=../../../.tmp/videodl/target cargo test   # 34 unit tests, no network
```

Binary: `.tmp/videodl/target/release/videodl`.

## Usage

```bash
# 1. Inspect what would be downloaded: mp4 max, HLS ladder, chosen rendition
videodl --proxy http://127.0.0.1:2081 resolve https://x.com/user/status/123

# 2. Download to a local staging directory
videodl --proxy http://127.0.0.1:2081 download -f list.md -o .tmp/videos

# 3. Download + upload in one pass
videodl --proxy http://127.0.0.1:2081 sync -f list.md \
    -o .tmp/videos --remote-dir 261024-shenqiao

# 4. Upload files that already exist locally
videodl upload .tmp/videos/*.mp4 --remote-dir 261024-shenqiao

# 5. Drive housekeeping
videodl ls --path 261024-shenqiao
videodl rm 261024-shenqiao/wrong-name.mp4
```

Global flags: `--proxy`, `--base` (default `https://cc.guohuasun.com`),
`--cookie` (default `xstream_auth=ok`), `--passcode`, `--connect-timeout`.
Targets may be full URLs or bare status ids; `-f/--manifest` can be repeated and
its entries are processed before positional targets, in file order.

## Manifest format

One entry per line; anything before `|` may hold `N.` + title, a `YYYY-MM-DD`
field is picked up as the date, the first `http(s)` token becomes the URL.
Blank lines, `#` comments and bare URLs are all accepted.

```text
1. 芭提雅泳池派对 | 2026-08-30 | 约4:18 | https://x.com/user/status/2093971248846143696
https://x.com/user/status/2093748973723156709
```

Output naming: `01-title-20260830.mp4` when manifest metadata exists, otherwise
`20260830-<author>-<status id>.mp4`. Second and later videos in one post get a
`-2`, `-3`, ... suffix.

## Safety / semantics

- Local existence is re-verified (`ftyp` + size), so a corrupt file triggers a
  re-download instead of a silent skip; `--force` overrides both caches.
- Remote skips require a byte-size match against the local file.
- Uploads assert the server-reported size equals the local size and print the
  ostore sha256, so a partial upload cannot be mistaken for success.
- `--max-height` (default 2160) caps rendition choice; the tool never silently
  falls back to a taller rendition.
- A post whose upload was already soft stays soft: X serves only its own
  transcode ladder, so 720p @ ~0.9 Mbps is the ceiling for such posts and no
  client-side flag can change it. Check `resolve` output before assuming a bad
  download.

## Files

```text
src/main.rs       CLI, naming, orchestration, summary tables
src/manifest.rs   loose list parsing + status-id extraction
src/twitter.rs    metadata mirror parsing, rendition selection
src/download.rs   resumable fetch, HLS remux via ffmpeg, MP4 verification + ffprobe
src/xstream.rs    cookie -> ostore token handshake, list/stat/put/delete
src/progress.rs   shared byte meter for downloads and uploads
```

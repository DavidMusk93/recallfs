//! disk-gc: reclaim disk space from stale build caches, stale git worktrees and
//! stale tmp entries.
//!
//! Contract (fail closed):
//! - `plan` never mutates anything; `apply` recomputes the plan and executes it.
//! - "last used" is the newest `max(mtime, ctime)` of every entry in the tree
//!   (plus the worktree admin dir). atime is ignored on purpose: indexers and
//!   scanners bump it without real use.
//! - A candidate is kept when it is fresh, protected, used by a live process
//!   (cwd/exe/root/open fd), contains a mount point, hosts a repo whose linked
//!   worktrees live outside it, had scan errors, or (worktrees) git cannot
//!   describe it.
//! - Before a linked worktree is removed, its HEAD, branch, `git status` and
//!   `git diff --binary HEAD` are saved under the run dir; a detached HEAD is
//!   pinned with `refs/disk-gc/<run>/<name>` so its commits stay reachable.
//!   Untracked files are listed in status.txt but not copied.

use std::collections::{BTreeSet, HashSet};
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::os::unix::fs::MetadataExt;
use std::path::{Component, Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::thread;
use std::time::{SystemTime, UNIX_EPOCH};

pub const DAY: i64 = 86_400;
const CACHEDIR_SIGNATURE: &str = "Signature: 8a477f597d28d172789f06886806bc55";

pub const USAGE: &str = "\
usage: disk-gc <plan|apply> [options]

  plan                  dry run: print and record what would be deleted
  apply                 recompute the plan, rescue git state, then delete

candidates:
  --root DIR            discover linked git worktrees and build dirs under DIR
                        (build dir = CACHEDIR.TAG, out-of-source CMakeCache.txt,
                        blade build64_* next to BLADE_ROOT, .ccache)
  --max-depth N         discovery depth below each --root (default 6)
  --cache DIR           DIR as a single build-cache candidate
  --cache-children DIR  every child of DIR as a build-cache candidate
  --tmp DIR             every child of DIR as a stale-tmp candidate
                        (dot entries and systemd-private-* are skipped)

policy:
  --worktree-days N     delete worktrees unused for N days (default 30)
  --cache-days N        delete build caches unused for N days (default 7)
  --tmp-days N          delete tmp entries unused for N days (default 30)
  --min-size SIZE       ignore cache/tmp candidates below SIZE (default 256M)
  --orphans             also delete orphan worktrees (git admin dir missing or
                        owned by another path); they cannot be rescued by git
  --protect PATH        never delete PATH, its ancestors or descendants

run:
  --state-dir DIR       plan, action log and rescue output (default ./.tmp/disk-gc)
  --jobs N              parallel scan threads (default 8)
  --verbose             also print kept candidates
";

// ---------------------------------------------------------------- config

#[derive(Clone, Debug)]
pub struct Config {
    pub apply: bool,
    pub roots: Vec<PathBuf>,
    pub max_depth: usize,
    pub caches: Vec<PathBuf>,
    pub cache_children: Vec<PathBuf>,
    pub tmps: Vec<PathBuf>,
    pub protects: Vec<PathBuf>,
    pub worktree_days: i64,
    pub cache_days: i64,
    pub tmp_days: i64,
    pub min_bytes: u64,
    pub orphans: bool,
    pub state_dir: PathBuf,
    pub jobs: usize,
    pub verbose: bool,
}

fn take<'a>(it: &mut std::slice::Iter<'a, String>, flag: &str) -> Result<&'a str, String> {
    it.next()
        .map(|s| s.as_str())
        .ok_or_else(|| format!("{flag} needs a value"))
}

fn num<T: std::str::FromStr>(s: &str, flag: &str) -> Result<T, String> {
    s.parse::<T>()
        .map_err(|_| format!("{flag}: invalid number {s:?}"))
}

pub fn parse_args(args: &[String]) -> Result<Config, String> {
    let mut it = args.iter();
    let apply = match it.next().map(|s| s.as_str()) {
        Some("plan") => false,
        Some("apply") => true,
        Some("-h") | Some("--help") | Some("help") | None => return Err(String::new()),
        Some(m) => return Err(format!("unknown mode {m:?}")),
    };
    let mut cfg = Config {
        apply,
        roots: vec![],
        max_depth: 6,
        caches: vec![],
        cache_children: vec![],
        tmps: vec![],
        protects: vec![],
        worktree_days: 30,
        cache_days: 7,
        tmp_days: 30,
        min_bytes: 256 << 20,
        orphans: false,
        state_dir: PathBuf::from(".tmp/disk-gc"),
        jobs: 8,
        verbose: false,
    };
    while let Some(flag) = it.next() {
        let flag = flag.as_str();
        match flag {
            "--root" => cfg.roots.push(take(&mut it, flag)?.into()),
            "--max-depth" => cfg.max_depth = num(take(&mut it, flag)?, flag)?,
            "--cache" => cfg.caches.push(take(&mut it, flag)?.into()),
            "--cache-children" => cfg.cache_children.push(take(&mut it, flag)?.into()),
            "--tmp" => cfg.tmps.push(take(&mut it, flag)?.into()),
            "--protect" => cfg.protects.push(take(&mut it, flag)?.into()),
            "--worktree-days" => cfg.worktree_days = num(take(&mut it, flag)?, flag)?,
            "--cache-days" => cfg.cache_days = num(take(&mut it, flag)?, flag)?,
            "--tmp-days" => cfg.tmp_days = num(take(&mut it, flag)?, flag)?,
            "--min-size" => cfg.min_bytes = parse_size(take(&mut it, flag)?)?,
            "--orphans" => cfg.orphans = true,
            "--state-dir" => cfg.state_dir = take(&mut it, flag)?.into(),
            "--jobs" => cfg.jobs = num::<usize>(take(&mut it, flag)?, flag)?.max(1),
            "--verbose" => cfg.verbose = true,
            "-h" | "--help" => return Err(String::new()),
            other => return Err(format!("unknown option {other:?}")),
        }
    }
    if cfg.roots.is_empty()
        && cfg.caches.is_empty()
        && cfg.cache_children.is_empty()
        && cfg.tmps.is_empty()
    {
        return Err("nothing to scan: pass --root, --cache, --cache-children or --tmp".into());
    }
    if cfg.worktree_days < 0 || cfg.cache_days < 0 || cfg.tmp_days < 0 {
        return Err("day thresholds must be >= 0".into());
    }
    Ok(cfg)
}

pub fn parse_size(s: &str) -> Result<u64, String> {
    let s = s.trim();
    let (digits, mul) = match s.chars().last() {
        Some('K') | Some('k') => (&s[..s.len() - 1], 1u64 << 10),
        Some('M') | Some('m') => (&s[..s.len() - 1], 1u64 << 20),
        Some('G') | Some('g') => (&s[..s.len() - 1], 1u64 << 30),
        Some('T') | Some('t') => (&s[..s.len() - 1], 1u64 << 40),
        _ => (s, 1),
    };
    digits
        .parse::<u64>()
        .ok()
        .and_then(|n| n.checked_mul(mul))
        .ok_or_else(|| format!("invalid size {s:?}"))
}

pub fn human(b: u64) -> String {
    const UNITS: [&str; 5] = ["B", "K", "M", "G", "T"];
    let mut v = b as f64;
    let mut i = 0;
    while v >= 1024.0 && i < UNITS.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    if i == 0 {
        format!("{b}B")
    } else {
        format!("{v:.1}{}", UNITS[i])
    }
}

// ---------------------------------------------------------------- time

pub fn now_secs() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// `YYYY-MM-DDTHH:MM:SSZ`
pub fn fmt_utc(secs: i64) -> String {
    let (y, m, d) = civil_from_days(secs.div_euclid(DAY));
    let r = secs.rem_euclid(DAY);
    format!(
        "{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z",
        r / 3600,
        r % 3600 / 60,
        r % 60
    )
}

/// `YYYYMMDDTHHMMSSZ`, safe for file and ref names.
pub fn fmt_compact(secs: i64) -> String {
    fmt_utc(secs).replace(['-', ':'], "")
}

// ---------------------------------------------------------------- paths

/// Lexical normalization (no symlink resolution).
pub fn normalize(p: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            Component::ParentDir => {
                out.pop();
            }
            Component::CurDir => {}
            other => out.push(other.as_os_str()),
        }
    }
    out
}

pub fn overlaps(a: &Path, b: &Path) -> bool {
    a.starts_with(b) || b.starts_with(a)
}

fn depth(p: &Path) -> usize {
    p.components()
        .filter(|c| matches!(c, Component::Normal(_)))
        .count()
}

fn sanitize(s: &str) -> String {
    let mut out: String = s
        .trim_matches('/')
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .collect();
    out.truncate(180);
    if out.is_empty() {
        out.push('_');
    }
    out
}

// ---------------------------------------------------------------- git links

#[derive(Debug, PartialEq, Eq)]
pub enum GitLink {
    /// `.git` file pointing at `<common>/worktrees/<name>`
    Worktree { admin: PathBuf },
    /// submodule or anything else that is not a linked worktree
    Other,
}

pub fn parse_git_file(worktree: &Path, content: &str) -> Option<GitLink> {
    let raw = content
        .lines()
        .next()?
        .trim()
        .strip_prefix("gitdir:")?
        .trim();
    if raw.is_empty() {
        return None;
    }
    let p = Path::new(raw);
    let admin = if p.is_absolute() {
        normalize(p)
    } else {
        normalize(&worktree.join(p))
    };
    let under_worktrees = admin
        .parent()
        .and_then(|d| d.file_name())
        .map(|n| n == "worktrees")
        .unwrap_or(false);
    Some(if under_worktrees {
        GitLink::Worktree { admin }
    } else {
        GitLink::Other
    })
}

fn read_link_file(dir: &Path) -> Option<GitLink> {
    let git = dir.join(".git");
    let md = fs::symlink_metadata(&git).ok()?;
    if !md.is_file() {
        return None;
    }
    parse_git_file(dir, &fs::read_to_string(&git).ok()?)
}

pub fn common_dir(admin: &Path) -> PathBuf {
    match fs::read_to_string(admin.join("commondir")) {
        Ok(s) => {
            let p = PathBuf::from(s.trim());
            if p.is_absolute() {
                p
            } else {
                normalize(&admin.join(p))
            }
        }
        Err(_) => normalize(&admin.join("../..")),
    }
}

// ---------------------------------------------------------------- candidates

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Kind {
    Worktree,
    OrphanWorktree,
    BuildCache,
    StaleTmp,
}

impl Kind {
    pub fn as_str(self) -> &'static str {
        match self {
            Kind::Worktree => "worktree",
            Kind::OrphanWorktree => "orphan-worktree",
            Kind::BuildCache => "build-cache",
            Kind::StaleTmp => "stale-tmp",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Candidate {
    pub kind: Kind,
    pub path: PathBuf,
    /// git common dir on which `git worktree prune` runs after deletion
    pub prune_common: Option<PathBuf>,
    /// worktree admin dir; its HEAD/index/logs count as use
    pub admin: Option<PathBuf>,
    pub rule: String,
}

fn classify_worktree(dir: &Path, admin: PathBuf) -> Candidate {
    let mk = |kind, prune_common, admin, rule: String| Candidate {
        kind,
        path: dir.to_path_buf(),
        prune_common,
        admin,
        rule,
    };
    if !admin.is_dir() {
        return mk(
            Kind::OrphanWorktree,
            None,
            None,
            format!("admin dir missing: {}", admin.display()),
        );
    }
    let common = common_dir(&admin);
    let registered = match fs::read_to_string(admin.join("gitdir")) {
        Ok(s) => PathBuf::from(s.trim()),
        Err(_) => {
            return mk(
                Kind::OrphanWorktree,
                None,
                Some(admin),
                "admin gitdir unreadable".into(),
            )
        }
    };
    let registered = if registered.is_absolute() {
        registered
    } else {
        normalize(&admin.join(registered))
    };
    match (
        fs::canonicalize(&registered),
        fs::canonicalize(dir.join(".git")),
    ) {
        (Ok(a), Ok(b)) if a == b => mk(
            Kind::Worktree,
            Some(common),
            Some(admin),
            "linked worktree".into(),
        ),
        (Err(_), _) => mk(
            Kind::OrphanWorktree,
            Some(common),
            Some(admin),
            format!("moved worktree, admin registered {}", registered.display()),
        ),
        _ => mk(
            Kind::OrphanWorktree,
            None,
            None,
            format!("admin belongs to {}", registered.display()),
        ),
    }
}

pub fn build_dir_rule(dir: &Path) -> Option<&'static str> {
    let name = dir.file_name()?.to_str()?;
    let has = |n: &str| fs::symlink_metadata(dir.join(n)).is_ok();
    if let Ok(tag) = fs::read(dir.join("CACHEDIR.TAG")) {
        if tag.starts_with(CACHEDIR_SIGNATURE.as_bytes()) {
            return Some("CACHEDIR.TAG");
        }
    }
    if has("CMakeCache.txt") && !has("CMakeLists.txt") {
        return Some("out-of-source CMake build dir");
    }
    if name.starts_with("build64_")
        && dir
            .parent()
            .map(|p| p.join("BLADE_ROOT").exists())
            .unwrap_or(false)
    {
        return Some("blade output dir");
    }
    if name == ".ccache" {
        return Some("ccache dir");
    }
    None
}

pub fn discover(root: &Path, max_depth: usize, out: &mut Vec<Candidate>, warns: &mut Vec<String>) {
    discover_dir(root, 0, max_depth, out, warns);
}

fn discover_dir(
    dir: &Path,
    depth: usize,
    max_depth: usize,
    out: &mut Vec<Candidate>,
    warns: &mut Vec<String>,
) {
    if let Some(GitLink::Worktree { admin }) = read_link_file(dir) {
        out.push(classify_worktree(dir, admin));
        return;
    }
    if depth > 0 {
        if let Some(rule) = build_dir_rule(dir) {
            out.push(Candidate {
                kind: Kind::BuildCache,
                path: dir.to_path_buf(),
                prune_common: None,
                admin: None,
                rule: rule.into(),
            });
            return;
        }
    }
    if depth >= max_depth {
        return;
    }
    let rd = match fs::read_dir(dir) {
        Ok(r) => r,
        Err(e) => {
            warns.push(format!("discover {}: {e}", dir.display()));
            return;
        }
    };
    let mut subdirs: Vec<PathBuf> = rd
        .flatten()
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .filter(|e| e.file_name() != ".git" && e.file_name() != "node_modules")
        .map(|e| e.path())
        .collect();
    subdirs.sort();
    for sub in subdirs {
        discover_dir(&sub, depth + 1, max_depth, out, warns);
    }
}

// ---------------------------------------------------------------- tree scan

#[derive(Clone, Debug, Default)]
pub struct TreeStats {
    pub bytes: u64,
    pub newest: i64,
    pub newest_path: PathBuf,
    pub entries: u64,
    pub errors: u64,
    pub foreign_mount: Option<PathBuf>,
    /// linked worktrees nested inside: (worktree dir, git common dir)
    pub nested_worktrees: Vec<(PathBuf, PathBuf)>,
    /// a repo inside whose linked worktrees live outside the tree
    pub external_worktree_host: Option<PathBuf>,
}

fn account(st: &mut TreeStats, p: &Path, md: &fs::Metadata, seen: &mut HashSet<(u64, u64)>) {
    st.entries += 1;
    let t = md.mtime().max(md.ctime());
    if t > st.newest {
        st.newest = t;
        st.newest_path = p.to_path_buf();
    }
    if !md.is_dir() && md.nlink() > 1 && !seen.insert((md.dev(), md.ino())) {
        return;
    }
    st.bytes += md.blocks() * 512;
}

fn inspect_git_dir(gitdir: &Path, root: &Path, st: &mut TreeStats) {
    let Ok(rd) = fs::read_dir(gitdir.join("worktrees")) else {
        return;
    };
    for e in rd.flatten() {
        let Ok(s) = fs::read_to_string(e.path().join("gitdir")) else {
            continue;
        };
        let dotgit = PathBuf::from(s.trim());
        let Some(wt) = dotgit.parent() else { continue };
        if wt.exists() && !normalize(wt).starts_with(root) && st.external_worktree_host.is_none() {
            st.external_worktree_host = Some(gitdir.to_path_buf());
        }
    }
}

pub fn tree_stats(root: &Path, extra: &[PathBuf]) -> TreeStats {
    let mut st = TreeStats::default();
    let mut seen = HashSet::new();
    for p in extra {
        if let Ok(md) = fs::symlink_metadata(p) {
            let t = md.mtime().max(md.ctime());
            if t > st.newest {
                st.newest = t;
                st.newest_path = p.clone();
            }
        }
    }
    let root_md = match fs::symlink_metadata(root) {
        Ok(m) => m,
        Err(_) => {
            st.errors += 1;
            return st;
        }
    };
    account(&mut st, root, &root_md, &mut seen);
    if !root_md.is_dir() {
        return st;
    }
    let dev = root_md.dev();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let rd = match fs::read_dir(&dir) {
            Ok(r) => r,
            Err(_) => {
                st.errors += 1;
                continue;
            }
        };
        for ent in rd {
            let Ok(ent) = ent else {
                st.errors += 1;
                continue;
            };
            let p = ent.path();
            let md = match fs::symlink_metadata(&p) {
                Ok(m) => m,
                Err(_) => {
                    st.errors += 1;
                    continue;
                }
            };
            if md.is_dir() && md.dev() != dev {
                st.foreign_mount.get_or_insert(p);
                continue;
            }
            account(&mut st, &p, &md, &mut seen);
            let is_dotgit = ent.file_name() == ".git";
            if md.is_dir() {
                if is_dotgit {
                    inspect_git_dir(&p, root, &mut st);
                }
                stack.push(p);
            } else if is_dotgit && md.is_file() && dir != root {
                if let Some(GitLink::Worktree { admin }) = read_link_file(&dir) {
                    if admin.is_dir() {
                        st.nested_worktrees.push((dir.clone(), common_dir(&admin)));
                    }
                }
            }
        }
    }
    st
}

// ---------------------------------------------------------------- live use

/// Paths held by live processes: cwd, exe, non-/ root, open fds.
pub fn proc_paths() -> Vec<PathBuf> {
    let mut v = vec![];
    let Ok(rd) = fs::read_dir("/proc") else {
        return v;
    };
    let me = std::process::id().to_string();
    for e in rd.flatten() {
        let name = e.file_name();
        let name = name.to_string_lossy();
        if !name.bytes().all(|b| b.is_ascii_digit()) || name == me {
            continue;
        }
        let base = e.path();
        for l in ["cwd", "exe"] {
            if let Ok(t) = fs::read_link(base.join(l)) {
                v.push(t);
            }
        }
        if let Ok(t) = fs::read_link(base.join("root")) {
            if t != Path::new("/") {
                v.push(t);
            }
        }
        if let Ok(fds) = fs::read_dir(base.join("fd")) {
            for fd in fds.flatten() {
                if let Ok(t) = fs::read_link(fd.path()) {
                    if t.is_absolute() {
                        v.push(t);
                    }
                }
            }
        }
    }
    v.sort();
    v.dedup();
    v
}

// ---------------------------------------------------------------- git state

#[derive(Clone, Debug)]
pub struct GitInfo {
    pub head: String,
    pub branch: Option<String>,
    pub status: String,
    pub dirty_tracked: bool,
    pub untracked: usize,
}

impl GitInfo {
    fn state(&self) -> &'static str {
        match (self.dirty_tracked, self.untracked > 0) {
            (false, false) => "clean",
            (false, true) => "untrk",
            _ => "dirty",
        }
    }
}

fn git_cmd() -> Command {
    let mut c = Command::new("git");
    // never take index.lock or refresh the index: scanning must not look like use
    c.env("GIT_OPTIONAL_LOCKS", "0");
    c
}

fn git(dir: &Path, args: &[&str]) -> Result<String, String> {
    let out = git_cmd()
        .arg("-C")
        .arg(dir)
        .args(args)
        .output()
        .map_err(|e| format!("spawn git: {e}"))?;
    if out.status.success() {
        Ok(String::from_utf8_lossy(&out.stdout).into_owned())
    } else {
        Err(format!(
            "git {}: {}",
            args.join(" "),
            String::from_utf8_lossy(&out.stderr).trim()
        ))
    }
}

pub fn git_info(wt: &Path) -> Result<GitInfo, String> {
    let head = git(wt, &["rev-parse", "--verify", "HEAD"])?
        .trim()
        .to_string();
    let branch = git(wt, &["symbolic-ref", "-q", "--short", "HEAD"])
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty());
    let status = git(
        wt,
        &["status", "--porcelain=v1", "--untracked-files=normal"],
    )?;
    let mut dirty_tracked = false;
    let mut untracked = 0;
    for l in status.lines() {
        if l.starts_with("??") {
            untracked += 1;
        } else if !l.is_empty() {
            dirty_tracked = true;
        }
    }
    Ok(GitInfo {
        head,
        branch,
        status,
        dirty_tracked,
        untracked,
    })
}

fn rescue(wt: &Path, info: &GitInfo, dir: &Path, run_id: &str) -> Result<(), String> {
    fs::create_dir_all(dir).map_err(|e| format!("mkdir {}: {e}", dir.display()))?;
    let mut meta = format!(
        "path\t{}\nhead\t{}\nbranch\t{}\n",
        wt.display(),
        info.head,
        info.branch.as_deref().unwrap_or("(detached)")
    );
    if info.branch.is_none() {
        let name = sanitize(
            &wt.file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default(),
        );
        let r = format!("refs/disk-gc/{run_id}/{name}");
        git(wt, &["update-ref", &r, &info.head])?;
        meta.push_str(&format!("keep_ref\t{r}\n"));
    }
    fs::write(dir.join("meta.tsv"), meta).map_err(|e| format!("write meta: {e}"))?;
    fs::write(dir.join("status.txt"), &info.status).map_err(|e| format!("write status: {e}"))?;
    if info.dirty_tracked {
        let f =
            File::create(dir.join("tracked.patch")).map_err(|e| format!("create patch: {e}"))?;
        let st = git_cmd()
            .arg("-C")
            .arg(wt)
            .args(["diff", "--binary", "HEAD"])
            .stdout(f)
            .status()
            .map_err(|e| format!("spawn git diff: {e}"))?;
        if !st.success() {
            return Err(format!("git diff failed in {}", wt.display()));
        }
    }
    Ok(())
}

// ---------------------------------------------------------------- plan

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Decision {
    Delete,
    Keep(String),
    Covered(PathBuf),
}

#[derive(Clone, Debug)]
pub struct Item {
    pub cand: Candidate,
    pub stats: TreeStats,
    pub git: Option<GitInfo>,
    /// (worktree, common dir, info) for linked worktrees nested inside
    pub nested_git: Vec<(PathBuf, PathBuf, GitInfo)>,
    pub decision: Decision,
}

impl Item {
    fn age_days(&self, now: i64) -> i64 {
        (now - self.stats.newest).max(0) / DAY
    }
    fn git_state(&self) -> &'static str {
        match (&self.cand.kind, &self.git) {
            (_, Some(g)) => g.state(),
            (Kind::Worktree, None) | (Kind::OrphanWorktree, None) => "?",
            _ => "-",
        }
    }
}

fn decide(item: &Item, cfg: &Config, now: i64, used: &[PathBuf], guard: &[PathBuf]) -> Decision {
    let p = &item.cand.path;
    if depth(p) < 2 {
        return Decision::Keep("path too shallow".into());
    }
    if let Some(g) = guard.iter().find(|g| overlaps(p, g)) {
        return Decision::Keep(format!("protected: {}", g.display()));
    }
    let st = &item.stats;
    if st.errors > 0 {
        return Decision::Keep(format!("{} scan errors", st.errors));
    }
    if let Some(m) = &st.foreign_mount {
        return Decision::Keep(format!("contains mount point {}", m.display()));
    }
    if let Some(h) = &st.external_worktree_host {
        return Decision::Keep(format!(
            "hosts repo with outside worktrees: {}",
            h.display()
        ));
    }
    let (limit, allowed) = match item.cand.kind {
        Kind::Worktree => (cfg.worktree_days, true),
        Kind::OrphanWorktree => (cfg.worktree_days, cfg.orphans),
        Kind::BuildCache => (cfg.cache_days, true),
        Kind::StaleTmp => (cfg.tmp_days, true),
    };
    let age = item.age_days(now);
    if age < limit {
        return Decision::Keep(format!(
            "fresh: {age}d < {limit}d ({})",
            st.newest_path.display()
        ));
    }
    if !allowed {
        return Decision::Keep("orphan worktree, pass --orphans".into());
    }
    if let Some(u) = used.iter().find(|u| u.starts_with(p)) {
        return Decision::Keep(format!("in use by a process: {}", u.display()));
    }
    Decision::Delete
}

fn collect_candidates(cfg: &Config, warns: &mut Vec<String>) -> Vec<Candidate> {
    let canon = |p: &Path, warns: &mut Vec<String>| match fs::canonicalize(p) {
        Ok(c) => Some(c),
        Err(e) => {
            warns.push(format!("skip {}: {e}", p.display()));
            None
        }
    };
    let mut out = vec![];
    for r in &cfg.roots {
        if let Some(r) = canon(r, warns) {
            discover(&r, cfg.max_depth, &mut out, warns);
        }
    }
    let explicit = |kind: Kind, path: PathBuf, rule: String| Candidate {
        kind,
        path,
        prune_common: None,
        admin: None,
        rule,
    };
    for c in &cfg.caches {
        if let Some(c) = canon(c, warns) {
            out.push(explicit(Kind::BuildCache, c, "--cache".into()));
        }
    }
    let children = |dir: &Path, warns: &mut Vec<String>| -> Vec<PathBuf> {
        match fs::read_dir(dir) {
            Ok(rd) => {
                let mut v: Vec<PathBuf> = rd
                    .flatten()
                    .filter(|e| {
                        e.file_type()
                            .map(|t| t.is_dir() || t.is_file())
                            .unwrap_or(false)
                    })
                    .map(|e| e.path())
                    .collect();
                v.sort();
                v
            }
            Err(e) => {
                warns.push(format!("list {}: {e}", dir.display()));
                vec![]
            }
        }
    };
    for c in &cfg.cache_children {
        if let Some(c) = canon(c, warns) {
            for p in children(&c, warns) {
                out.push(explicit(
                    Kind::BuildCache,
                    p,
                    format!("child of --cache-children {}", c.display()),
                ));
            }
        }
    }
    for t in &cfg.tmps {
        if let Some(t) = canon(t, warns) {
            for p in children(&t, warns) {
                let name = p
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default();
                if name.starts_with('.') || name.starts_with("systemd-private-") {
                    continue;
                }
                out.push(explicit(
                    Kind::StaleTmp,
                    p,
                    format!("child of --tmp {}", t.display()),
                ));
            }
        }
    }
    let mut seen = BTreeSet::new();
    out.retain(|c| seen.insert(c.path.clone()));
    out
}

fn scan_parallel(cands: &[Candidate], jobs: usize) -> Vec<TreeStats> {
    let next = AtomicUsize::new(0);
    let results: Mutex<Vec<Option<TreeStats>>> = Mutex::new(vec![None; cands.len()]);
    thread::scope(|s| {
        for _ in 0..jobs.min(cands.len().max(1)) {
            s.spawn(|| loop {
                let i = next.fetch_add(1, Ordering::SeqCst);
                if i >= cands.len() {
                    break;
                }
                let c = &cands[i];
                let extra: Vec<PathBuf> = c
                    .admin
                    .iter()
                    .flat_map(|a| {
                        ["HEAD", "index", "logs/HEAD", "ORIG_HEAD", "FETCH_HEAD"]
                            .iter()
                            .map(move |f| a.join(f))
                    })
                    .collect();
                let st = tree_stats(&c.path, &extra);
                results.lock().unwrap()[i] = Some(st);
            });
        }
    });
    results
        .into_inner()
        .unwrap()
        .into_iter()
        .map(|o| o.unwrap_or_default())
        .collect()
}

pub fn build_plan(cfg: &Config, now: i64, guard: &[PathBuf], warns: &mut Vec<String>) -> Vec<Item> {
    let cands = collect_candidates(cfg, warns);
    let stats = scan_parallel(&cands, cfg.jobs);
    let used = proc_paths();
    let mut items: Vec<Item> = cands
        .into_iter()
        .zip(stats)
        .filter(|(c, s)| {
            matches!(c.kind, Kind::Worktree | Kind::OrphanWorktree) || s.bytes >= cfg.min_bytes
        })
        .map(|(cand, stats)| Item {
            cand,
            stats,
            git: None,
            nested_git: vec![],
            decision: Decision::Keep(String::new()),
        })
        .collect();
    for item in items.iter_mut() {
        item.decision = decide(item, cfg, now, &used, guard);
    }
    // git state is required before a worktree may be deleted (fail closed)
    for item in items.iter_mut() {
        if !matches!(item.cand.kind, Kind::Worktree | Kind::OrphanWorktree) {
            continue;
        }
        match git_info(&item.cand.path) {
            Ok(g) => item.git = Some(g),
            Err(e) => {
                if item.cand.kind == Kind::Worktree && item.decision == Decision::Delete {
                    item.decision = Decision::Keep(format!("git unreadable: {e}"));
                }
            }
        }
        if item.decision != Decision::Delete {
            continue;
        }
        for (wt, common) in item.stats.nested_worktrees.clone() {
            match git_info(&wt) {
                Ok(g) => item.nested_git.push((wt, common, g)),
                Err(e) => {
                    item.decision =
                        Decision::Keep(format!("nested worktree {} unreadable: {e}", wt.display()));
                    break;
                }
            }
        }
    }
    // a deleted ancestor covers its descendants
    let deleting: Vec<PathBuf> = items
        .iter()
        .filter(|i| i.decision == Decision::Delete)
        .map(|i| i.cand.path.clone())
        .collect();
    for item in items.iter_mut() {
        if let Some(a) = deleting
            .iter()
            .find(|a| item.cand.path != **a && item.cand.path.starts_with(a))
        {
            item.decision = Decision::Covered(a.clone());
        }
    }
    items.sort_by(|a, b| {
        let rank = |d: &Decision| match d {
            Decision::Delete => 0,
            Decision::Keep(_) => 1,
            Decision::Covered(_) => 2,
        };
        rank(&a.decision)
            .cmp(&rank(&b.decision))
            .then(b.stats.bytes.cmp(&a.stats.bytes))
    });
    items
}

// ---------------------------------------------------------------- execute

fn avail(p: &Path) -> Option<u64> {
    let out = Command::new("df")
        .arg("-B1")
        .arg("--output=avail")
        .arg(p)
        .output()
        .ok()?;
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .nth(1)?
        .trim()
        .parse()
        .ok()
}

fn execute(item: &Item, run_dir: &Path, run_id: &str) -> Result<String, String> {
    let path = &item.cand.path;
    let mut notes = vec![];
    let mut commons: Vec<PathBuf> = item.cand.prune_common.iter().cloned().collect();
    if let Some(g) = &item.git {
        let dir = run_dir
            .join("rescue")
            .join(sanitize(&path.to_string_lossy()));
        rescue(path, g, &dir, run_id)?;
        notes.push(format!("rescue={}", dir.display()));
    }
    for (wt, common, g) in &item.nested_git {
        let dir = run_dir.join("rescue").join(sanitize(&wt.to_string_lossy()));
        rescue(wt, g, &dir, run_id)?;
        commons.push(common.clone());
    }
    let md = fs::symlink_metadata(path).map_err(|e| format!("stat: {e}"))?;
    if md.is_dir() {
        fs::remove_dir_all(path).map_err(|e| format!("remove_dir_all: {e}"))?;
    } else {
        fs::remove_file(path).map_err(|e| format!("remove_file: {e}"))?;
    }
    commons.sort();
    commons.dedup();
    for c in commons {
        let out = git_cmd()
            .arg(format!("--git-dir={}", c.display()))
            .args(["worktree", "prune"])
            .output();
        match out {
            Ok(o) if o.status.success() => notes.push(format!("pruned={}", c.display())),
            _ => notes.push(format!("prune-failed={}", c.display())),
        }
    }
    Ok(notes.join(" "))
}

fn write_plan(path: &Path, items: &[Item], now: i64) -> std::io::Result<()> {
    let mut f = File::create(path)?;
    writeln!(
        f,
        "decision\tkind\tbytes\tage_days\tnewest_utc\tgit\tpath\treason\tnewest_path"
    )?;
    for i in items {
        let (d, reason) = match &i.decision {
            Decision::Delete => ("delete", i.cand.rule.clone()),
            Decision::Keep(r) => ("keep", r.clone()),
            Decision::Covered(a) => ("covered", format!("by {}", a.display())),
        };
        writeln!(
            f,
            "{d}\t{}\t{}\t{}\t{}\t{}\t{}\t{reason}\t{}",
            i.cand.kind.as_str(),
            i.stats.bytes,
            i.age_days(now),
            fmt_utc(i.stats.newest),
            i.git_state(),
            i.cand.path.display(),
            i.stats.newest_path.display()
        )?;
    }
    Ok(())
}

/// Returns the number of failed actions.
pub fn run(cfg: &Config) -> Result<usize, String> {
    let now = now_secs();
    let run_id = fmt_compact(now);
    let run_dir = cfg.state_dir.join(format!("run-{run_id}"));
    fs::create_dir_all(&run_dir).map_err(|e| format!("mkdir {}: {e}", run_dir.display()))?;
    let run_dir = fs::canonicalize(&run_dir).map_err(|e| e.to_string())?;

    let mut guard: Vec<PathBuf> = vec![];
    if let Ok(s) = fs::canonicalize(&cfg.state_dir) {
        guard.push(s);
    }
    if let Ok(c) = std::env::current_dir() {
        guard.push(c);
    }
    if let Ok(e) = std::env::current_exe() {
        guard.push(e);
    }
    for p in &cfg.protects {
        guard.push(fs::canonicalize(p).unwrap_or_else(|_| p.clone()));
    }

    let mut warns = vec![];
    let items = build_plan(cfg, now, &guard, &mut warns);
    for w in &warns {
        eprintln!("warn: {w}");
    }
    let plan_path = run_dir.join("plan.tsv");
    write_plan(&plan_path, &items, now).map_err(|e| format!("write plan: {e}"))?;

    println!(
        "{:<7} {:<15} {:>8} {:>5} {:<5} PATH  # REASON",
        "ACTION", "KIND", "SIZE", "AGE", "GIT"
    );
    let mut totals: Vec<(Kind, usize, u64)> = vec![];
    for i in &items {
        let (act, reason) = match &i.decision {
            Decision::Delete => {
                match totals.iter_mut().find(|t| t.0 == i.cand.kind) {
                    Some(t) => {
                        t.1 += 1;
                        t.2 += i.stats.bytes;
                    }
                    None => totals.push((i.cand.kind, 1, i.stats.bytes)),
                }
                ("DELETE", i.cand.rule.clone())
            }
            Decision::Keep(r) if cfg.verbose => ("keep", r.clone()),
            Decision::Covered(a) if cfg.verbose => ("covered", format!("by {}", a.display())),
            _ => continue,
        };
        println!(
            "{act:<7} {:<15} {:>8} {:>4}d {:<5} {}  # {reason}",
            i.cand.kind.as_str(),
            human(i.stats.bytes),
            i.age_days(now),
            i.git_state(),
            i.cand.path.display()
        );
    }
    let kept = items
        .iter()
        .filter(|i| matches!(i.decision, Decision::Keep(_)))
        .count();
    let total: u64 = totals.iter().map(|t| t.2).sum();
    println!("---");
    for (k, n, b) in &totals {
        println!("{:<15} {n:>4} candidates  {}", k.as_str(), human(*b));
    }
    println!(
        "reclaimable {}  (kept {kept}, plan {})",
        human(total),
        plan_path.display()
    );

    if !cfg.apply {
        println!("dry run: re-run with `apply` to delete");
        return Ok(0);
    }

    // filesystems touched, for before/after evidence
    let mut fs_probe: Vec<(u64, PathBuf)> = vec![];
    for i in items.iter().filter(|i| i.decision == Decision::Delete) {
        if let Some(parent) = i.cand.path.parent() {
            if let Ok(md) = fs::metadata(parent) {
                if !fs_probe.iter().any(|(d, _)| *d == md.dev()) {
                    fs_probe.push((md.dev(), parent.to_path_buf()));
                }
            }
        }
    }
    let before: Vec<Option<u64>> = fs_probe.iter().map(|(_, p)| avail(p)).collect();

    let log_path = run_dir.join("actions.tsv");
    let mut log = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
        .map_err(|e| format!("open log: {e}"))?;
    let _ = writeln!(log, "utc\tresult\tkind\tbytes\tpath\tdetail");
    let mut failed = 0;
    for i in items.iter().filter(|i| i.decision == Decision::Delete) {
        let res = execute(i, &run_dir, &run_id);
        let (tag, detail) = match &res {
            Ok(n) => ("ok", n.clone()),
            Err(e) => {
                failed += 1;
                ("FAILED", e.clone())
            }
        };
        println!(
            "{tag:<6} {:>8} {}  {detail}",
            human(i.stats.bytes),
            i.cand.path.display()
        );
        let _ = writeln!(
            log,
            "{}\t{tag}\t{}\t{}\t{}\t{detail}",
            fmt_utc(now_secs()),
            i.cand.kind.as_str(),
            i.stats.bytes,
            i.cand.path.display()
        );
    }
    for ((_, p), b) in fs_probe.iter().zip(before) {
        let a = avail(p);
        println!(
            "avail {}: {} -> {}",
            p.display(),
            b.map(human).unwrap_or_else(|| "?".into()),
            a.map(human).unwrap_or_else(|| "?".into())
        );
    }
    println!("log {}", log_path.display());
    Ok(failed)
}

// ---------------------------------------------------------------- tests

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn git_file_links() {
        let wt = Path::new("/w/repo-wt");
        assert_eq!(
            parse_git_file(wt, "gitdir: /w/repo/.git/worktrees/repo-wt\n"),
            Some(GitLink::Worktree {
                admin: PathBuf::from("/w/repo/.git/worktrees/repo-wt")
            })
        );
        assert_eq!(
            parse_git_file(wt, "gitdir: ../repo/.git/worktrees/x"),
            Some(GitLink::Worktree {
                admin: PathBuf::from("/w/repo/.git/worktrees/x")
            })
        );
        assert_eq!(
            parse_git_file(wt, "gitdir: ../../.git/modules/third_party/re2"),
            Some(GitLink::Other)
        );
        assert_eq!(parse_git_file(wt, "garbage"), None);
        assert_eq!(parse_git_file(wt, "gitdir: "), None);
    }

    #[test]
    fn lexical_normalize() {
        assert_eq!(
            normalize(Path::new("/a/b/../c/./d")),
            PathBuf::from("/a/c/d")
        );
    }

    #[test]
    fn time_format() {
        assert_eq!(fmt_utc(0), "1970-01-01T00:00:00Z");
        assert_eq!(fmt_utc(1_000_000_000), "2001-09-09T01:46:40Z");
        assert_eq!(fmt_utc(951_782_400), "2000-02-29T00:00:00Z");
        assert_eq!(fmt_compact(0), "19700101T000000Z");
    }

    #[test]
    fn sizes() {
        assert_eq!(parse_size("0").unwrap(), 0);
        assert_eq!(parse_size("256M").unwrap(), 256 << 20);
        assert_eq!(parse_size("1g").unwrap(), 1 << 30);
        assert!(parse_size("x").is_err());
        assert_eq!(human(512), "512B");
        assert_eq!(human(3 << 30), "3.0G");
    }

    #[test]
    fn overlap_rules() {
        assert!(overlaps(Path::new("/a/b"), Path::new("/a")));
        assert!(overlaps(Path::new("/a"), Path::new("/a/b")));
        assert!(!overlaps(Path::new("/a/bc"), Path::new("/a/b")));
        assert_eq!(depth(Path::new("/tmp")), 1);
    }

    #[test]
    fn args() {
        let a: Vec<String> = ["plan", "--root", "/x", "--worktree-days", "30", "--orphans"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let c = parse_args(&a).unwrap();
        assert!(!c.apply && c.orphans && c.worktree_days == 30);
        assert!(parse_args(&["plan".to_string()]).is_err());
        assert!(parse_args(&["nuke".to_string()]).is_err());
    }
}

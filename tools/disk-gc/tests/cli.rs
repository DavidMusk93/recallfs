//! End-to-end checks against real git repositories.
//! Set DISK_GC_TEST_DIR to keep scratch data off a full /tmp.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

fn scratch(name: &str) -> PathBuf {
    let base = std::env::var_os("DISK_GC_TEST_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    let d = base.join(format!("disk-gc-test-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&d);
    fs::create_dir_all(&d).unwrap();
    fs::canonicalize(&d).unwrap()
}

fn sh(dir: &Path, args: &[&str]) {
    let st = Command::new(args[0])
        .args(&args[1..])
        .current_dir(dir)
        .env("GIT_AUTHOR_NAME", "t")
        .env("GIT_AUTHOR_EMAIL", "t@t")
        .env("GIT_COMMITTER_NAME", "t")
        .env("GIT_COMMITTER_EMAIL", "t@t")
        .status()
        .unwrap();
    assert!(st.success(), "{args:?} in {}", dir.display());
}

fn disk_gc(args: &[&str]) -> Output {
    let out = Command::new(env!("CARGO_BIN_EXE_disk-gc"))
        .args(args)
        .output()
        .unwrap();
    assert!(
        out.status.success(),
        "stdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    out
}

fn repo_with_commit(d: &Path) -> PathBuf {
    let repo = d.join("repo");
    fs::create_dir(&repo).unwrap();
    sh(&repo, &["git", "init", "-q"]);
    fs::write(repo.join("a.txt"), "one\n").unwrap();
    sh(&repo, &["git", "add", "a.txt"]);
    sh(&repo, &["git", "commit", "-qm", "init"]);
    repo
}

fn find_file(dir: &Path, name: &str) -> Option<PathBuf> {
    for e in fs::read_dir(dir).ok()?.flatten() {
        let p = e.path();
        if p.is_dir() {
            if let Some(f) = find_file(&p, name) {
                return Some(f);
            }
        } else if e.file_name() == name {
            return Some(p);
        }
    }
    None
}

fn s(p: &Path) -> &str {
    p.to_str().unwrap()
}

#[test]
fn stale_worktrees_are_rescued_then_removed() {
    let d = scratch("wt");
    let repo = repo_with_commit(&d);
    let wt_branch = d.join("wt-branch");
    let wt_detached = d.join("wt-detached");
    sh(
        &repo,
        &[
            "git",
            "worktree",
            "add",
            "-q",
            "-b",
            "feature",
            s(&wt_branch),
        ],
    );
    sh(
        &repo,
        &["git", "worktree", "add", "-q", "--detach", s(&wt_detached)],
    );
    fs::write(wt_detached.join("b.txt"), "two\n").unwrap();
    sh(&wt_detached, &["git", "add", "b.txt"]);
    sh(&wt_detached, &["git", "commit", "-qm", "detached work"]);
    let detached_head = String::from_utf8(
        Command::new("git")
            .args(["-C", s(&wt_detached), "rev-parse", "HEAD"])
            .output()
            .unwrap()
            .stdout,
    )
    .unwrap();
    fs::write(wt_branch.join("a.txt"), "changed\n").unwrap();

    let state = d.join("state");
    let plan = disk_gc(&[
        "plan",
        "--root",
        s(&d),
        "--worktree-days",
        "0",
        "--state-dir",
        s(&state),
    ]);
    let plan = String::from_utf8_lossy(&plan.stdout);
    assert!(
        plan.contains("wt-branch") && plan.contains("dirty"),
        "{plan}"
    );
    assert!(wt_branch.exists(), "plan must not delete");

    disk_gc(&[
        "apply",
        "--root",
        s(&d),
        "--worktree-days",
        "0",
        "--state-dir",
        s(&state),
    ]);
    assert!(!wt_branch.exists() && !wt_detached.exists());
    assert!(repo.join("a.txt").exists(), "main worktree untouched");

    let refs = Command::new("git")
        .args([
            "-C",
            s(&repo),
            "for-each-ref",
            "--format=%(refname) %(objectname)",
        ])
        .output()
        .unwrap();
    let refs = String::from_utf8_lossy(&refs.stdout);
    assert!(refs.contains("refs/heads/feature"), "{refs}");
    assert!(
        refs.lines()
            .any(|l| l.starts_with("refs/disk-gc/") && l.ends_with(detached_head.trim())),
        "detached commit pinned: {refs}"
    );
    let list = Command::new("git")
        .args(["-C", s(&repo), "worktree", "list", "--porcelain"])
        .output()
        .unwrap();
    let list = String::from_utf8_lossy(&list.stdout);
    assert_eq!(
        list.lines().filter(|l| l.starts_with("worktree ")).count(),
        1,
        "{list}"
    );
    let patch = find_file(&state, "tracked.patch").expect("patch saved");
    assert!(fs::read_to_string(patch).unwrap().contains("+changed"));
    let _ = fs::remove_dir_all(&d);
}

#[test]
fn fresh_or_busy_worktrees_are_kept() {
    let d = scratch("busy");
    let repo = repo_with_commit(&d);
    let wt = d.join("wt");
    sh(
        &repo,
        &["git", "worktree", "add", "-q", "-b", "busy", s(&wt)],
    );
    let state = d.join("state");

    let out = disk_gc(&[
        "plan",
        "--root",
        s(&d),
        "--state-dir",
        s(&state),
        "--verbose",
    ]);
    assert!(String::from_utf8_lossy(&out.stdout).contains("fresh:"));

    let mut child = Command::new("sleep")
        .arg("30")
        .current_dir(&wt)
        .spawn()
        .unwrap();
    let out = disk_gc(&[
        "apply",
        "--root",
        s(&d),
        "--worktree-days",
        "0",
        "--state-dir",
        s(&state),
        "--verbose",
    ]);
    let _ = child.kill();
    let _ = child.wait();
    assert!(String::from_utf8_lossy(&out.stdout).contains("in use by a process"));
    assert!(wt.exists());
    let _ = fs::remove_dir_all(&d);
}

#[test]
fn build_dirs_follow_markers() {
    let d = scratch("build");
    let tagged = d.join("proj/target");
    fs::create_dir_all(&tagged).unwrap();
    fs::write(
        tagged.join("CACHEDIR.TAG"),
        "Signature: 8a477f597d28d172789f06886806bc55\n",
    )
    .unwrap();
    fs::write(tagged.join("blob"), vec![1u8; 8192]).unwrap();
    let out_of_source = d.join("proj/cmake-build");
    fs::create_dir_all(&out_of_source).unwrap();
    fs::write(out_of_source.join("CMakeCache.txt"), "x").unwrap();
    let in_source = d.join("insrc");
    fs::create_dir_all(&in_source).unwrap();
    fs::write(in_source.join("CMakeCache.txt"), "x").unwrap();
    fs::write(in_source.join("CMakeLists.txt"), "x").unwrap();
    let state = d.join("state");

    disk_gc(&[
        "apply",
        "--root",
        s(&d),
        "--cache-days",
        "0",
        "--min-size",
        "0",
        "--state-dir",
        s(&state),
    ]);
    assert!(!tagged.exists());
    assert!(!out_of_source.exists());
    assert!(in_source.exists(), "in-source CMake tree is not a cache");
    let _ = fs::remove_dir_all(&d);
}

#[test]
fn stale_tmp_skips_dot_entries_and_protected() {
    let d = scratch("tmp");
    let t = d.join("t");
    fs::create_dir_all(t.join("old")).unwrap();
    fs::create_dir_all(t.join(".X11-unix")).unwrap();
    fs::create_dir_all(t.join("keepme")).unwrap();
    let state = d.join("state");
    disk_gc(&[
        "apply",
        "--tmp",
        s(&t),
        "--tmp-days",
        "0",
        "--min-size",
        "0",
        "--protect",
        s(&t.join("keepme")),
        "--state-dir",
        s(&state),
    ]);
    assert!(!t.join("old").exists());
    assert!(t.join(".X11-unix").exists());
    assert!(t.join("keepme").exists());
    let _ = fs::remove_dir_all(&d);
}

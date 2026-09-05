//! Bridge host commands to the repository-local Fil-C Linux VM.
//!
//! Build this file to `.tmp/fil-c/bin/filcc` and copy the binary to
//! `.tmp/fil-c/bin/filrun`. The executable name selects compile or run mode.

use std::env;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const INSTANCE: &str = "recallfs-filc";

fn tool_root(executable: &Path) -> Result<PathBuf, String> {
    executable
        .parent()
        .and_then(Path::parent)
        .map(Path::to_path_buf)
        .ok_or_else(|| {
            format!(
                "{} must be installed as .tmp/fil-c/bin/filcc or filrun",
                executable.display()
            )
        })
}

fn executable_mode(executable: &Path) -> Result<&str, String> {
    match executable.file_name().and_then(|name| name.to_str()) {
        Some("filcc") => Ok("compile"),
        Some("filrun") => Ok("run"),
        _ => Err(format!(
            "unsupported executable name: {}",
            executable.display()
        )),
    }
}

fn require_file(path: &Path, description: &str) -> Result<(), String> {
    if path.is_file() {
        Ok(())
    } else {
        Err(format!("{description} not found: {}", path.display()))
    }
}

fn main() -> ExitCode {
    let executable = match env::current_exe() {
        Ok(path) => path,
        Err(error) => {
            eprintln!("filc bridge: cannot resolve executable: {error}");
            return ExitCode::FAILURE;
        }
    };
    let mode = match executable_mode(&executable) {
        Ok(mode) => mode,
        Err(error) => {
            eprintln!("filc bridge: {error}");
            return ExitCode::from(2);
        }
    };
    let root = match tool_root(&executable) {
        Ok(path) => path,
        Err(error) => {
            eprintln!("filc bridge: {error}");
            return ExitCode::from(2);
        }
    };

    let limactl = root.join("lima/bin/limactl");
    let lima_home = root.join("lima-home");
    let compiler = root.join("dist/build/bin/clang");
    if let Err(error) = require_file(&limactl, "Lima executable") {
        eprintln!("filc bridge: {error}");
        return ExitCode::FAILURE;
    }
    if mode == "compile" {
        if let Err(error) = require_file(&compiler, "Fil-C compiler") {
            eprintln!("filc bridge: {error}");
            return ExitCode::FAILURE;
        }
    }

    let mut arguments: Vec<OsString> = env::args_os().skip(1).collect();
    let guest_program = if mode == "compile" {
        compiler.into_os_string()
    } else if arguments.is_empty() {
        eprintln!("usage: filrun <program> [argument]...");
        return ExitCode::from(2);
    } else {
        arguments.remove(0)
    };

    let workdir = match env::current_dir() {
        Ok(path) => path,
        Err(error) => {
            eprintln!("filc bridge: cannot resolve working directory: {error}");
            return ExitCode::FAILURE;
        }
    };

    let status = Command::new(&limactl)
        .env("LIMA_HOME", &lima_home)
        .arg("--tty=false")
        .arg("shell")
        .arg("--start")
        .arg("--workdir")
        .arg(&workdir)
        .arg(INSTANCE)
        .arg("--")
        .arg(guest_program)
        .args(arguments)
        .status();

    match status {
        Ok(status) => ExitCode::from(status.code().unwrap_or(1) as u8),
        Err(error) => {
            eprintln!(
                "filc bridge: failed to invoke {}: {error}",
                limactl.display()
            );
            ExitCode::FAILURE
        }
    }
}

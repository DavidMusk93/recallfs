//! Validate Markdown `text` diagrams and print a column ruler for manual alignment review.
//!
//! This intentionally has no crate dependencies so it can run from a fresh checkout with
//! `rustc tools/verify_ascii_graphs.rs -o .tmp/verify-ascii-graphs`.

use std::env;
use std::fs;
use std::path::Path;
use std::process::ExitCode;

#[derive(Debug, PartialEq, Eq)]
struct Graph {
    start_line: usize,
    end_line: usize,
    lines: Vec<String>,
}

#[derive(Debug, Default, PartialEq, Eq)]
struct Report {
    graphs: Vec<Graph>,
    errors: Vec<String>,
}

fn inspect_markdown(path: &Path, content: &str) -> Report {
    let mut report = Report::default();
    let mut active: Option<Graph> = None;

    for (index, raw_line) in content.lines().enumerate() {
        let line_number = index + 1;
        let trimmed = raw_line.trim();

        if trimmed == "```text" {
            if active.is_some() {
                report.errors.push(format!(
                    "{}:{line_number}: nested text graph fence",
                    path.display()
                ));
            } else {
                active = Some(Graph {
                    start_line: line_number + 1,
                    end_line: line_number,
                    lines: Vec::new(),
                });
            }
            continue;
        }

        if trimmed == "```" && active.is_some() {
            let mut graph = active.take().expect("checked is_some");
            graph.end_line = line_number - 1;
            report.graphs.push(graph);
            continue;
        }

        let Some(graph) = active.as_mut() else {
            continue;
        };

        for byte in raw_line.bytes() {
            if !byte.is_ascii() {
                report.errors.push(format!(
                    "{}:{line_number}: non-ASCII byte in text graph",
                    path.display()
                ));
                break;
            }
        }
        if raw_line.contains('\t') {
            report.errors.push(format!(
                "{}:{line_number}: tab is not allowed in text graph",
                path.display()
            ));
        }
        if raw_line.ends_with(' ') {
            report.errors.push(format!(
                "{}:{line_number}: trailing whitespace in text graph",
                path.display()
            ));
        }
        graph.lines.push(raw_line.to_owned());
    }

    if let Some(graph) = active {
        report.errors.push(format!(
            "{}:{}: unclosed text graph fence",
            path.display(),
            graph.start_line - 1
        ));
    }

    report
}

fn ruler_line(width: usize, digits: bool) -> String {
    (0..width)
        .map(|column| {
            if digits {
                char::from(b'0' + (column % 10) as u8)
            } else if column % 10 == 0 {
                char::from(b'0' + ((column / 10) % 10) as u8)
            } else {
                ' '
            }
        })
        .collect()
}

fn print_ruler(path: &Path, graph_index: usize, graph: &Graph) {
    let width = graph.lines.iter().map(String::len).max().unwrap_or(0);
    println!(
        "{}: text graph {} (lines {}-{}, width {})",
        path.display(),
        graph_index + 1,
        graph.start_line,
        graph.end_line,
        width
    );
    println!("      {}", ruler_line(width, false));
    println!("      {}", ruler_line(width, true));
    for (offset, line) in graph.lines.iter().enumerate() {
        println!("{:>5} {}", graph.start_line + offset, line);
    }
}

fn usage() {
    eprintln!("usage: verify_ascii_graphs [--ruler] <markdown-file>...");
}

fn main() -> ExitCode {
    let mut show_ruler = false;
    let mut paths = Vec::new();

    for argument in env::args().skip(1) {
        match argument.as_str() {
            "--ruler" => show_ruler = true,
            "--help" | "-h" => {
                usage();
                return ExitCode::SUCCESS;
            }
            _ if argument.starts_with('-') => {
                eprintln!("unknown option: {argument}");
                usage();
                return ExitCode::from(2);
            }
            _ => paths.push(argument),
        }
    }

    if paths.is_empty() {
        usage();
        return ExitCode::from(2);
    }

    let mut valid = true;
    for path in paths {
        let path = Path::new(&path);
        match fs::read_to_string(path) {
            Ok(content) => {
                let report = inspect_markdown(path, &content);
                for error in &report.errors {
                    eprintln!("error: {error}");
                }
                if show_ruler {
                    for (index, graph) in report.graphs.iter().enumerate() {
                        print_ruler(path, index, graph);
                    }
                }
                valid &= report.errors.is_empty();
            }
            Err(error) => {
                eprintln!("error: {}: {error}", path.display());
                valid = false;
            }
        }
    }

    if valid {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_ascii_text_graph() {
        let report = inspect_markdown(
            Path::new("doc.md"),
            "before\n```text\nleft |--> right\n```\nafter\n",
        );
        assert!(report.errors.is_empty());
        assert_eq!(
            report.graphs,
            vec![Graph {
                start_line: 3,
                end_line: 3,
                lines: vec!["left |--> right".into()],
            }]
        );
    }

    #[test]
    fn rejects_non_ascii_tabs_and_trailing_whitespace() {
        let report = inspect_markdown(Path::new("doc.md"), "```text\nleft\t中 \n```\n");
        assert_eq!(report.errors.len(), 3);
    }

    #[test]
    fn renders_zero_based_ruler() {
        assert_eq!(ruler_line(12, false), "0         1 ");
        assert_eq!(ruler_line(12, true), "012345678901");
    }
}

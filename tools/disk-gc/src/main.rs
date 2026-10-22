use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match disk_gc::parse_args(&args) {
        Err(e) => {
            if !e.is_empty() {
                eprintln!("error: {e}\n");
            }
            eprintln!("{}", disk_gc::USAGE);
            ExitCode::from(2)
        }
        Ok(cfg) => match disk_gc::run(&cfg) {
            Ok(0) => ExitCode::SUCCESS,
            Ok(failed) => {
                eprintln!("error: {failed} action(s) failed, see the run log");
                ExitCode::from(1)
            }
            Err(e) => {
                eprintln!("error: {e}");
                ExitCode::from(1)
            }
        },
    }
}

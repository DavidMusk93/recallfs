fn extent_end(start_block: u64, block_count: u64) -> u64 {
    start_block + block_count
}

fn main() {
    let end = extent_end(1_000, 24);
    println!("ordinary Rust: extent [1000, {end})");
}

#[cfg(test)]
mod tests {
    use super::extent_end;

    #[test]
    fn computes_extent_end_for_typical_inputs() {
        assert_eq!(extent_end(0, 1), 1);
        assert_eq!(extent_end(1_000, 24), 1_024);
        assert_eq!(extent_end(8_192, 128), 8_320);
    }
}

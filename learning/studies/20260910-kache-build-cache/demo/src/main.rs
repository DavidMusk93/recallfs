fn fibonacci(n: u32) -> u64 {
    (0..n).fold((0_u64, 1_u64), |(a, b), _| (b, a + b)).0
}

fn main() {
    assert_eq!(fibonacci(20), 6765);
    println!("{}", fibonacci(20));
}

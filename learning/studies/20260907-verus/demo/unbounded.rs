use vstd::prelude::*;

verus! {

fn extent_end(start_block: u64, block_count: u64) -> (end: u64)
    ensures
        end == start_block + block_count,
{
    start_block + block_count
}

fn main() {}

} // verus!

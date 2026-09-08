use vstd::prelude::*;

verus! {

fn extent_end(start_block: u64, block_count: u64, capacity: u64) -> (end: u64)
    requires
        start_block <= capacity,
        block_count <= capacity - start_block,
    ensures
        end == start_block + block_count,
        start_block <= end <= capacity,
{
    start_block + block_count
}

fn main() {
    let end = extent_end(1_000, 24, 4_096);
    assert(end == 1_024);
}

} // verus!

use vstd::prelude::*;

verus! {

fn extent_end(start_block: u64, block_count: u64, capacity: u64) -> (end: u64)
    requires
        start_block <= capacity,
        0 < block_count <= capacity - start_block,
    ensures
        end == start_block + block_count,
        end <= capacity,
{
    start_block + block_count - 1
}

fn main() {}

} // verus!

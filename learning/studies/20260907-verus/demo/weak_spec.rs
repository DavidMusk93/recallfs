use vstd::prelude::*;

verus! {

fn extent_end(_start_block: u64, _block_count: u64, capacity: u64) -> (end: u64)
    ensures
        end <= capacity,
{
    0
}

fn main() {}

} // verus!

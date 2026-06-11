# Radix Top-K Source Archive

| Field | Value |
| --- | --- |
| URL | `https://veitner.bearblog.dev/radix-top-k/` |
| Access Date | `2026-06-11` |
| Topic | Radix Top-K selection without full sorting |

## Archived Content

Radix Top-K is an algorithm for finding the top-k elements in an array without sorting the full array.

For simplicity, assume the values are unsigned integers. The same idea can be extended to other representations.

Initial setup: choose `TOP_K` and `BITS_PER_ITER`, and assume every element can be represented with `NUM_BITS` bits.

Iteratively apply the following procedure:

1. Extract the next `BITS_PER_ITER` bits from all current candidates, starting from the most significant bits.
2. Count how many candidates fall into each bucket: `0, 1, ..., 2^{BITS_PER_ITER} - 1`.
3. Perform an inclusive scan over the bucket counts.
4. Let `K_remaining` be `TOP_K` minus the number of elements already known to be in the top-k. Select the first bucket index `i` where `inclusive_scan[i] >= K_remaining`. All elements in buckets `j < i` are guaranteed to be in the top-k. Elements in bucket `i` remain candidates for the next round. Elements in buckets `j > i` are discarded.
5. Repeat with the new candidates as input, updating `K_remaining` by subtracting the number of elements already guaranteed to be in the top-k.

After all bit chunks have been processed, if more candidates remain than open top-k slots, keep only as many as needed. This can happen when multiple values are tied at the boundary.

As written, this finds the `TOP_K` smallest values. For `TOP_K` largest values, reverse the bucket order.

## Notes

- The page includes a minimal PyTorch script using `bincount`, `cumsum`, and bucket pruning.
- The image in the original page was not archived locally.

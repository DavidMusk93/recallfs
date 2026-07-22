//! LeetCode 4. 寻找两个正序数组的中位数
//! https://leetcode.cn/problems/median-of-two-sorted-arrays/
//!
//! Binary partition：在较短数组上二分 cut `i`，数量不变量 `j = h - i`，
//! 顺序不变量交叉比较；端点用 ±∞ 哨兵统一。

pub struct Solution;

impl Solution {
    /// leetcode.cn Rust 模板签名。
    pub fn find_median_sorted_arrays(nums1: Vec<i32>, nums2: Vec<i32>) -> f64 {
        // 保证 A 较短，使 j = h - i 对任意 i ∈ [0, m] 落在 [0, n]
        let (a, b) = if nums1.len() <= nums2.len() {
            (nums1, nums2)
        } else {
            (nums2, nums1)
        };
        let m = a.len();
        let n = b.len();
        // 全局左袋 |L| = h；奇数时中位数落在 L 的边界
        let h = (m + n + 1) / 2;

        let mut lo: isize = 0;
        let mut hi: isize = m as isize;

        while lo <= hi {
            let i = ((lo + hi) / 2) as usize;
            let j = h - i;

            // 哨兵：缺侧 −∞ / +∞，交叉公式不变
            let a_im1 = if i == 0 { i32::MIN } else { a[i - 1] };
            let a_i = if i == m { i32::MAX } else { a[i] };
            let b_jm1 = if j == 0 { i32::MIN } else { b[j - 1] };
            let b_j = if j == n { i32::MAX } else { b[j] };

            if a_im1 > b_j {
                // A 贡献给 L 太多 → 缩小 cut i
                hi = i as isize - 1;
            } else if b_jm1 > a_i {
                // A 贡献给 L 太少 → 增大 cut i
                lo = i as isize + 1;
            } else {
                // 合法 partition
                let max_left = a_im1.max(b_jm1);
                if (m + n) % 2 == 1 {
                    return max_left as f64;
                }
                let min_right = a_i.min(b_j);
                return (max_left as f64 + min_right as f64) / 2.0;
            }
        }

        unreachable!("a valid partition always exists for sorted inputs");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn almost_eq(got: f64, expect: f64) {
        assert!(
            (got - expect).abs() < 1e-9,
            "got {got}, expect {expect}"
        );
    }

    #[test]
    fn sample_1_odd() {
        almost_eq(
            Solution::find_median_sorted_arrays(vec![1, 3], vec![2]),
            2.0,
        );
    }

    #[test]
    fn sample_2_even() {
        almost_eq(
            Solution::find_median_sorted_arrays(vec![1, 2], vec![3, 4]),
            2.5,
        );
    }

    #[test]
    fn one_empty() {
        almost_eq(
            Solution::find_median_sorted_arrays(vec![], vec![1]),
            1.0,
        );
        almost_eq(
            Solution::find_median_sorted_arrays(vec![2], vec![]),
            2.0,
        );
    }

    #[test]
    fn even_single_each() {
        almost_eq(
            Solution::find_median_sorted_arrays(vec![1], vec![2]),
            1.5,
        );
    }

    #[test]
    fn all_from_one_side() {
        // 合法 cut 可能让一侧不贡献给 L
        almost_eq(
            Solution::find_median_sorted_arrays(vec![1, 2], vec![3, 4, 5, 6]),
            3.5,
        );
    }

    #[test]
    fn negatives() {
        almost_eq(
            Solution::find_median_sorted_arrays(vec![-5, -3, -1], vec![-2, 0, 4]),
            -1.5,
        );
    }

    #[test]
    fn longer_first_swaps() {
        almost_eq(
            Solution::find_median_sorted_arrays(vec![1, 2, 3, 4, 5], vec![6]),
            3.5,
        );
    }
}

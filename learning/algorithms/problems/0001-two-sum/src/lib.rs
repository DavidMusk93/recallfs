//! LeetCode 1. 两数之和
//! https://leetcode.cn/problems/two-sum/
//!
//! 一遍哈希：扫到 x 时查 need = target - x；先查后写。

use std::collections::HashMap;

pub struct Solution;

impl Solution {
    /// leetcode.cn Rust 模板签名。
    pub fn two_sum(nums: Vec<i32>, target: i32) -> Vec<i32> {
        // value -> index（仅存更早出现的元素）
        let mut seen: HashMap<i32, i32> = HashMap::with_capacity(nums.len());

        for (i, &x) in nums.iter().enumerate() {
            let need = target - x;
            if let Some(&j) = seen.get(&need) {
                return vec![j, i as i32];
            }
            // 先查后写：避免同一下标用两次
            seen.insert(x, i as i32);
        }

        // 题设保证恰好一解；本地测试若走到这里说明样例有误
        unreachable!("problem guarantees exactly one solution");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sorted_pair(mut v: Vec<i32>) -> Vec<i32> {
        v.sort_unstable();
        v
    }

    #[test]
    fn sample_1() {
        assert_eq!(
            sorted_pair(Solution::two_sum(vec![2, 7, 11, 15], 9)),
            vec![0, 1]
        );
    }

    #[test]
    fn sample_2() {
        assert_eq!(
            sorted_pair(Solution::two_sum(vec![3, 2, 4], 6)),
            vec![1, 2]
        );
    }

    #[test]
    fn sample_3_same_value_different_indices() {
        assert_eq!(
            sorted_pair(Solution::two_sum(vec![3, 3], 6)),
            vec![0, 1]
        );
    }

    #[test]
    fn negatives_and_zero() {
        assert_eq!(
            sorted_pair(Solution::two_sum(vec![-1, 0, 1], 0)),
            vec![0, 2]
        );
    }
}

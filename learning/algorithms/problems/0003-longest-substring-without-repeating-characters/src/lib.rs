//! LeetCode 3. 无重复字符的最长子串
//! https://leetcode.cn/problems/longest-substring-without-repeating-characters/
//!
//! 主推：sliding window + last index。
//! left = max(left, last[c]+1) 保证窗口 s[left..=right] 无重复且 left 单调不减。

use std::collections::HashMap;

pub struct Solution;

impl Solution {
    /// leetcode.cn Rust 模板签名。
    pub fn length_of_longest_substring(s: String) -> i32 {
        let bytes = s.as_bytes();
        let n = bytes.len();
        // char(byte) -> last index；ASCII 场景也可用 [usize; 128]
        let mut last: HashMap<u8, usize> = HashMap::with_capacity(n.min(128));
        let mut left = 0usize;
        let mut ans = 0i32;

        for (right, &c) in bytes.iter().enumerate() {
            if let Some(&prev) = last.get(&c) {
                // 仅当旧位置仍在当前窗口内时才需要收缩；
                // max 防止 left 回退（旧 last 已过期）。
                if prev >= left {
                    left = prev + 1;
                }
            }
            last.insert(c, right);
            let len = (right - left + 1) as i32;
            if len > ans {
                ans = len;
            }
        }

        ans
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sample_1() {
        assert_eq!(
            Solution::length_of_longest_substring("abcabcbb".into()),
            3
        );
    }

    #[test]
    fn sample_2() {
        assert_eq!(Solution::length_of_longest_substring("bbbbb".into()), 1);
    }

    #[test]
    fn sample_3() {
        assert_eq!(Solution::length_of_longest_substring("pwwkew".into()), 3);
    }

    #[test]
    fn empty() {
        assert_eq!(Solution::length_of_longest_substring("".into()), 0);
    }

    #[test]
    fn abba_needs_max_left() {
        // 经典：若 left 误写成 last[c]+1 而不 max，right 扫到最后 'a' 会回退 left
        assert_eq!(Solution::length_of_longest_substring("abba".into()), 2);
    }

    #[test]
    fn all_unique() {
        assert_eq!(Solution::length_of_longest_substring("abcdef".into()), 6);
    }

    #[test]
    fn single() {
        assert_eq!(Solution::length_of_longest_substring("a".into()), 1);
    }
}

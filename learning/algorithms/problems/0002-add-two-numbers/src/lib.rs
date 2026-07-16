//! LeetCode 2. 两数相加
//! https://leetcode.cn/problems/add-two-numbers/
//!
//! 主推：按位迭代 + carry。低位在 head，循环条件含最终 carry。

// leetcode.cn 会预置 ListNode；本地 crate 自带定义以便 cargo test。
#[derive(PartialEq, Eq, Clone, Debug)]
pub struct ListNode {
    pub val: i32,
    pub next: Option<Box<ListNode>>,
}

impl ListNode {
    #[inline]
    pub fn new(val: i32) -> Self {
        ListNode { next: None, val }
    }
}

pub struct Solution;

impl Solution {
    /// leetcode.cn Rust 模板签名。
    pub fn add_two_numbers(
        mut l1: Option<Box<ListNode>>,
        mut l2: Option<Box<ListNode>>,
    ) -> Option<Box<ListNode>> {
        let mut dummy = Box::new(ListNode::new(0));
        // tail 始终指向结果链最后一个已写入节点
        let mut tail = dummy.as_mut();
        let mut carry = 0;

        // 任一链还有节点，或还有进位，就必须继续
        while l1.is_some() || l2.is_some() || carry != 0 {
            let a = l1.as_ref().map(|n| n.val).unwrap_or(0);
            let b = l2.as_ref().map(|n| n.val).unwrap_or(0);
            let sum = a + b + carry;
            carry = sum / 10;

            tail.next = Some(Box::new(ListNode::new(sum % 10)));
            tail = tail.next.as_mut().unwrap();

            l1 = l1.and_then(|n| n.next);
            l2 = l2.and_then(|n| n.next);
        }

        dummy.next
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn from_digits(digits: &[i32]) -> Option<Box<ListNode>> {
        let mut head: Option<Box<ListNode>> = None;
        for &d in digits.iter().rev() {
            let mut node = Box::new(ListNode::new(d));
            node.next = head;
            head = Some(node);
        }
        head
    }

    fn to_digits(mut head: Option<Box<ListNode>>) -> Vec<i32> {
        let mut out = Vec::new();
        while let Some(n) = head {
            out.push(n.val);
            head = n.next;
        }
        out
    }

    #[test]
    fn sample_1() {
        // 342 + 465 = 807 → [7,0,8]
        let l1 = from_digits(&[2, 4, 3]);
        let l2 = from_digits(&[5, 6, 4]);
        assert_eq!(to_digits(Solution::add_two_numbers(l1, l2)), vec![7, 0, 8]);
    }

    #[test]
    fn sample_2_zeros() {
        let l1 = from_digits(&[0]);
        let l2 = from_digits(&[0]);
        assert_eq!(to_digits(Solution::add_two_numbers(l1, l2)), vec![0]);
    }

    #[test]
    fn sample_3_long_carry() {
        // 9999999 + 9999 → 10009998 → [8,9,9,9,0,0,0,1]
        let l1 = from_digits(&[9, 9, 9, 9, 9, 9, 9]);
        let l2 = from_digits(&[9, 9, 9, 9]);
        assert_eq!(
            to_digits(Solution::add_two_numbers(l1, l2)),
            vec![8, 9, 9, 9, 0, 0, 0, 1]
        );
    }

    #[test]
    fn unequal_length_short_plus_long() {
        // 9 + 999 = 1008 → [8,0,0,1]
        let l1 = from_digits(&[9]);
        let l2 = from_digits(&[9, 9, 9]);
        assert_eq!(
            to_digits(Solution::add_two_numbers(l1, l2)),
            vec![8, 0, 0, 1]
        );
    }

    #[test]
    fn final_carry_only() {
        // 5 + 5 = 10 → [0,1]
        let l1 = from_digits(&[5]);
        let l2 = from_digits(&[5]);
        assert_eq!(to_digits(Solution::add_two_numbers(l1, l2)), vec![0, 1]);
    }
}

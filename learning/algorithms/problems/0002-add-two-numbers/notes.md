# 2. 两数相加 · notes

## 一句话

逆序链表加法 = 竖式：`s = a + b + carry`，写 `s%10`，`carry = s/10`；循环条件含最终 carry。

## 实现要点（Rust）

| 点 | 做法 |
| --- | --- |
| 头节点 | dummy head，返回 `dummy.next` |
| 短链 | `as_ref().map(\|n\| n.val).unwrap_or(0)` |
| 前进 | `l1 = l1.and_then(\|n\| n.next)` |
| 尾指针 | `tail` 始终指已写完的最后一节 |

## 易错

1. 两链都空了但 `carry == 1` → 还要挂节点（`5+5`）。  
2. 不要先转 `i128`：位数到 100。  
3. 输入已是 reverse order，**不要**先 reverse。

## 理解测备忘（本次会话）

- 手推输出 `[7,0,8]` 时注意用英文逗号。  
- 图解/不等长/尾 carry 值得对照实现。

## 提交

- 本地：`cargo test -p p0002_add_two_numbers`
- 粘贴：`add_two_numbers` + 平台预置 `ListNode`（本地 `lib.rs` 含定义，提交时若重复定义则删本地 struct）

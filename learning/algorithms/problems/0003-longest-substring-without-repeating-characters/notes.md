# 3. 无重复字符的最长子串 · notes

## 一句话

滑动窗口 + last index：`left = max(left, last[c]+1)`，维护最长无重复 substring 长度。

## 为何必须 max

| 写法 | 问题 |
| --- | --- |
| 总是 `left = last[c]+1` | 旧 last 已在窗口外时会把 left **往回拉**，窗口重新引入已排除字符（如 `abba`） |
| `if last[c] >= left { left = last[c]+1 }` | 与 max 等价，语义更直白 |
| `left = max(left, last[c]+1)` | 一行版；left 单调 |

手推 `abba` 见 learn.html「为什么是 max」一节。

## Rust

- 签名：`length_of_longest_substring(s: String) -> i32`
- 可用 `HashMap<u8, usize>` 或定长数组（ASCII）

## 提交后

- 本地：`cargo test -p p0003_longest_substring_without_repeating_characters`

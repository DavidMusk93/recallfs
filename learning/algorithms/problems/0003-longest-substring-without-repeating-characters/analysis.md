# 3. 无重复字符的最长子串 · 分析

## 1. 结论

| 项 | 内容 |
| --- | --- |
| 一句话题意 | 在字符串 `s` 中找 **无重复字符的最长 substring** 的长度（不是 subsequence）。 |
| 主推思路 | **sliding window** + 记录字符 **last index**：右指针扫全串；遇重复则把左边界跳到 `last[c]+1`（且不回退）。 |
| 时间 / 空间 | O(n) / O(min(n, Σ))，Σ 为字符集（ASCII≈128，Unicode 更宽） |
| 关联 pattern | `sliding-window-unique`（可变窗口 + 唯一性约束） |
| 难度 | Medium · 免费 |
| 链接 | https://leetcode.cn/problems/longest-substring-without-repeating-characters/ |

```text
s[i..j] 始终「窗口内字符互异」
  right 扩张 → 可能破坏唯一性
  left  跳过 → 恢复唯一性
  ans = max(ans, right-left+1)
```

## 2. 题面形式化

### 2.1 输入 / 输出 / 约束

| 项 | 内容 |
| --- | --- |
| 输入 | `s`：字符串 |
| 输出 | 最长无重复 substring 的 **长度**（`i32`） |
| 长度 | `0 ≤ s.len() ≤ 5·10^4`（以 leetcode.cn 题面为准） |
| 字符 | 常为英文字母、数字、符号、空格等 |
| Rust | `length_of_longest_substring(s: String) -> i32` |

样例：

| s | 输出 | 解释 |
| --- | ---: | --- |
| `abcabcbb` | 3 | `abc` |
| `bbbbb` | 1 | `b` |
| `pwwkew` | 3 | `wke`（`pwke` 是 subsequence，不算） |
| `""` | 0 | 空串 |

### 2.2 边界与陷阱

- **substring ≠ subsequence**：必须连续。  
- 左指针 **只右移不左移**（单调）：`left = max(left, last[c]+1)`。  
- 同字符再次出现时，若旧位置已在窗口外，**不要**错误收缩。  
- 空串、单字符、全相同、全不同。

## 3. 思路演进

### 3.1 Brute force

```text
对每个 L,R 检查 s[L..=R] 是否无重复 → O(n²)~O(n³)
```

n=5e4 不可行。价值：定义「合法窗口」。

### 3.2 Sliding window + set（慢一拍）

```text
right 扩张；while 窗口内有重复：left++ 并删字符
```

均摊 O(n)，但 left 一步步挪。有 **last index** 时可一次跳到位。

### 3.3 主推：window + last index（HashMap）

```text
+-----------------------------+
| last: char -> 上次出现下标   |
| left = 0, ans = 0           |
+-------------+---------------+
              |
              v
+-----------------------------+
| for right, c in s:          |
|   if c 见过且 last[c] >= left|
|     left = last[c] + 1      |
|   last[c] = right           |
|   ans = max(ans, right-left+1)
+-----------------------------+
```

| 对比 | Brute | Window + set | Window + last index |
| --- | --- | --- | --- |
| 时间 | 过大 | O(n) 均摊 | O(n) |
| 实现 | 直观 | 稍繁 | 一次跳 left |
| 优化什么 | — | 避免重复检查 | 避免 left 一步步挪 |

## 4. 社区灵感

| 来源 | 吸收点 |
| --- | --- |
| 官方题解「滑动窗口」 | 双指针维护区间唯一性 |
| 高频写法 | `last[c]` + `left = max(left, last+1)` |
| 标签 | Hash Table · String · Sliding Window |

## 5. 与 patterns 的关系

- 与 two-sum 的 HashMap 不同：这里 Map 存 **下标** 管窗口边界，不是补数。  
- 后续：最小覆盖子串、至多 K 个不同字符等，同属 sliding window 家族。

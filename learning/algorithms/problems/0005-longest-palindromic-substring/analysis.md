# 5. 最长回文子串 · 分析

## 1. 结论

| 项 | 内容 |
| --- | --- |
| 一句话题意 | 给定字符串 \(s\)，返回其中**最长的回文 substring**（连续子串；多解任返一个）。 |
| 主推思路 | **Expand around centers**：a palindrome is fixed by its center; expand left/right; keep the longest span. |
| 时间 / 空间 | **\(O(n^2)\)** / \(O(1)\)（中心扩展）；DP 同时间但空间 \(O(n^2)\) |
| 关联 pattern | `expand-around-centers` |
| 难度 | **Medium** · 免费 |
| 链接 | https://leetcode.cn/problems/longest-palindromic-substring/ |

```text
palindrome ⇔ mirror around a center
  odd:  center on a character   … a b a …
  even: center in the gap       … a b b a …
~2n−1 centers; each expands O(n) → O(n²)
no need to enumerate all endpoint pairs
```

## 2. 题面形式化

### 2.1 输入 / 输出 / 约束

| 项 | 内容 |
| --- | --- |
| 输入 | 字符串 \(s\) |
| 输出 | 最长回文 substring（字符串）；同长多解返回任一 |
| 规模 | \(1 \le |s| \le 1000\)（以 leetcode.cn 为准） |
| 字符集 | 数字与英文字母 |
| Rust | `longest_palindrome(s: String) -> String` |

样例：

| \(s\) | 合法答案之一 |
| --- | --- |
| `"babad"` | `"bab"` 或 `"aba"` |
| `"cbbd"` | `"bb"` |

### 2.2 边界

- 单字符：自身即答案。  
- 全相同：整串。  
- 无长度 \(\ge 2\) 的回文：返回任一单字符。  
- 注意 **substring（连续）** ≠ subsequence（可跳）。

## 3. 思路演进

### 3.1 暴力 \(O(n^3)\)

枚举所有 \([L,R]\)，再 \(O(n)\) 判回文。建立定义，n=1000 偏紧。

### 3.2 Recommended: expand around centers \(O(n^2)/O(1)\)

```text
for each center (odd on char / even in gap):
  seed L, R
  while L≥0 && R<n && s[L]==s[R]:
    L--; R++
  record longer span [L+1, R-1]

odd:  seed L=R=i
even: seed L=i, R=i+1
```

| 对比 | brute | expand | DP |
| --- | --- | --- | --- |
| time | \(O(n^3)\) | \(O(n^2)\) | \(O(n^2)\) |
| space | \(O(1)\) | \(O(1)\) | \(O(n^2)\) |
| hard part | low | odd + even centers | fill order |

### 3.3 变体：DP

\[
\mathrm{dp}[i][j] = (s[i]=s[j]) \land \bigl((j-i\le 2) \lor \mathrm{dp}[i+1][j-1]\bigr)
\]

由短到长填表。正确但更吃空间；教学上中心扩展更贴「回文 = 对称」。

### 3.4 进阶：Manacher \(O(n)\)

利用已算回文半径的对称性。面试少手写；知道存在即可。

## 4. 社区灵感

| 来源 | 吸收点 |
| --- | --- |
| 官方「中心扩展」 | 2n−1 个中心覆盖奇偶 |
| 高频写法 | 统一 `expand(l,r)` 辅助函数 |
| 与题 3 对比 | 题 3 是 unique 窗口；本题是 **对称扩展**，不是 last-index 窗口 |

## 5. 卡点预期

- substring vs subsequence  
- why both odd centers (on char) and even centers (in gap)  
- after expand stops, span is \([L+1, R-1]\)  
- ties: any max-length answer is OK  

# Pattern · sliding-window-unique

## 何时用

- 答案是 **连续区间**（substring / subarray）
- 区间约束是「内部元素唯一」或可随两端指针单调维护

## 核心

```text
right 扩张
  若破坏唯一性：
    left 右移（一步步或一次跳到 last[c]+1）
  更新答案 = max(长度)
left 只增不减 → 总时间 O(n)
```

## last index 要点

```text
left = max(left, last[c] + 1)
```

防止过期的 last 把 left 拉回去。

## 代表题

| id | slug |
| --- | --- |
| 3 | longest-substring-without-repeating-characters |

## 家族

- 至多 K 个不同字符
- 最小覆盖子串（约束从「唯一」换成「覆盖」）

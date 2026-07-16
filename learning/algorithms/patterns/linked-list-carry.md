# Pattern · linked-list-carry

## 何时用

- 数字按 **digit 链表** 存（常 reverse：低位在 head）
- 需要对齐位相加 / 减，并处理 **carry**

## 不变量

```text
每一步处理「当前位」：
  s = digit1 + digit2 + carry
  写出 s % base
  carry' = s / base
缺位 digit = 0；结束条件含 carry ≠ 0
```

## 复杂度

Time O(max(m,n)) · Space O(1) 辅助（输出另计）

## 代表题

| id | slug | 备注 |
| --- | --- | --- |
| 2 | add-two-numbers | 十进制 base=10，逆序 |
| 445 | add-two-numbers-ii | 高位在前 → reverse 或栈 |

## 变体

| 变化 | 调整 |
| --- | --- |
| base=2 | 二进制链表相加 |
| 高位在前 | reverse 或从尾递归 |
| 字符串大数 | 同一模型，换数组下标 |

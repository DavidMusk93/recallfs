# Postgres SELECT DISTINCT Does Not Scale

- 原文：https://www.dbos.dev/blog/postgres-select-distinct-does-not-scale
- 作者：Peter Kraft
- 发布日期：2026-08-10
- 访问日期：2026-09-09
- 类型：DBOS benchmark blog
- 原始 HTML SHA-256：
  `0b986ec69aef49cfc5dfbc4eff4ff297c47c72c9acafe06051ca443e312325b7`

## 归档说明

页面正文和代码主要由 Webflow HTML 与图片组成。为避免提交重复站点模板和
第三方二进制，本仓库保留结构化摘要、关键 SQL 转录、原始 URL 与内容
digest，不提交原始 HTML 和图片。

## 原文问题

DBOS 使用 PostgreSQL 表保存持久化队列。每个 queue 下有多个
`queue_partition_key`，dequeue 前需要枚举所有存在 `ENQUEUED` workflow
的 active partition：

```sql
SELECT DISTINCT queue_partition_key
FROM dbos.workflow_status
WHERE queue_name = 'example_queue'
  AND status = 'ENQUEUED'
  AND queue_partition_key IS NOT NULL;
```

对应索引的列顺序是：

```sql
CREATE INDEX idx_workflow_status_partition_dequeue
ON dbos.workflow_status (
    queue_name,
    status,
    queue_partition_key
)
WHERE status IN ('ENQUEUED', 'PENDING')
  AND queue_partition_key IS NOT NULL;
```

## 原文核心主张

1. 当 `queue_name` 和 `status` 固定时，B-tree 已按
   `queue_partition_key` 排序，但 PostgreSQL 的普通 `DISTINCT` 仍读取
   每个匹配索引项，再由 `Unique` 去重。
2. 固定 10 个 partition、把每个 partition 的行数从 100 增到 1,000,000
   时，普通查询延迟随匹配行数近似线性增长。
3. PostgreSQL 18 的 multicolumn index skip scan 解决的是缺失前导列约束
   的访问问题，不是返回索引前缀唯一值的 loose index scan，因此不改变
   这个查询。
4. 可用递归 CTE 模拟 loose index scan：先取最小 key，随后反复寻找严格
   大于当前 key 的最小值。

原文 workaround：

```sql
WITH RECURSIVE partitions(pk) AS (
    SELECT min(queue_partition_key)
    FROM dbos.workflow_status
    WHERE queue_name = 'example_queue'
      AND status = 'ENQUEUED'
      AND queue_partition_key IS NOT NULL

    UNION ALL

    SELECT (
        SELECT min(queue_partition_key)
        FROM dbos.workflow_status
        WHERE queue_name = 'example_queue'
          AND status = 'ENQUEUED'
          AND queue_partition_key > partitions.pk
    )
    FROM partitions
    WHERE partitions.pk IS NOT NULL
)
SELECT pk
FROM partitions
WHERE pk IS NOT NULL;
```

## 需要独立验证的边界

- 原文的 “O(number of partitions)” 是工程近似。B-tree 每次重新定位仍有
  seek 成本，更准确地说接近 `O(D log N)`，其中 `N` 是匹配行数、`D`
  是不同 key 数量。
- 当 `D` 接近 `N` 时，递归 CTE 的大量重复 seek 可能比一次顺序索引扫描
  更慢。
- PostgreSQL 18 的 skip scan 和 loose index scan 必须分开验证，不能因
  名称相似而视为同一种算子。
- 单机 CTE 的每次 seek 是本地调用；若分布式实现把每次 seek 变成一次远程
  RPC，低行数复杂度仍可能对应很差的 wall time。

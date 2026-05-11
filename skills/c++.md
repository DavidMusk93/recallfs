优化方案需要根据类别和环境来确定。

# 计算密集

- 消除高周期指令，如除法使用`libdivide` 库、利用定点数近似计算。
- simd。

# 内存密集

- 优化数据结构。讨好cache，tile 级别控制批处理数据量。

# 数据结构

## queue

队列一般是解耦利器。但使用要识别场景，比如spsc or mpsc 等。

优化要根据场景。

## map or set

使用cache 友好的数据结构。比如flat map or set。

以boost 为例，使用flat 结构时，还以搭配hestogeneous operation: 可以通过std::string\_view 查询std::string 类型。

## lock

mutex 在竞争轻度的情况下，性能较好。场景简单，尽量使用lock free 的数据解耦。

分析锁的区域和类型，避免死锁和不必要的锁。


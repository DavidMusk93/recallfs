# Blade Test 排障记录

## 文档信息

| 字段 | 内容 |
| --- | --- |
| Topic | `blade-test` |
| Kind | `troubleshooting` |
| Status | `draft` |
| 适用场景 | 本地执行 `blade test`、新增 `cc_test`、定位构建 / 运行失败 |

## 一页总结

这次排障覆盖了 4 类典型问题：

1. `blade` 默认目录不可写
2. `sailfish/goma` 在当前环境下无法启动
3. 新增测试 target 缺失链接依赖
4. 测试进程启动期崩溃与测试用例本身失败要分开定位

最终结论：

- 在当前 devbox / sandbox 环境里，跑本地 `blade test` 建议显式关闭 `sailfish/cas`
- 新增 `cc_test` 时，依赖项需要按真实链接链路补全，不能只照抄最小 target
- 先把“环境阻断”剥离，再看“编译错误”，最后看“测试断言失败”，排障效率最高

## 推荐命令

当前仓库中执行单个测试，推荐优先使用下面这套环境：

```bash
HOME=/root/Documents/fringedb/.blade-home \
TMPDIR=/root/Documents/fringedb/.blade-home/tmp \
TMP_DIR=/root/Documents/fringedb/.blade-home/tmp \
XDG_RUNTIME_DIR=/root/Documents/fringedb/.blade-home/runtime \
BLADE_SAILFISH_SKIP=1 \
DISABLE_CAS=1 \
blade test //:partition_consumer_ut --bundle=release --verbose
```

目的：

- 把 `blade` 默认写目录从 `/root/.blade` 重定向到仓库内可写目录
- 禁用 `sailfish/goma` 远端链路，强制落到本地真实编译与测试执行

## 问题 1：`blade` 默认目录不可写

### 症状

首次执行 `blade test` 时直接失败，报错写入：

- `/root/.blade/blade_status/...`
- `/root/.blade/dir.txt`

### 根因

当前执行环境下，`/root/.blade` 不可写。

### 解决方式

把 `HOME` 切到仓库内自建目录，例如：

```bash
mkdir -p /root/Documents/fringedb/.blade-home
HOME=/root/Documents/fringedb/.blade-home blade test ...
```

### 经验

- `blade` 不只是“读代码”，还会写状态目录
- 这类问题和代码本身无关，应先排除，否则后续报错都是噪音

## 问题 2：`sailfish/goma` 启动失败

### 症状

即使切换了 `HOME`，`blade` 仍然失败，日志出现：

- `temp dir (/run/user/0/goma_unknown) is not owned by you`
- `compiler_proxy 启动失败`
- `SAILFISH启动失败`

### 根因

`blade` 默认仍会尝试走 `sailfish/goma` / `CAS` 远端编译链路，而当前环境下对应临时目录和代理进程无法正常启动。

### 证据

`blade` 自身源码支持以下开关：

- `BLADE_SAILFISH_SKIP=1`
- `DISABLE_CAS=1`

### 解决方式

显式关闭远端链路：

```bash
BLADE_SAILFISH_SKIP=1 DISABLE_CAS=1 blade test ...
```

并同时设置：

```bash
TMPDIR=/root/Documents/fringedb/.blade-home/tmp
TMP_DIR=/root/Documents/fringedb/.blade-home/tmp
XDG_RUNTIME_DIR=/root/Documents/fringedb/.blade-home/runtime
```

### 经验

- 遇到 `sailfish/goma` 报错时，不要继续猜测试代码
- 先把构建链路切回本地，拿到“真实编译错误”再继续

## 问题 3：新增测试 target 缺少 `#lzma`

### 症状

在关闭 `sailfish/cas` 后，构建推进到真实链接阶段，出现：

```text
libunwind.a(elf64.o): undefined reference to 'lzma_*'
```

### 根因

`partition_consumer_ut` target 没有显式依赖 `#lzma`，而链接链路中的 `libunwind` 需要解析 `lzma_*` 符号。

### 解决方式

在 `BUILD` 中为 `partition_consumer_ut` 增加：

```python
deps = [
    "#lzma",
    ":fringedb",
    "cpp3rdlib/jemalloc:sys-5.2.1.bm.2@//cpp3rdlib/jemalloc:jemalloc",
    "cpp3rdlib/gtest:1.10.0@//cpp3rdlib/gtest:gtest",
]
```

### 经验

- 新增 `cc_test` 时不要只看“能不能编译这个 `.cc`”
- 要看最终二进制的真实链接依赖
- 如果错误出现在 `undefined reference`，优先检查 target `deps`

## 问题 4：测试进程启动期 `SIGSEGV`

### 症状

补完 `#lzma` 后，测试可以编译并启动，但运行直接 `SIGSEGV:-11`。

### 证据

使用 `gdb` 抓到的栈停在：

- `ASan`
- `jemalloc`
- `dlsym`
- 动态加载初始化

也就是说，崩溃发生在进入测试主体之前，不是测试 case 内部逻辑。

### 根因

对这个新增 target 来说，`asan` 版本的构建在当前环境下会在进程启动期崩溃。

这说明：

- 当前失败不是 `PartitionConsumer` 逻辑错误
- 也不是测试断言错误
- 而是测试 target 的运行时装配问题

### 解决方式

对 `partition_consumer_ut` 这个新增测试 target，去掉 ASan 相关编译 / 链接参数，保留普通 `cc_test`：

```python
extra_cppflags = [
    "-ggdb3 -Og -std=c++17 -Wall -Wextra",
]
```

### 经验

- “进程启动前就崩”与“测试逻辑失败”是两类问题
- 先让测试正常启动，才能讨论 case 是否正确
- 对局部新增单测，优先保证可运行；需要 sanitizer 时再单独补一轮环境兼容

## 问题 5：测试用例本身的前提错误

### 症状

在构建与运行都恢复正常后，只剩 1 个失败用例：

- `event_max_index_can_move_ahead_of_queue_contents`

### 根因

测试错误地假设：

- `PartitionConsumer(1)` 等价于“队列可存 1 条事件”

但 `folly::ProducerConsumerQueue` 的真实语义是：

- 构造参数 `size >= 2`
- 可用槽位数是 `size - 1`

因此：

- `size=1` 不是“可存 1 条”
- 该断言前提本身错误

### 解决方式

把测试改成稳定可验证的满队列场景：

- `PartitionConsumer(2)`，即可用槽位正好为 1
- 先写入第一条事件
- 再写入第二条事件
- 验证第二条事件可能丢弃，但 `event_max_index_` 已更新

### 经验

- 写并发 / 队列类单测时，先确认底层容器的容量语义
- 不要把“构造参数”直接等同于“可用容量”

## 推荐排障顺序

以后遇到 `blade test` 失败，建议严格按这个顺序排：

1. 先排环境
   - `HOME`
   - `TMPDIR`
   - `XDG_RUNTIME_DIR`
   - `sailfish/goma/cas`
2. 再排构建
   - 缺头文件
   - 缺链接依赖
   - target `deps` 不完整
3. 再排运行
   - 进程启动期崩溃
   - 动态库找不到
   - sanitizer / allocator 冲突
4. 最后才排测试逻辑
   - 断言错误
   - 误解第三方库语义
   - 用例设计不稳定

## 新增测试时的建议

### `BUILD` 侧

- 优先从“最接近的成功 target”复制，而不是从“最小 target”开始猜
- 明确检查是否需要：
  - `#lzma`
  - `:fringedb`
  - `gtest`
  - `test/util.cc`
  - sanitizer 相关依赖

### 代码侧

- 单测 helper 尽量最小化
- 先验证最稳定的纯逻辑 case
- 有底层容器时，先确认容量、顺序、边界语义

### 运行侧

- 本地排障优先关闭 `sailfish/cas`
- 确认测试 binary 能正常启动，再加复杂配置

## 本次可直接复用的经验

| 场景 | 建议 |
| --- | --- |
| `blade` 一启动就报写文件失败 | 先重定向 `HOME` |
| `sailfish/goma` 启动失败 | 先加 `BLADE_SAILFISH_SKIP=1 DISABLE_CAS=1` |
| `undefined reference` | 优先检查 target `deps` |
| `SIGSEGV` 发生在 main 之前 | 优先怀疑运行时装配，不要先改测试逻辑 |
| 队列类单测不稳定 | 先读底层队列实现，确认容量语义 |

## 当前已验证命令

以下命令已在当前仓库验证通过：

```bash
HOME=/root/Documents/fringedb/.blade-home \
TMPDIR=/root/Documents/fringedb/.blade-home/tmp \
TMP_DIR=/root/Documents/fringedb/.blade-home/tmp \
XDG_RUNTIME_DIR=/root/Documents/fringedb/.blade-home/runtime \
BLADE_SAILFISH_SKIP=1 \
DISABLE_CAS=1 \
blade test //:partition_consumer_ut --bundle=release --verbose
```

结果：

- `partition_consumer_ut`
- `6 tests`
- `All tests passed`

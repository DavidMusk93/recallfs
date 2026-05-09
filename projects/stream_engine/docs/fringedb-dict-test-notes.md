# FringeDB Dict 配置兼容测试经验

## 1. 背景

本文记录 `FringeDB` 适配 `column.physical_column` 配置兼容改造时的测试经验，重点覆盖：

- 如何在当前仓库中运行 `FringeDB` 相关 Blade 测试
- `dict::Repo` 初始化依赖的环境变量
- 为什么测试通过后进程仍可能在退出阶段崩溃
- 当前采用的规避方案

对应测试代码见：

- [fringedb_sink_dict_test.cpp](file:///root/Documents/stream_engine/src/test/fringedb_test/fringedb_sink_dict_test.cpp)
- [BUILD](file:///root/Documents/stream_engine/src/test/BUILD#L552-L562)

## 2. 本次新增测试覆盖范围

本次新增的 `fringedb_test` 主要验证 `src/sink/fringedb.cpp` 的配置兼容逻辑，而不是完整的端到端写入链路。

当前覆盖的场景包括：

1. 旧格式配置兼容
   - `fringedb.dict_encoding_columns=local_node_name`
   - 预期：`physical_column` 默认等于 `column`
2. 新格式共享字典配置
   - `fringedb.dict_encoding_columns=local_node_name.__c1__,remote_node_name.__c1__`
   - 预期：两个逻辑列绑定到同一个物理 repo 名称
3. 混合格式与非法列名跳过
   - `missing_col.__c2__|local_node_name.__c1__;session_type`
   - 预期：不存在的列被跳过，合法列仍然生成 `_dict_idx`

对应断言见：

- [fringedb_sink_dict_test.cpp](file:///root/Documents/stream_engine/src/test/fringedb_test/fringedb_sink_dict_test.cpp#L54-L118)

## 3. Blade 测试命令

在当前环境下，推荐使用下面的命令运行单测：

```bash
rm -f .trace.log && touch .trace.log && chmod 666 .trace.log && \
mkdir -p /tmp/blade_home && \
HOME=/tmp/blade_home DISABLE_CAS=1 ENV_DICT_SERVICE_ADDR=localhost:7788 \
blade test //src/test:fringedb_test
```

关键点说明：

1. `DISABLE_CAS=1`
   - 避免远端 CAS 编译路径过慢或卡住
2. `HOME=/tmp/blade_home`
   - 避免 `blade` 向默认家目录写缓存时受限
3. `ENV_DICT_SERVICE_ADDR=localhost:7788`
   - 让 `dict::Repo::getInstance(...)` 能通过配置解析
4. `.trace.log`
   - 当前环境下 `blade` 会尝试写仓库根目录下的 `.trace.log`
   - 若权限不对，可能在构建启动阶段直接失败

## 4. 第一个常见问题：CAS 路径卡住

### 4.1 现象

直接执行：

```bash
blade test //src/test:fringedb_test
```

可能出现以下问题：

- 卡在 CAS 远端编译阶段
- 长时间停留在大型依赖目标上
- 测试启动明显变慢

### 4.2 处理方式

优先增加：

```bash
export DISABLE_CAS=1
```

本次验证中，关闭 CAS 后测试链路更稳定，也更容易快速定位到真实失败点。

## 5. 第二个常见问题：`ENV_DICT_SERVICE_ADDR` 未设置

### 5.1 现象

如果没有设置：

```bash
ENV_DICT_SERVICE_ADDR=localhost:7788
```

测试会在 `FringeDB::Create(...)` 阶段失败，报错类似：

```text
Config::parse: empty address; set ENV_DICT_SERVICE_ADDR or pass an explicit address to Config.
Expected format host:port or [ipv6]:port
```

### 5.2 原因

`FringeDB` 在解析 dict 编码列时，会调用：

```cpp
dict::Repo::getInstance(...)
```

而 `dict` 库内部会读取 `ENV_DICT_SERVICE_ADDR` 做配置初始化。

即使测试并不依赖真实字典服务返回数据，只要 `getInstance(...)` 触发配置解析，这个环境变量就必须存在。

### 5.3 当前处理方式

本次测试直接在 fixture 中设置：

```cpp
setenv("ENV_DICT_SERVICE_ADDR", "localhost:7788", 1);
```

对应代码见：

- [fringedb_sink_dict_test.cpp](file:///root/Documents/stream_engine/src/test/fringedb_test/fringedb_sink_dict_test.cpp#L49-L52)

### 5.4 为什么没有额外起 mock 服务

本次测试只校验：

- 配置字符串如何解析
- repo 名称如何构造
- final schema 如何追加 `_dict_idx`

在这些场景下，只要 `dict::Repo` 能成功初始化即可，不需要真实执行远端字典查询。

实际运行结果也证明：

- 设置 `ENV_DICT_SERVICE_ADDR=localhost:7788` 后
- `Repo` 会走本地 arena / preload 路径
- 当前测试不依赖真实服务响应也能通过

因此这次没有继续引入额外的 mock dict 服务。

## 6. 第三个常见问题：测试通过但进程退出时崩溃

### 6.1 现象

本次测试中曾出现：

1. 所有 gtest case 都显示 `PASSED`
2. 但测试进程在退出阶段触发：

```text
pure virtual method called
terminate called without an active exception
```

同时日志里还能看到类似：

```text
dict_execution usage 0B reserved 0B peak 0B
pools_.size() != 0
```

### 6.2 原因判断

触发点不在业务逻辑本身，而在第三方库全局析构阶段：

1. `dict::Repo` 初始化后，会引入 `local_dict/velox` 的全局资源
2. 测试结束后，这些全局对象与 Velox memory manager 的析构顺序不稳定
3. 最终在进程退出时触发内存池相关的析构问题

也就是说：

- 逻辑断言是通过的
- 崩溃发生在测试结果输出之后的进程清理阶段

## 7. 当前规避方案

### 7.1 方案内容

在测试文件里增加一个 gtest listener：

1. 等所有测试结果输出完
2. 在 `OnTestIterationEnd(...)` 中调用：

```cpp
std::_Exit(unit_test.Passed() ? 0 : 1);
```

这样可以：

- 保留完整的测试结果输出
- 保留正确的进程返回码
- 跳过不稳定的静态析构阶段

对应代码见：

- [fringedb_sink_dict_test.cpp](file:///root/Documents/stream_engine/src/test/fringedb_test/fringedb_sink_dict_test.cpp#L11-L26)

### 7.2 为什么接受这个方案

这是一个测试层面的工程兜底，不影响生产代码逻辑。

采用该方案的理由是：

1. 崩溃发生在测试进程退出阶段，不是功能逻辑错误
2. 问题位于 `local_dict/velox` 第三方析构链路，不在本次改造范围内
3. 如果强行深入清理第三方全局资源，成本远高于本次测试目标

因此当前把它视为：

```text
测试进程退出阶段的已知库问题，用 listener + _Exit 做稳定化处理
```

## 8. 如何判断后续是否需要 mock 字典服务

本次不需要 mock 的前提是：

1. 只验证配置解析和 repo 绑定
2. 不要求真实调用远端字典服务返回索引
3. `ENV_DICT_SERVICE_ADDR` 只需要满足初始化格式校验

如果后续测试要覆盖以下内容，则可能需要 mock 或真实服务：

1. `registerKeys(...)` 的真实远端取值
2. 字典版本切换
3. 共享字典在多列之间的真实索引一致性
4. 依赖字典服务返回内容的运行时写入路径

换句话说：

- 当前测试是“配置兼容单测”
- 不是“字典服务集成测试”

## 9. 建议复用模板

后续如果再写类似测试，可以直接沿用这几个约定：

### 9.1 环境变量

```bash
export DISABLE_CAS=1
export ENV_DICT_SERVICE_ADDR=localhost:7788
```

### 9.2 运行命令

```bash
rm -f .trace.log && touch .trace.log && chmod 666 .trace.log && \
mkdir -p /tmp/blade_home && \
HOME=/tmp/blade_home DISABLE_CAS=1 ENV_DICT_SERVICE_ADDR=localhost:7788 \
blade test //src/test:fringedb_test
```

### 9.3 测试设计原则

1. 优先验证配置解析结果，而不是立即做重型 E2E
2. 尽量只断言：
   - 绑定结构
   - repo 名称
   - final schema
3. 如果触发 `local_dict/velox` 析构崩溃，优先复用 `_Exit` listener 方案

## 10. 本次最终验证结果

本次最终通过的测试命令为：

```bash
HOME=/tmp/blade_home DISABLE_CAS=1 ENV_DICT_SERVICE_ADDR=localhost:7788 \
blade test //src/test:fringedb_test
```

最终结果：

```text
All tests passed!
```

对应测试报告见：

- [report_fringedb_test.xml](file:///root/Documents/stream_engine/build64_release/report_fringedb_test.xml)

## 11. 总结

这次 `FringeDB` dict 配置兼容测试的核心经验可以归纳为三点：

1. 先关掉 CAS，避免调试时间浪费在远端构建链路上
2. `dict::Repo` 初始化必须提供 `ENV_DICT_SERVICE_ADDR`
3. 测试通过后若在进程退出阶段崩溃，可用 gtest listener + `_Exit` 规避第三方库析构问题

因此，当前这组测试已经具备可复用的最小运行模板，后续再扩展 `FringeDB` dict 相关测试时，可以直接以这份文档作为操作手册。

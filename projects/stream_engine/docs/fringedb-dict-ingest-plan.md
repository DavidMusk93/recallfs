# FringeDB Dict 列入库流程与配置适配 Plan

## 1. 目标与背景

本文基于参考文档 `docs/dict2-shared-dict-bool-rewrite-plan.md`，梳理 `src/sink/fringedb.cpp` 中 dict 列的入库流程，并给出适配 `column.physical_column` 配置变更的改造方案。

参考文档的核心变化点是：

- 字典列配置从“只声明逻辑列名”升级为“逻辑列 + 物理列映射”
- 逻辑列之间是否共享字典，不再只靠列名判断，而是靠 `physical_column` 判断
- 共享字典的 identity 应该基于物理列，而不是逻辑列

而 `FringeDB` 当前的入库侧实现，仍然是：

- 通过 `fringedb.dict_encoding_columns` 读取“需要编码的逻辑列列表”
- 直接按 `tenant/table/column` 创建 `dict::Repo`
- 为每个逻辑列追加一个 `${column}_dict_idx` 的 `int32` 列

这意味着当前实现还没有“逻辑列 -> 物理列”的绑定层，也无法显式表达共享字典。

## 2. 当前实现概览

### 2.1 关键结构

`FringeDB` 内部把 dict 编码相关的运行时状态放在 `SingleFlight` 中，主要包括：

- `dictEncodingColumnIndices`
  - 原始输入表中需要做 dict 编码的列下标
- `dictRepos`
  - 与每个 dict 列一一对应的 `dict::Repo`
- `finalSchema`
  - 在原始 schema 基础上追加了 `_dict_idx` 列后的最终写入 schema

对应代码见：

- [fringedb.h](file:///root/Documents/stream_engine/src/sink/fringedb.h#L47-L77)

### 2.2 主流程入口

dict 列入库主要分成三个阶段：

1. `create_impl()`
   - 解析 sink 配置
   - 识别哪些列需要 dict 编码
   - 构建额外的 `_dict_idx` 字段并保存到 `SingleFlight`
2. `Init()`
   - 打开 FringeDB 实例
   - 这一步本身不做 dict 编码，但保证后续写入可用
3. `Run()`
   - 对输入表进行排序
   - 对每个 dict 列调用 `dictRepo->registerKeys(...)`
   - 将返回的编码列拼到表尾
   - 以 `finalSchema` 组装新表并写入 FringeDB

对应代码见：

- [create_impl](file:///root/Documents/stream_engine/src/sink/fringedb.cpp#L173-L737)
- [Init](file:///root/Documents/stream_engine/src/sink/fringedb.cpp#L741-L790)
- [Run](file:///root/Documents/stream_engine/src/sink/fringedb.cpp#L964-L1029)

## 3. Dict 列入库现状流程

### 3.1 配置读取阶段

`create_impl()` 会先解析通用的 FringeDB sink 配置，然后在尾部单独处理 dict 编码列配置：

```cpp
auto dictEncodingColumnsStr = GetOrDefault(options, "fringedb.dict_encoding_columns"s, ""s);
```

后续行为如下：

1. 如果配置为空，则不启用 dict 编码路径
2. 如果配置非空，则按 `, ; |` 三种分隔符拆分列名
3. 对每个列名做 `trim`
4. 只要列名非空，就进入 dict 编码初始化逻辑

对应代码见：

- [fringedb.cpp](file:///root/Documents/stream_engine/src/sink/fringedb.cpp#L713-L729)

当前配置语义只有一层：

```text
fringedb.dict_encoding_columns=local_node_name,remote_node_name
```

它只能表达“哪些逻辑列要做 dict 编码”，不能表达：

- 某个逻辑列对应哪个物理字典
- 多个逻辑列是否共享同一个字典

### 3.2 Schema 扩展阶段

一旦发现存在 dict 编码列，`create_impl()` 会先复制原始 schema，再为每个 dict 列追加一个新的 `int32` 字段：

```cpp
schemaBuilder.AddField(arrow::field(col + "_dict_idx", arrow::int32(), false));
```

这一阶段做了三件关键事情：

1. 记录原始列下标
   - `schema->GetFieldIndex(col)`
   - 保存到 `sf->dictEncodingColumnIndices`
2. 创建列级字典仓库
   - `dict::Repo::getInstance(dbopts.tenant + "/" + dbopts.table + "/" + col)`
   - 保存到 `sf->dictRepos`
3. 生成最终写入 schema
   - `dbopts.schema = schemaBuilder.Finish().ValueOrDie()`
   - 同时保存到 `sf->finalSchema`

这意味着当前 `dict::Repo` 的命名规则是：

```text
tenant/table/column
```

也就是说，字典 identity 绑定的是逻辑列名 `column`，不是物理列名。

对应代码见：

- [fringedb.cpp](file:///root/Documents/stream_engine/src/sink/fringedb.cpp#L715-L730)

### 3.3 DB 初始化阶段

`Init()` 的职责主要是按 `tenant/table` 维度复用或创建 `fringedb::DB`：

1. 通过 `connection_key = tenant + "/" + table` 查找共享 DB
2. 如果还没有打开，则调用 `fringedb::DB::Open(options_)`
3. 将 DB 句柄存回 `single_flight_`

这一步与 dict 编码没有直接计算逻辑，但它依赖 `create_impl()` 已经把最终 schema 放入 `options_.schema`。因此后续 DB 写入时看到的是“原始列 + `_dict_idx` 扩展列”的 schema。

对应代码见：

- [Init](file:///root/Documents/stream_engine/src/sink/fringedb.cpp#L741-L790)

### 3.4 写入前编码阶段

`Run()` 是 dict 列真正发生编码的位置，主流程如下：

1. 先根据 sort keys 对输入表排序
2. 读取 `single_flight_->dictEncodingColumnIndices.size()`
3. 如果没有 dict 列，直接走普通写入
4. 如果存在 dict 列：
   - 取出当前表的所有列 `cols = table->columns()`
   - 为每个 dict 列执行 `dictRepo->registerKeys(table->column(colIdx))`
   - 将返回的编码结果作为新的 `ChunkedArray` 追加到 `cols`
5. 使用 `single_flight_->finalSchema` 和扩展后的 `cols` 构造新表
6. 调用 `db_->Write(std::move(table), queue_no)` 完成入库

其中最关键的一步是：

```cpp
auto regResult = dictRepo->registerKeys(table->column(colIdx));
```

可以把它理解成：

- 输入：原始字符串列或低基数字典列
- 输出：与原列逐行对齐的整数编码列

之后追加的新列顺序与 `create_impl()` 中追加 schema 的顺序保持一致，因此 `finalSchema` 与 `cols` 可以正确对齐。

对应代码见：

- [Run](file:///root/Documents/stream_engine/src/sink/fringedb.cpp#L996-L1018)

### 3.5 最终落库结果

当前实现下，FringeDB 最终写入的数据形态是：

- 原始业务列保留
- 每个在 `fringedb.dict_encoding_columns` 中声明的列，额外新增一个 `${column}_dict_idx`

例如：

```text
local_node_name
remote_node_name
local_node_name_dict_idx
remote_node_name_dict_idx
```

这个设计有两个明显特征：

1. `_dict_idx` 列名仍然按逻辑列命名
2. 字典 repo identity 也按逻辑列命名

第 1 点和参考文档并不冲突，但第 2 点会阻碍“共享物理字典”的表达。

## 4. 当前实现与参考方案的差异

参考文档希望把配置从：

```text
tide.sql.dict.columns=local_node_name,remote_node_name
```

演进到：

```text
tide.sql.dict.columns=local_node_name.__c1__,remote_node_name.__c1__
```

其本质是为每个逻辑列补充一个 `physical_column`。

而 `FringeDB` 当前实现与该目标的差异主要有四点：

### 4.1 配置模型差异

当前 `FringeDB` 只识别：

```text
fringedb.dict_encoding_columns=<column list>
```

没有能力解析：

```text
column.physical_column
```

### 4.2 Repo 命名差异

当前 repo 名称构造方式是：

```text
tenant/table/column
```

参考方案希望共享字典 identity 由物理列决定，因此更合适的命名应为：

```text
tenant/table/physical_column
```

### 4.3 元数据缺失

当前 `SingleFlight` 里只保存：

- 原始列下标
- repo 指针
- 最终 schema

缺少一层明确的“逻辑列到物理列”的绑定信息，因此文档语义和运行时代码无法一一对应。

### 4.4 兼容策略缺失

参考方案明确要求：

- 新格式支持 `column.physical_column`
- 旧格式 `column` 等价于 `column.column`

当前 `FringeDB` 还没有这层兼容逻辑。

## 5. 配置变更对 FringeDB 的影响判断

从 `FringeDB` 侧看，这次配置变更不会改变“是否追加 `_dict_idx` 列”的主流程，但会改变以下关键点：

1. 解析输入配置的方式
2. `dict::Repo` 的命名依据
3. 运行时需要保存的 dict 列元信息

不会改变的部分包括：

1. `_dict_idx` 列仍然建议按逻辑列命名
2. `registerKeys()` 仍然逐列执行
3. 最终仍然是“原始列 + 新增编码列”一起写入 FringeDB

因此更合理的改造方向是：

- 保持当前编码执行路径不变
- 只在配置解析、repo 选择和元信息保存层做增量改造

## 6. 适配方案 Plan

### 6.1 目标

让 `FringeDB` 在保持现有入库流程基本不变的前提下，支持参考文档中的共享字典配置语义：

- 支持 `column.physical_column`
- 兼容旧格式 `column`
- `dict::Repo` 按 `physical_column` 维度复用
- `_dict_idx` 列仍然按逻辑列命名

### 6.2 建议新增的数据结构

建议不要继续只用 `std::vector<int>` 和 `std::vector<std::shared_ptr<dict::Repo>>` 裸存信息，而是引入一层显式绑定结构，例如：

```cpp
struct DictEncodingBinding {
    std::string column;
    std::string physical_column;
    int column_index;
    std::shared_ptr<dict::Repo> repo;
};
```

然后把 `SingleFlight` 中的 dict 运行时信息调整为：

```cpp
std::vector<DictEncodingBinding> dict_bindings;
std::shared_ptr<arrow::Schema> finalSchema;
```

这样做的好处是：

1. 文档语义和运行时结构一致
2. 后续如果需要调试共享字典命中情况，信息更完整
3. `Run()` 中不再依赖多个并行数组按下标对齐

### 6.3 Phase 1: 配置解析改造

在 `create_impl()` 中新增对配置项的解析层。优先方案有两种：

1. 直接升级 `fringedb.dict_encoding_columns`
   - 允许值为 `column` 或 `column.physical_column`
2. 新增独立配置项，例如 `fringedb.dict_encoding_bindings`
   - 老配置继续保留一段时间

如果目标是和上游文档语义对齐、同时减少配置项分裂，建议采用第 1 种。

解析规则建议如下：

1. 输入项为 `column`
   - 视为 `column.column`
2. 输入项为 `column.physical_column`
   - 逻辑列为 `column`
   - 物理列为 `physical_column`
3. 空串或非法格式
   - 记录 warning
   - 不中断已有合法列的处理
4. 逻辑列不存在于 schema
   - 记录 error 或 warning
   - 跳过该列，避免产生 `GetFieldIndex()` 为 `-1` 的隐患

### 6.4 Phase 2: Repo 命名切换

将当前 repo 创建逻辑：

```text
tenant/table/column
```

改为：

```text
tenant/table/physical_column
```

即把：

```cpp
dict::Repo::getInstance(dbopts.tenant + "/" + dbopts.table + "/" + col)
```

改造成基于 `physical_column` 的版本。

这样能够保证：

1. `local_node_name.__c1__` 与 `remote_node_name.__c1__`
   - 命中同一个 repo
2. `local_node_name.__c1__` 与 `remote_node_name.__c2__`
   - 命中不同 repo

这与参考文档中“共享字典 identity 由物理列决定”的设计一致。

### 6.5 Phase 3: 保持编码列命名不变

虽然 repo identity 要切到 `physical_column`，但编码列名仍应保持为：

```text
${column}_dict_idx
```

原因是：

1. `_dict_idx` 列是逻辑列的编码表示
2. 两个逻辑列即使共享同一个物理字典，也仍然是两个独立字段
3. 这与参考文档里的命名原则一致

因此 `schemaBuilder.AddField(...)` 的逻辑可以继续保留，只需要确保它使用的是逻辑列名 `column`。

### 6.6 Phase 4: Run 阶段切换到绑定结构

`Run()` 中的编码逻辑建议改为遍历 `dict_bindings`：

1. 从 `binding.column_index` 取原始列
2. 调用 `binding.repo->registerKeys(...)`
3. 将结果列追加到 `cols`

这一步不改变核心算法，只是把“并行数组”改成“显式绑定对象”，从而让 `physical_column` 信息贯穿到运行时。

### 6.7 Phase 5: 回归与兼容验证

至少需要覆盖以下场景：

1. 旧格式配置
   - `local_node_name`
   - 等价于 `local_node_name.local_node_name`
2. 新格式单列配置
   - `local_node_name.__c1__`
3. 新格式共享字典配置
   - `local_node_name.__c1__,remote_node_name.__c1__`
4. 新格式非共享字典配置
   - `local_node_name.__c1__,remote_node_name.__c2__`
5. schema 中不存在的逻辑列
6. 含空白、空项、混合分隔符的配置文本

## 7. 推荐实施顺序

建议按下面顺序落地，便于控制风险：

### Step 1

先抽出配置解析函数，把字符串解析为绑定结构，不立即改动写入主流程。

目标：

- 先把“逻辑列 / 物理列”概念落地
- 保持原有行为可回归

### Step 2

再把 repo 命名从 `column` 切到 `physical_column`。

目标：

- 让共享字典语义真正进入运行时

### Step 3

最后把 `Run()` 中的并行数组逻辑替换为绑定结构遍历。

目标：

- 让实现更稳定，也便于未来继续扩展 dict 元信息

### Step 4

补充日志与测试，明确输出：

- 逻辑列名
- 物理列名
- repo 名称
- 列下标

这样后续排查“为什么两个列没有共享字典”会更直接。

## 8. 风险与注意事项

### 8.1 最大兼容风险

如果 repo 命名规则从 `tenant/table/column` 切换为 `tenant/table/physical_column`，需要确认现网 dict repo 的持久化和复用语义是否允许直接切换。若 repo 后端状态与名字强绑定，可能需要：

- 灰度开关
- 双读双写窗口
- 或者显式迁移策略

### 8.2 配置容错风险

当前代码直接把 `schema->GetFieldIndex(col)` 的返回值放进 `dictEncodingColumnIndices`，如果列名不存在，后续 `table->column(colIdx)` 可能存在越界或非法访问风险。适配新配置时应顺手把这类校验补上。

### 8.3 功能边界

这次改造只解决入库侧“如何把逻辑列映射到共享物理字典”的问题，不直接实现查询侧的 bool rewrite。查询侧是否能利用共享字典做：

```text
left_dict_idx = right_dict_idx
```

还需要上游 planner / rewrite 规则配合。

## 9. 总结

`src/sink/fringedb.cpp` 当前的 dict 列入库流程已经具备完整的“配置识别 -> schema 扩展 -> 逐列编码 -> 写入 FringeDB”闭环，但它仍然建立在“逻辑列名就是字典 identity”的前提上。

参考文档提出的配置变更，本质上是把字典 identity 从逻辑列提升到物理列。对 `FringeDB` 而言，最合适的改造方式不是重写整条入库链路，而是：

1. 给 dict 列增加显式绑定结构
2. 把 repo 命名依据从 `column` 切到 `physical_column`
3. 保持 `_dict_idx` 列名和现有写入流程不变

这样可以在兼容现有入库逻辑的基础上，为共享字典场景提供正确的运行时表达。

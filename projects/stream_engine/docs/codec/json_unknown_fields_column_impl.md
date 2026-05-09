# `json.unknown.fields.column` 实现说明

## 1. 当前实现范围

已实现：

- 新配置：`json.unknown.fields.column`
- 只收集 **顶层 unknown key**
- 输出列类型：`map<string,string>`
- value 保存 **JSON 片段**
- 与 `json.raw.field` 共存
- 与 `fields.mapping` / `json.field.path` 共存
- 与 `json.carry.field.name` 冲突时报错
- 打开 `json.ordering-fields.enabled=true` 时，若配置了该新 option，会**自动回退普通 JSON decoder**

当前不支持：

- `json.mode.unnest=true`
- 递归展开 nested unknown fields
- ordering-fields 快路径内直接处理 unknown fields

---

## 2. 实现要点

### 2.1 编译期/初始化阶段

- 注册新 option： [format_options.h](file:///root/Documents/stream_engine/src/sql/encdec/options/format_options.h)
- SQL source schema 自动补 `MAP` 列： [engine.cpp](file:///root/Documents/stream_engine/src/sql/engine/engine.cpp)
- `JSONStructuredDecoder::Init()` 中完成：
  - 与 `json.carry.field.name` 的冲突检查
  - `json.mode.unnest` 禁用检查
  - 目标列存在性检查
  - 目标列类型必须是 `map<string,string>` 的检查
  - `known top-level keys` 预计算

### 2.2 运行期

- `HandleParseObj()` 遍历顶层 JSON key 时：
  - 如果 key 属于已知业务字段，走原有 mapping / writer 流程
  - 如果 key 不属于已知业务字段，且配置了 `json.unknown.fields.column`：
    - 直接写入 unknown map 列
    - 不走旧 `carry` 分支

### 2.3 writer

- 新增独立 writer：`MapUnknownFieldsWriter`
- 不复用 `MapCarryWriter`
- value 生成规则：
  - `1` -> `1`
  - `"x"` -> `"x"`
  - `true` -> `true`
  - `{"k":1}` -> `{"k":1}`
  - `[1,2]` -> `[1,2]`

---

## 3. 测试

已通过的复杂用例：

1. `JSONStructuredDecoderUnknownFieldsColumnBasicTest`
   - 同时覆盖标量/object/array unknown value
2. `JSONStructuredDecoderUnknownFieldsColumnWithMappingAndRawTest`
   - 覆盖与 `json.field.path`、`json.raw.field` 共存
3. `JSONStructuredDecoderUnknownFieldsColumnRejectCarryConflictTest`
   - 覆盖与 `json.carry.field.name` 冲突
4. `OrderingFieldsDecoderUnknownFieldsColumnFallbackToNormalDecoderTest`
   - 覆盖 ordering-fields 开启时自动回退普通 decoder

测试文件：

- [json_decode_test.cpp](file:///root/Documents/stream_engine/src/test/plan/json_decode_test.cpp)

验证命令：

```bash
blade test //src/test:json_decode_test -- --gtest_filter=JsonDecodeTest.JSONStructuredDecoderUnknownFieldsColumnBasicTest:JsonDecodeTest.JSONStructuredDecoderUnknownFieldsColumnWithMappingAndRawTest:JsonDecodeTest.JSONStructuredDecoderUnknownFieldsColumnRejectCarryConflictTest:JsonDecodeTest.OrderingFieldsDecoderUnknownFieldsColumnFallbackToNormalDecoderTest
```


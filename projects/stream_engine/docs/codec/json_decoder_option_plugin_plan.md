# JSON Decode Parse Plugin Plan

## Scope

Apply the plugin abstraction only to:

- `json.raw.field`
- `json.unknown.fields.column`

Do not merge them into one aggregate plugin. Do not migrate other options yet.

## Design

- keep `JSONStructuredDecoder` as the owner of parse flow and row lifecycle
- bind plugins to specific hook points in the existing parse path
- prefer hook-bound direct concrete members over a generic plugin abstraction in hot path
- reuse the already parsed object/view; do not parse raw input again inside plugins
- expose a parser-neutral row view so both simdjson and the secondary parser can use the same plugin path
- for this tiny plugin set, use direct hook members, not generic hook arrays

## Review

Strictly speaking, this is **not** a zero-cost abstraction in the C++ textbook sense, because:

- enabled hooks still add branch checks in the parse path
- enabled features still add real write/format work
- row context adaptation still has a small fixed construction cost

Verdict:

    - **not** strict zero-cost abstraction
    - **yes** to zero extra parse work
    - **yes** to zero extra traversal
    - **no** to fully zero runtime overhead in the enabled path

## Constraints

### 1. No support for unnest or ordering mode
When json.raw.field or json.unknown.fields.column is enabled:
- We do **not** support `json.mode.unnest`
- We do **not** support ordering fields fast path
- The decoder will fall back to normal path
- Why?
  - Unnest mode operates on nested arrays and changes row-level logic significantly
  - Ordering fields fast path is a specialized optimized path that expects known fields only
  - Adding plugin support would complicate both paths without clear benefits

### 2. No plugin support for rapidjson fallback path
When the decoder falls back to rapidjson (because simdjson encountered invalid JSON):
- We do **not** run any plugin logic
- Why?
  - The fallback path is for invalid JSON inputs only
  - Invalid UTF8 makes the input itself invalid
  - Adding plugin support here would increase complexity for an edge case

So the real target should be:

- zero extra parse work
- zero extra traversal
- near-zero disabled-path overhead
- explicit and bounded enabled-path overhead

In other words:

- no hook-bound plugin enabled -> almost the same decode path as today
- plugin enabled -> pay only for the hooks and writes that are actually used
- no plugin is allowed to trigger a second parse

## Zero-Cost Options Matrix

| Option | Core idea | Runtime overhead | Strict zero-cost? | Impl complexity | Extensibility | Readability | Fit for current scope | Main issue |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `virtual + final` | Use `JsonDecoderPlugin` as abstract base and bind `JsonRawFieldPlugin final` / `JsonUnknownFieldsColumnPlugin final` to hooks | Has null-checks and virtual calls; no second parse; no second traversal | No | Low | High | High | Good for first implementation | Enabled path still has dynamic dispatch cost |
| hook-bound direct concrete members | Keep concrete hook owners like `rawFieldPlugin_` / `unknownFieldsPlugin_` and call them through fixed hook members | Mostly null-checks and direct concrete calls; no second parse; no second traversal | Close, but not absolute | Medium | Medium | High | Best fit for current tiny plugin set | More specialized; becomes less clean if plugin count grows |
| compile-time policy / template strategy | Expand hook behavior at compile time with template policy objects or compile-time composition | Theoretically minimal and closest to zero extra runtime cost | Closest | High | Low | Medium to Low | Not suitable for current stage | Template complexity, code bloat, slower compile, harder debug |

| Dimension | `virtual + final` | direct concrete hook binding | compile-time policy |
| --- | --- | --- | --- |
| zero extra parse work | Yes | Yes | Yes |
| zero extra traversal | Yes | Yes | Yes |
| enabled-path zero runtime overhead | No | Almost | Closest |
| disabled-path overhead | Very low | Very low | Lowest |
| fit for `json.raw.field` + `json.unknown.fields.column` | Good | Best | Over-designed |
| fit as long-term general framework | Good | Fair | Fair |

| Recommendation | Conclusion |
| --- | --- |
| best current choice | **hook-bound direct concrete members** |
| why | Tiny plugin set, fixed hook points, lower overhead, simpler than template strategy |
| keep `virtual + final`? | Can be kept as a more generic fallback design, but not the best current fit |
| use compile-time policy now? | Not recommended; engineering complexity is higher than current value |

```text
Decision Summary

For current scope:
  best choice -> hook-bound direct concrete members

Reason:
  - zero extra parse work
  - zero extra traversal
  - no generic plugin broadcast
  - lower runtime overhead than virtual fan-out
  - much simpler than template/policy design

Conclusion:
  virtual + final
    -> good general design
    -> not zero-cost

  direct concrete hook binding
    -> best fit for current tiny fixed plugin set

  compile-time policy
    -> closest to zero-cost
    -> too heavy for current scope
```

## Relation Graph

```text
Object Ownership

JSONStructuredDecoder
├─ pluginContext_ : JsonDecoderPluginContext
├─ rawFieldPlugin_ : JsonRawFieldPlugin
├─ unknownFieldsPlugin_ : JsonUnknownFieldsColumnPlugin
├─ onObjectReadyUnknownFieldsHook_ : JsonUnknownFieldsColumnPlugin*
├─ onUnknownFieldUnknownFieldsHook_ : JsonUnknownFieldsColumnPlugin*
└─ onBeforeRowFinishRawFieldHook_ : JsonRawFieldPlugin*

Hook Ownership

JsonUnknownFieldsColumnPlugin
├─ onObjectReadyUnknownFieldsHook_
└─ onUnknownFieldUnknownFieldsHook_

JsonRawFieldPlugin
└─ onBeforeRowFinishRawFieldHook_

Runtime View

JsonDecoderRowContext
├─ row
├─ domObject : simdjson::dom::object
└─ raw
```

## Detailed Plan

```text
Phase 1: carve out raw-field
  decoder private state
    rawFieldName_
    rawFieldIndex_
  ->
  JsonRawFieldPlugin

Phase 2: carve out unknown-fields
  decoder private state
    unknownFieldsColumn_
    unknownFieldsColumnIndex_
    unknown-fields helper logic
  ->
  JsonUnknownFieldsColumnPlugin

Phase 3: bind direct hooks
  decoder
    onBeforeRowFinishRawFieldHook_
    onObjectReadyUnknownFieldsHook_
    onUnknownFieldUnknownFieldsHook_

Phase 4: keep core decode flow unchanged
  parser selection
  mapped-field write path
  row finish
  error/drop-row behavior

Phase 5: optimize disabled path
  all hook pointers == nullptr
    -> stay on near-original path
```

## Hook Layout

```cpp
class JSONStructuredDecoder {
private:
    std::unique_ptr<JsonRawFieldPlugin> rawFieldPlugin_;
    std::unique_ptr<JsonUnknownFieldsColumnPlugin> unknownFieldsPlugin_;

    JsonUnknownFieldsColumnPlugin* onObjectReadyUnknownFieldsHook_{nullptr};
    JsonUnknownFieldsColumnPlugin* onUnknownFieldUnknownFieldsHook_{nullptr};
    JsonRawFieldPlugin* onBeforeRowFinishRawFieldHook_{nullptr};
};
```

## Lifecycle

```text
init()
  |
  +-- create pluginContext_
  +-- create rawFieldPlugin_?            if json.raw.field enabled
  +-- create unknownFieldsPlugin_?       if json.unknown.fields.column enabled
  +-- validate plugins
  +-- prepare plugins
  `-- bind hooks
        onObjectReadyUnknownFieldsHook_      -> unknownFieldsPlugin_
        onUnknownFieldUnknownFieldsHook_     -> unknownFieldsPlugin_
        onBeforeRowFinishRawFieldHook_       -> rawFieldPlugin_

Decoder owns:
  parse success/failure
  builder finish
  row finish
  drop-row semantics

Plugin owns:
  observe
  plugin-local write
```

## Plugin Classes

```cpp
class JsonRawFieldPlugin final {
public:
    explicit JsonRawFieldPlugin(std::string columnName);

    tidecore::run::Status validate(const JsonDecoderPluginContext& ctx);
    tidecore::run::Status prepare(const JsonDecoderPluginContext& ctx);
    void onBeforeRowFinish(JsonDecoderRowContext& ctx);

private:
    std::string columnName_;
    int32_t columnIndex_{-1};
};

class JsonUnknownFieldsColumnPlugin final {
public:
    explicit JsonUnknownFieldsColumnPlugin(std::string columnName);

    tidecore::run::Status validate(const JsonDecoderPluginContext& ctx);
    tidecore::run::Status prepare(const JsonDecoderPluginContext& ctx);
    void onObjectReady(JsonDecoderRowContext& ctx);
    void onUnknownField(JsonDecoderRowContext& ctx,
                        std::string_view key,
                        simdjson::dom::element value);

private:
    std::string columnName_;
    int32_t columnIndex_{-1};
    boost::container::flat_set<std::string, StringViewLess> mappedTopLevelKeys_;
};
```

Responsibilities:

- `JsonRawFieldPlugin`
  - validate raw target column exists and is `STRING`
  - write raw input in `onBeforeRowFinish(...)`
- `JsonUnknownFieldsColumnPlugin`
  - validate target column exists and is `MAP<STRING,STRING>`
  - collect unknown top-level fields in `onUnknownField(...)`

## Context

```cpp
using KeyPath = std::vector<std::string>;
using ColumnBinding = std::pair<uint32_t, KeyPath>;
using TopLevelBindings = std::vector<ColumnBinding>;

struct JsonDecoderPluginContext {
    const std::shared_ptr<arrow::Schema>& schema;
    const std::unordered_map<std::string, std::string>& options;

    // indices
    //   [0] = "user"
    //   [1] = "device"
    const std::vector<std::string_view>& indices;

    // columnNames
    //   [0]  // for indices[0] == "user"
    //     [0] -> (schemaCol=2, keyPath=["user", "name"])
    //     [1] -> (schemaCol=3, keyPath=["user", "age"])
    //   [1]  // for indices[1] == "device"
    //     [0] -> (schemaCol=4, keyPath=["device", "os"])
    const std::vector<TopLevelBindings>& columnNames;

    // ordering-fields fast path requested by options.
    bool orderingFieldsEnabled{false};

    // unnest mode enabled in decoder config.
    bool unnestMode{false};
};

struct JsonDecoderRowContext {
    decoder::NamederRow& row;
    simdjson::dom::object domObject;  // parsed JSON object
    std::string_view raw;
    // plugin write target, set by the plugin that owns the current hook.
    int32_t pluginColumnIndex{-1};
};
```

## Hook Context

```text
Hook -> Context -> Purpose

onObjectReadyUnknownFieldsHook_
  input:
    rowCtx.domObject
    rowCtx.raw
  timing:
    once per parsed object
    before field loop
  owner:
    JsonUnknownFieldsColumnPlugin
  purpose:
    per-object setup

onUnknownFieldUnknownFieldsHook_
  input:
    rowCtx
    key : std::string_view
    value : simdjson::dom::element
  timing:
    only in unknown-field branch
    during field loop
  owner:
    JsonUnknownFieldsColumnPlugin
  purpose:
    collect unknown top-level fields

onBeforeRowFinishRawFieldHook_
  input:
    rowCtx.row
    rowCtx.raw
  timing:
    after field loop
    before row finish
  owner:
    JsonRawFieldPlugin
  purpose:
    write raw payload column
```

## Parse Path Integration

```text
parse one message
  |
  +-- parse with simdjson -> simdjson::dom::object
  |
  +-- build JsonDecoderRowContext
  |
  +-- if onObjectReadyUnknownFieldsHook_ != nullptr
  |     `-- onObjectReadyUnknownFieldsHook_->onObjectReady(rowCtx)
  |
  +-- foreach field in domObject
  |     |
  |     +-- mapped field?
  |     |     `-- normal decoder mapping path
  |     |
  |     `-- unknown field?
  |           `-- if onUnknownFieldUnknownFieldsHook_ != nullptr
  |                 `-- onUnknownFieldUnknownFieldsHook_->onUnknownField(rowCtx, key, value)
  |
  `-- before row finish
        `-- if onBeforeRowFinishRawFieldHook_ != nullptr
              `-- onBeforeRowFinishRawFieldHook_->onBeforeRowFinish(rowCtx)
```

This keeps one parse and one traversal. Raw JSON can be retrieved from simdjson::dom::element as std::string_view.

## Switch Categories

```text
Switch Map

1) init-time option switch

json.raw.field enabled?
  yes -> create rawFieldPlugin_
  no  -> rawFieldPlugin_ = nullptr

json.unknown.fields.column enabled?
  yes -> create unknownFieldsPlugin_
  no  -> unknownFieldsPlugin_ = nullptr


2) parser backend switch

parse input
  |
  +-- simdjson path
  |     `-- use as simdjson::dom::object
  |         (plugins run here)
  |
  `-- rapidjson fallback path (for invalid JSON)
        `-- NO PLUGINS run here (per constraints)


3) field-level switch

foreach field in domObject
  |
  +-- mapped?
  |     `-- normal decoder mapping path
  |
  `-- unknown?
        `-- onUnknownFieldUnknownFieldsHook_


4) hook-binding switch

rawFieldPlugin_
  -> onBeforeRowFinishRawFieldHook_

unknownFieldsPlugin_
  -> onObjectReadyUnknownFieldsHook_
  -> onUnknownFieldUnknownFieldsHook_
```

## Cost Categories

```text
Cost Map

A) disabled-path cost

all hook members == nullptr
  |
  +-- extra parse?        no
  +-- extra traversal?    no
  `-- extra runtime?      one cheap null-check branch


B) init-time cost

init()
  |
  +-- create enabled plugins
  +-- validate options
  +-- bind column indices
  `-- bind hook members

paid:
  once per decoder instance


C) per-row fixed cost

per parsed object
  |
  +-- build JsonDecoderRowContext
  +-- if onObjectReadyUnknownFieldsHook_ != nullptr
  |     `-- one direct concrete call
  `-- if onBeforeRowFinishRawFieldHook_ != nullptr
        `-- one direct concrete call

scales with:
  enabled hook points

does not scale with:
  message size
  nested depth


D) per-unknown-field cost

per unknown top-level field
  |
  `-- if onUnknownFieldUnknownFieldsHook_ != nullptr
        +-- one direct concrete call
        `-- plugin write/formatting work

scales with:
  number of unknown top-level fields

does not scale with:
  total document size beyond existing traversal


E) forbidden cost

forbidden by design:
  - second parse of raw JSON for plugin logic
  - second full traversal only for plugin logic
  - parser-backend-specific branches inside every plugin
  - aggregate mega-plugin paying for unrelated options
  - generic hook arrays for this tiny fixed plugin set
```

## Compatibility

- `JsonRawFieldPlugin::validate()`
  - target column exists
  - target column type is `STRING`
  - `json.mode.unnest=true` is not enabled
- `JsonUnknownFieldsColumnPlugin::validate()`
  - target column exists
  - target column type is `MAP<STRING,STRING>`
  - `json.carry.field.name` is not enabled
  - `json.mode.unnest=true` is not enabled
- if either option is enabled, ordering fields fast path is disabled
- when using rapidjson fallback path (for invalid JSON), no plugin logic is executed

## Files

```text
src/sql/encdec/json/
  decoder.h
  decoder.cpp
  plugin/
    json_decoder_plugin_context.h
    json_decoder_row_context.h
    json_raw_field_plugin.h
    json_raw_field_plugin.cpp
    json_unknown_fields_column_plugin.h
    json_unknown_fields_column_plugin.cpp
```

(no extra wrapper classes needed; use simdjson types directly)

## Migration

1. add `JsonDecoderPluginContext`, `JsonDecoderRowContext`
2. add concrete plugin owners plus direct hook-bound members
3. move `json.raw.field` validate/prepare/write into `JsonRawFieldPlugin`
4. move `json.unknown.fields.column` validate/prepare/unknown-field handling into `JsonUnknownFieldsColumnPlugin`
5. bind `unknownFieldsPlugin_` to `onObjectReadyUnknownFieldsHook_` and `onUnknownFieldUnknownFieldsHook_`
6. bind `rawFieldPlugin_` to `onBeforeRowFinishRawFieldHook_`
7. remove decoder-private raw-field and unknown-fields state that becomes plugin-owned
8. keep the rest of the decoder unchanged for now

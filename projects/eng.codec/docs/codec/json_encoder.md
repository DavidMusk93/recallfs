# JSON Encoder

## Scope

This note describes the JSON encoder implemented under `src/sql/encdec/json/`.
The goal is to capture:

- how the encoder is selected and initialized
- how it walks Arrow tables
- what Arrow types it can serialize today
- important limitations and behavior differences between modes

The encoder returns an `arrow::BinaryArray` where each element is one encoded JSON object.
Common sinks then write those values line by line, so the effective output format is usually NDJSON / JSON Lines.

## Entry Points

- Factory selection: `src/sql/encdec/factory/encoder_factory.h`
- JSON encoder class: `src/sql/encdec/json/encoder.h`
- JSON encode implementation: `src/sql/encdec/json/encoder.cpp`
- Shared Arrow table visitor: `src/util/arrowx/visitor.h` and `src/util/arrowx/visitor.cpp`
- JSON formatter backend: `src/util/arrowx/formats/simdjson_format.h`

## Relationship Graph

```text
Caller / Sink / SQL runtime
        |
        v
EncoderFactory::Create(...)
        |
        |  format.type = "json"
        v
NewJSONEncoder(options, schema)
        |
        +------------------------------+
        |                              |
        | json.encoder.mode=column     | json.encoder.mode=row
        v                              v
JSONStructuredEncoder<true>      JSONStructuredEncoder<false>
        |                              |
        +---------------+--------------+
                        |
                        v
                  Init()
                        |
                        | resolve schema-driven flags
                        | - json.unescape.fields
                        | - ignore.json.fields
                        | - json.unfold.carry.field.name
                        v
          arrowx::TableVisitor<SIMDJSONFormat>
                        |
                        +-------------------------------+
                        |                               |
                        | column traversal              | row traversal
                        v                               v
                 VisitColumnGroup(...)            VisitRowGroup(...)
                        |                               |
                        +---------------+---------------+
                                        |
                                        v
                              SIMDJSONFormat
                                        |
                                        v
                         one JSON object per input row
                                        |
                                        v
                           arrow::BinaryArray result
                                        |
                                        v
                 downstream sink may append '\n' per row
```

Relationship notes:

- `EncoderFactory::Create(...)` is the normal selection entry when `format.type=json`.
- `NewJSONEncoder(...)` converts option strings into a concrete encoder instance.
- `JSONStructuredEncoder` itself is thin; most serialization logic lives in `TableVisitor`.
- `SIMDJSONFormat` is the low-level JSON writer used by the visitor.
- wrappers such as the filesystem JSON output stream turn the array of JSON row strings into JSON Lines output.

## High-Level Flow

1. Caller sets `format.type = json`.
2. `EncoderFactory::Create()` calls `NewJSONEncoder(...)`.
3. `NewJSONEncoder(...)` reads JSON-specific options and chooses one of two implementations:
   - `JSONStructuredEncoder<true>`: column mode
   - `JSONStructuredEncoder<false>`: row mode
4. `Init()` resolves schema field names into per-column flags:
   - unescape flags from `json.unescape.fields`
   - ignore flags from `ignore.json.fields`
   - special carry-field handling from `json.unfold.carry.field.name`
5. `Encode(...)` delegates to `arrowx::TableVisitor<SIMDJSONFormat>`.
6. The visitor produces one JSON object per input row and stores the final strings in an `arrow::BinaryArray`.

## Options

### `format.type`

Must be `json` for the factory to choose this encoder.

### `json.encoder.mode`

Controls traversal strategy.

- `column`: default
- `row`: alternate implementation

The serialized result is still one JSON object per row in both modes. The difference is how the table is traversed and which Arrow types are handled correctly.

### `json.unescape.fields`

Semicolon-separated schema field names, or `*`.

Meaning:

- for matching string-like values, the encoder writes the field value as raw JSON instead of JSON-quoted text
- in the same field context, nested object keys / map keys are also written with the raw-key path

Practical effect:

- use this only when the source text already contains valid JSON fragments
- if the raw content is plain text instead of valid JSON, output can become invalid JSON

Unknown field names are only logged as warnings and otherwise ignored.

### `ignore.json.fields`

Semicolon-separated schema field names to skip from output.

Unknown field names are only logged as warnings and otherwise ignored.

### `json.unfold.carry.field.name`

Special handling for one field name in column mode.

When the current output column name matches this option:

- the encoder does not write the outer field key
- map entries from that field are written directly into the root object

This is effectively a "merge this map into the current row object" behavior.

## Output Shape

For a normal table with columns `a`, `b`, `c`, each row becomes:

```json
{"a":..., "b":..., "c":...}
```

The encoder itself returns those row objects as binary values in an Arrow array. Some wrappers, such as `src/sink/filesystem/io/format/json_format_output_stream.h`, append `\n` after every encoded row and therefore emit JSON Lines.

## Mode Logic

## Column Mode

Column mode is the default and the more complete implementation.

Behavior:

- starts one formatter per output row
- walks table columns one by one
- writes the same column key into every row, then appends the per-row value
- is chunk-aware for `arrow::ChunkedArray`
- also supports grouped encoding through `VisitColumnGroup(...)`

Why it matters:

- wider type coverage than row mode
- cleaner handling for chunked Arrow data
- safest mode for follow-up work unless there is a specific reason to use row mode

## Row Mode

Row mode iterates row by row using `RowVisitor`.

Behavior:

- opens one JSON object for each row
- for each row, fetches the value from each column and writes it immediately
- is available through `json.encoder.mode = row`

Observations from the current code:

- type coverage is narrower than column mode
- raw/unescaped field handling is not refreshed per field inside `VisitRowGroup(...)`
- ignore-field handling in the row loop is fragile because the schema index is not advanced when a field is skipped

For future work, assume column mode is the production path and treat row mode as feature-incomplete.

## Supported Top-Level Arrow Types

Top-level means a table column directly visited by the encoder.

| Arrow type | Column mode | Row mode | Notes |
| --- | --- | --- | --- |
| `BOOL` | yes | yes | JSON boolean |
| `INT32` | yes | yes | JSON number |
| `INT64` | yes | yes | JSON number |
| `UINT32` | yes | yes | JSON number |
| `UINT64` | yes | yes | JSON number |
| `FLOAT` | yes | no | row mode switch has no `FLOAT` case |
| `DOUBLE` | yes | yes | JSON number |
| `STRING` | yes | yes | quoted string unless field is unescaped |
| `BINARY` | yes | no | serialized through string-writing path in column mode |
| `TIME32` | yes | yes | formatted to datetime string |
| `TIME64` | yes | yes | formatted to datetime string |
| `TIMESTAMP` | yes | yes | formatted to datetime string |
| `LIST` | yes | yes | recursively serialized |
| `MAP` | yes | yes | map keys must be strings |
| `STRUCT` | yes | yes | recursively serialized |
| `DICTIONARY` | yes | partial | see dictionary notes below |
| `NULL` | effectively no useful output | no explicit path | not a practical supported data column |

## Dictionary Support

### Column mode

Column mode handles top-level `DICTIONARY` by forwarding to the underlying dictionary value array through the Arrow visitor path. In practice, it can inherit support from the underlying visited type, including types such as:

- string
- binary
- bool
- int32 / int64 / uint32 / uint64
- float / double
- list / map / struct
- time32 / time64 / timestamp

Nulls are checked on both:

- the dictionary index array
- the dictionary value array

This area has dedicated tests in `src/test/dict/dict_array_test.cpp`, mainly around null behavior.

### Row mode

Row mode has a hard-coded subset for dictionary value types:

- `BOOL`
- `TIME32`
- `INT32`
- `INT64`
- `UINT32`
- `UINT64`
- `DOUBLE`
- `STRING`

Notably absent in row mode dictionary handling:

- `FLOAT`
- `BINARY`
- `TIME64`
- `TIMESTAMP`
- `LIST`
- `MAP`
- `STRUCT`

## Supported Nested Types

The nested support comes from helper functions in `src/util/arrowx/visitor.h`.

## List Element Types

Lists can contain these element types:

- `INT32`
- `INT64`
- `UINT32`
- `UINT64`
- `FLOAT`
- `DOUBLE`
- `STRING`
- `BOOL`
- `LIST`
- `MAP`
- `STRUCT`

Not handled in list elements:

- `BINARY`
- `TIME32`
- `TIME64`
- `TIMESTAMP`
- `DICTIONARY`
- decimal / fixed-size binary / large-string style variants

## Map Types

Map handling assumptions:

- keys are string-only
- values may be:
  - `STRING`
  - `INT64`
  - `INT32`
  - `UINT64`
  - `UINT32`
  - `FLOAT`
  - `DOUBLE`
  - `BOOL`
  - `LIST`
  - `MAP`

Map value types not currently handled:

- `STRUCT`
- `BINARY`
- time types
- `DICTIONARY`

When `json.unfold.carry.field.name` matches a map column in column mode, the key-value pairs are emitted directly into the parent row object instead of nesting under the original field name.

## Struct Field Types

Struct fields can contain:

- `INT32`
- `INT64`
- `UINT32`
- `UINT64`
- `FLOAT`
- `DOUBLE`
- `STRING`
- `BOOL`
- `LIST`
- `MAP`
- `STRUCT`

Additional detail:

- if a struct field is logically `STRING` but physically dictionary-encoded, the code has a special branch to decode the dictionary value

Struct field types not currently handled:

- `BINARY`
- time types
- generic dictionary-typed fields other than the string-specific branch above

## Serialization Details

### Scalars

- integer and floating-point values become JSON numbers
- booleans become `true` / `false`
- nulls become `null`

### Strings

Default behavior:

- strings are JSON-escaped and quoted

If the field is unescaped:

- the string is injected as raw JSON content
- this is intended for preformatted JSON fragments, not ordinary text

### Time Values

`TIME32`, `TIME64`, and `TIMESTAMP` are converted to strings through `timestamp_to_string(...)`, which uses local time formatting helpers. The result is emitted as a JSON string, not a numeric epoch.

### Binary Values

Only column mode has a top-level `BINARY` visitor path. It uses the same string-writing path as text values. There is no explicit base64 encoding layer in this module.

## Error / Warning Behavior

The encoder is tolerant in several places:

- unknown names in `json.unescape.fields` or `ignore.json.fields` only produce warnings
- unsupported nested types usually log a warning and skip that value path
- unsupported top-level types in the visitor log an error

If final `arrow::BinaryBuilder::AppendValues(...)` fails:

- the encoder increments error counters
- stores one sample error message
- returns `nullptr` for the table-wide encode path

## Known Gaps And Caveats

1. Column mode and row mode do not have the same type support.
2. Row mode lacks explicit support for top-level `FLOAT` and `BINARY`.
3. Row mode dictionary support is only partial.
4. Nested maps require string keys.
5. Nested support does not cover many Arrow extension / decimal / large-container variants.
6. `json.unescape.fields` is dangerous if used on plain text; it writes raw JSON fragments.
7. `json.unfold.carry.field.name` only makes sense for map-like data and is implemented in the column traversal path.
8. The filesystem JSON output wrapper forces `json.unescape.fields = *`, so its output behavior is more "raw JSON projection" than "always quote string columns".

## Suggested Mental Model

Use this encoder as:

- a row-oriented JSON object emitter built on Arrow tables
- with column mode as the real reference implementation
- with strong support for common primitive columns plus recursive `list` / `map` / `struct`
- and with several sharp edges around raw JSON insertion and row-mode parity

For any new task touching this module, start by deciding:

1. whether the change is column-mode only or must also work in row mode
2. whether the target Arrow type is top-level, nested-in-list, nested-in-map, or nested-in-struct
3. whether the expected output is escaped JSON text or raw JSON fragment insertion

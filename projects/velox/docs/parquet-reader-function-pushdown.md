# Parquet Reader Function Pushdown

This note explains how function-style filter pushdown works for the default Velox parquet read path, and what is required if you want to push down a UDF.

Scope:

- This document describes the normal Hive connector plus DWIO parquet reader path.
- It does not describe the separate experimental cuDF parquet connector under `velox/experimental/cudf`.

## End-to-End Flow

At a high level, parquet pushdown happens in three stages:

1. A `TableScan` carries pushed filters in `HiveTableHandle`:
   - `subfieldFilters`: already-lowered `common::Filter` objects.
   - `remainingFilter`: a typed expression that still needs normal row evaluation unless parts of it can be extracted.
2. `HiveDataSource` tries to mine extra pushdown out of `remainingFilter` using `extractFiltersFromRemainingFilter(...)`.
3. The resulting `ScanSpec` and `MetadataFilter` are passed into the parquet row reader, which uses them for:
   - subfield pruning,
   - row-group skipping,
   - page skipping.

Relevant code:

- `velox/exec/tests/utils/PlanBuilder.cpp`
- `velox/expression/ExprToSubfieldFilter.cpp`
- `velox/connectors/hive/HiveDataSource.cpp`
- `velox/connectors/hive/HiveConnectorUtil.cpp`
- `velox/dwio/common/MetadataFilter.cpp`
- `velox/dwio/parquet/reader/ParquetReader.cpp`

## What "Function Pushdown" Really Means Here

In this path, parquet does not push down an arbitrary expression tree.

Instead, pushdown works by converting a filter expression into a `common::Filter` on one subfield. That filter is then attached to `ScanSpec`, and the parquet reader uses file metadata and page metadata to skip data.

So the important distinction is:

- Pushdown format: `Subfield -> common::Filter`
- Not pushdown format: arbitrary row expression

If an expression cannot be rewritten into that format, it stays as `remainingFilter` and is evaluated after reading rows.

## Built-In Extraction Behavior

`ExprToSubfieldFilter` is the core translator.

The default parser recognizes leaf calls such as:

- `eq`
- `neq`
- `lt`
- `lte`
- `gt`
- `gte`
- `between`
- `in`
- `is_null`
- `not(...)` over a supported leaf call
- `or(...)` only when both sides refer to the same subfield and can be merged

Important limits from the implementation:

- The field side must resolve to a single subfield path.
- The other side must be constant, or foldable to a constant.
- If constant folding throws a user error, extraction fails and the expression stays as remaining filter.
- Unsupported function names simply return `nullptr` for pushdown.

Examples that do push down:

- `a = 42`
- `a = 21 * 2`
- `a.b >= 10`
- `a in (1, 2, 3)`
- `a is not null`

Examples that do not push down:

- `a = b + 1`
- `like(a, 'foo%')`
- arbitrary multi-column predicates such as `c1 > c0`

## How Remaining Filter Is Handled

`HiveDataSource` starts with the explicit `subfieldFilters` from the table handle, then calls `extractFiltersFromRemainingFilter(...)`.

That function:

- extracts supported leaf predicates into `filters_`,
- recursively walks `and`,
- recursively walks `not`,
- keeps unsupported fragments as a smaller `remainingFilter`,
- builds a `MetadataFilter` from whatever remains pushdown-relevant for metadata evaluation.

This means one expression can be split:

- pushdown part: used for row-group or page pruning,
- leftover part: evaluated after row data is read.

Example:

```sql
c0 >= 0 AND c1 > c0
```

Likely result:

- `c0 >= 0` becomes a pushed `common::Filter`
- `c1 > c0` remains a normal filter

## What the Parquet Reader Actually Uses

Once filters are attached to `ScanSpec`:

- `makeScanSpec(...)` wires subfield filters onto the right scan nodes.
- `MetadataFilter` attaches logical combinations of metadata filters to the same scan tree.
- `ParquetRowReader::Impl::filterRowGroups()` applies row-group pruning.
- `ReaderBase::applyPageIndexFiltering()` applies page-level pruning when page indexes are available.

In other words, parquet pushdown is metadata-driven and scan-spec-driven, not generic expression execution inside the reader.

## UDF Pushdown Requirements

If you want to push down a UDF in this path, the real requirement is:

- your UDF call must be recognized and lowered into a parquet-usable `common::Filter` on exactly one subfield.

More concretely, a pushdown-able UDF must satisfy all of these:

1. It is semantically reducible to a single-column filter.
   Examples:
   - `custom_eq(col, 42)`
   - `custom_between(col, 10, 20)`
   - `custom_is_null(col)`

   Non-examples:
   - `custom_cmp(col1, col2)`
   - `custom_hash(col) = 7`
   - any UDF whose result depends on multiple columns, row state, randomness, external state, or non-determinism

2. The column argument can be converted to a `Subfield`.
   The helper only accepts field access or dereference chains that form one subfield path.

3. The non-column arguments are compile-time constants, or can be constant-folded safely.
   If the parser has to evaluate `21 * 2`, that is fine.
   If evaluation throws, extraction fails.

4. The UDF can be represented by an existing `common::Filter` kind.
   In practice this usually means one of:
   - equality
   - range
   - `in`
   - `between`
   - null checks
   - sometimes OR/negation if they lower into supported filter combinations

5. The parquet path actually supports that filter kind.
   This matters because expression parsing support and parquet reader support are not exactly the same thing.
   For example, there is a test comment in `TpchQueryBuilder.cpp` noting that `neq` is unsupported as a table-scan subfield filter for parquet.

6. The pushdown is safe to apply using metadata semantics.
   A UDF that cannot be answered conservatively from min/max/null/page metadata should not be pushed down as native parquet pruning.

## `dict_execution` In This Repo

The function you asked about is registered as:

- SQL name: `low_card_dict_execution`
- implementation symbol: `udf_dict_execution`
- file: `velox/sdk/velox/functions/LocalDict.cc`

Its current signature is:

```text
(INTEGER dict_idx, VARCHAR dict_name const, INTEGER dict_version const, VARCHAR expr const) -> BOOLEAN
```

Important properties of the current implementation:

- It is a `VectorFunction`, not a simple scalar expression alias.
- It is registered with `deterministic = false`.
- The predicate semantics are hidden inside the last `VARCHAR` constant argument.
- The parquet pushdown path does not understand opaque string predicates.

That means:

- You should not try to push down `low_card_dict_execution(...)` directly as-is.
- You need to lower it into a normal typed predicate, or into a `Subfield -> common::Filter` shape, before parquet pushdown can happen.

## Why A Cheap Bitmap Test Still Does Not Push Down Directly

It is important to separate these two ideas:

- cheap row-level evaluation,
- metadata-driven parquet pushdown.

`low_card_dict_execution(...)` may be very cheap if it is only testing whether one `dict_idx` belongs to a bitmap. But the current parquet pushdown path still cannot use it directly.

The reason is not CPU cost. The reason is representation.

Parquet pushdown in this repo wants something like:

```text
Subfield("col") -> common::Filter
```

The current `dict_execution` call shape is:

```text
low_card_dict_execution(dict_idx, dict_name, dict_version, expr_string) -> BOOLEAN
```

Those are different abstraction layers.

ASCII view:

```text
cheap boolean function                    parquet pushdown input
----------------------                    ----------------------
dict_execution(...)                       Subfield("c0") + Filter(...)
        |                                           |
        v                                           v
   row-level evaluation                    metadata pruning contract
```

So even if the bitmap test itself is light, parquet still cannot consume it unless it is first lowered into the pushdown contract.

### Why Metadata Filters Need More Than "Cheap"

The metadata-based filters in parquet work because they can answer a question without reading normal row data.

Typical examples:

- row-group min/max says the whole range cannot match,
- dictionary page says none of the encoded values in the page can match,
- page index says a whole page range can be skipped.

These mechanisms need a predicate that is visible to the reader in storage terms.

In other words, the reader must know:

- which physical parquet column or subfield the predicate applies to,
- what deterministic filter semantics are being applied,
- how to compare that filter to min/max or dictionary metadata.

An opaque boolean function call does not provide that information by itself.

### The Main Mismatch For `dict_execution`

`dict_execution` currently mixes several pieces of meaning together:

- data argument: `dict_idx`,
- dictionary identity: `dict_name`,
- dictionary snapshot: `dict_version`,
- predicate payload: `expr` string.

But the pushdown layer expects a much simpler object:

- one real parquet subfield,
- one deterministic `common::Filter`.

That is why the existing extraction path cannot use it directly.

### External Dictionary Versus Parquet Dictionary

There are also two different "dictionary" concepts here.

`dict_execution` is based on your own external or local dictionary semantics.

Parquet dictionary-page filtering is based on parquet's internal page dictionary metadata.

ASCII view:

```text
your dictionary logic                  parquet dictionary metadata
--------------------                  ---------------------------
dict_name + dict_version              values encoded in one page
business-level meaning                file-format-level meaning
```

These may correlate, but they are not automatically interchangeable.

So parquet cannot assume:

```text
dict_execution(bitmap on dict_idx)
==
native parquet dictionary-page predicate
```

unless you explicitly lower one into the other.

### When Bitmap-on-`dict_idx` Could Become Pushdown

If the semantics are really just:

```text
dict_idx in bitmap_set
```

then this can become pushdown-friendly.

But only after you rewrite it into a normal filter shape such as:

```text
Subfield("dict_idx") -> in(...)
```

or a logically equivalent range or discrete-value filter.

After that, the existing parquet path can try to use:

- min/max overlap checks,
- dictionary-page checks,
- page-index checks.

ASCII flow:

```text
dict_execution(bitmap test)
          |
          v
 lower to one-column deterministic filter
          |
          v
 Subfield("dict_idx") -> Filter(in/range)
          |
          +--> row-group min/max pruning
          |
          +--> dictionary-page pruning
          |
          v
      post-read fallback if needed
```

### What Is Missing Today

Today, direct pushdown is blocked by these issues:

- the function is a `BOOLEAN` `VectorFunction`, not already a pushed filter object,
- the pushdown extractor only understands `leafCallToSubfieldFilter(...)` returning `Subfield + Filter`,
- the predicate is hidden in a constant `VARCHAR expr`,
- the function metadata is currently marked non-deterministic,
- the mapping from `dict_idx` to the exact pushed parquet subfield is not encoded in the pushdown API.

So the current answer is:

- cheap enough to evaluate after read: yes,
- directly consumable by parquet metadata pushdown: no,
- convertible into pushdown after lowering: yes, potentially.

## `dict_execution` Pushdown Plan

Recommended direction:

- do not teach parquet reader about `dict_execution` itself,
- instead rewrite `low_card_dict_execution(...)` into a normal predicate before `ExprToSubfieldFilter` runs.

ASCII view:

```text
Current shape
-------------
low_card_dict_execution(dict_idx, dict_name, dict_version, "expr")
                |
                v
         opaque boolean UDF
                |
                x
      ExprToSubfieldFilter cannot extract
                |
                v
         stays in remainingFilter
                |
                v
          no parquet pruning


Target shape
------------
low_card_dict_execution(dict_idx, dict_name, dict_version, "a >= 10")
                |
                v
      planner / parser rewrite step
                |
                +--> actual typed expr: a >= 10
                |           |
                |           v
                |   ExprToSubfieldFilter
                |           |
                |           v
                |   Subfield("a") -> Filter(gte 10)
                |           |
                |           v
                +--> parquet row-group / page pruning
```

## Steps For `dict_execution`

If your goal is parquet filter pushdown for `low_card_dict_execution`, use these steps.

### Step 1: Freeze The Intended Semantics

Write down exactly what one call means.

For example, clarify whether:

- `dict_idx` is an encoded ID for one physical parquet column,
- `dict_name` and `dict_version` resolve to a dictionary snapshot,
- `expr` is a predicate over decoded dictionary values,
- null handling follows SQL null semantics,
- matching happens on decoded values, dictionary IDs, or both.

Without this, pushdown is unsafe.

### Step 2: Decide The Lowering Target

Choose one of these two targets.

Option A, recommended:

- rewrite `low_card_dict_execution(...)` into a normal typed predicate on the real column value.

Examples:

- `low_card_dict_execution(id_col, 't/c', 7, 'value = 42')`
  becomes `real_col = 42`
- `low_card_dict_execution(id_col, 't/c', 7, 'value in (1, 3, 9)')`
  becomes `real_col in (1, 3, 9)`

Option B, only if A is impossible:

- translate it directly into `Subfield + common::Filter` inside a custom `ExprToSubfieldFilterParser`.

Option A is better because it keeps `dict_execution` out of the storage layer.

### Step 3: Remove Opaque String Semantics

Today the last argument is `VARCHAR expr const`.

That is the main obstacle. The pushdown path works on typed expressions, not on strings.

So you need one rewrite layer that does one of these:

- parse the string into a typed expression over the real column,
- or convert the string into an already-lowered filter object,
- or replace `low_card_dict_execution(...)` in the plan with a native comparison call.

If you skip this step, parquet pushdown will not see anything useful.

### Step 4: Make It Deterministic If You Want Pushdown

Current metadata says:

```text
ExecutionFunction::metadata() -> {.deterministic = false}
```

For safe filter pushdown, the pushed semantics must be deterministic for the same input data and dictionary snapshot.

So either:

- make the lowered predicate deterministic,
- or keep `low_card_dict_execution` itself non-pushdown and only push down the deterministic lowered form.

Practical advice:

- keep the runtime vector function metadata as-is if needed,
- but make the rewritten predicate explicitly deterministic and independent of runtime side effects.

### Step 5: Bind The Function To One Real Column

Parquet pushdown only works for a single `Subfield`.

You must define how `dict_idx` maps to the real column path, for example:

```text
dict_idx column  ----decode metadata---->  real column subfield
```

The lowering step must end up with exactly one field path such as:

- `c0`
- `payload.user_id`

If the meaning depends on multiple columns, there is no standard parquet filter pushdown.

### Step 6: Limit Supported Predicate Shapes

Do not try to support all `expr` strings on day one.

Start with shapes that already map well to `common::Filter`:

- equality
- range
- `in`
- `between`
- `is null`
- `is not null`

Avoid initially:

- arbitrary UDFs inside `expr`
- regex or `like`
- multi-column comparisons
- non-deterministic logic
- expressions needing full row materialization

ASCII decision graph:

```text
                parsed dict expr
                      |
        +-------------+-------------+
        |                           |
   single column?                no
        |                           |
       yes                          v
        |                    keep as remainingFilter
        v
   constant bounds?
        |
   +----+----+
   |         |
  yes        no
   |         |
   v         v
 representable as     keep as remainingFilter
 common::Filter?
   |
 +--+--+
 |     |
yes    no
 |     |
 v     v
pushdown   keep as remainingFilter
```

### Step 7: Implement The Rewrite Or Parser Hook

You need one concrete integration point.

Path A, recommended:

1. Detect `low_card_dict_execution(...)` during planning or expression rewriting.
2. Resolve `dict_name` and `dict_version`.
3. Parse `expr`.
4. Rewrite the call into a normal typed predicate on the actual column.
5. Let existing `ExprToSubfieldFilter` logic do the rest.

Path B:

1. Subclass `ExprToSubfieldFilterParser`.
2. In `leafCallToSubfieldFilter(...)`, recognize `low_card_dict_execution`.
3. Decode or parse the constant `expr` argument.
4. Resolve the real target `Subfield`.
5. Return a `common::Filter`.

Use Path B only if the expression still arrives at pushdown time as a call node and you cannot rewrite it earlier.

### Step 8: Add Guardrails For Unsupported Cases

Your implementation must return "no pushdown" for cases like:

- bad or unparsable `expr`,
- dictionary lookup failure,
- non-deterministic dictionary source,
- expression over multiple decoded columns,
- unsupported operators,
- cases where you cannot conservatively derive metadata semantics.

Fallback behavior should be:

```text
return nullptr from pushdown extraction
-> keep expression in remainingFilter
-> evaluate after read
```

### Step 9: Add Focused Tests

You need tests at three layers.

Layer 1, expression lowering:

- `low_card_dict_execution(...)` rewrites to the expected typed predicate.

Layer 2, subfield filter extraction:

- the lowered predicate produces the expected `Subfield` and `common::Filter`.

Layer 3, parquet end-to-end:

- same results with pushdown enabled and disabled,
- row-group skipping happens when metadata allows it,
- unsupported shapes stay as `remainingFilter`.

### Step 10: Roll Out In Small Scope

Recommended rollout order:

1. support equality on one column,
2. support `in`,
3. support simple ranges,
4. add null checks,
5. only then consider more complex forms.

## Concrete Recommendation For `dict_execution`

For this repo, the best approach is:

1. Treat `low_card_dict_execution` as a rewrite source, not as a storage-native pushdown function.
2. Add a rewrite that converts the constant string predicate into a normal typed predicate over the decoded real column.
3. Reuse `ExprToSubfieldFilterParser` and existing `common::Filter` builders.
4. Leave unsupported or ambiguous cases in `remainingFilter`.

In short:

```text
Do not push down dict_execution directly.
Rewrite dict_execution into a normal single-column predicate first.
Then let the existing parquet pushdown path handle it.
```

## What You Need To Implement

For a real UDF pushdown, the normal extension point is `ExprToSubfieldFilterParser`.

You need to:

1. Implement a custom parser by subclassing `ExprToSubfieldFilterParser`.
2. Override `leafCallToSubfieldFilter(...)`.
3. Recognize your UDF name.
4. Use the helpers:
   - `toSubfield(...)`
   - `makeEqualFilter(...)`
   - `makeNotEqualFilter(...)`
   - `makeLessThanFilter(...)`
   - `makeLessThanOrEqualFilter(...)`
   - `makeGreaterThanFilter(...)`
   - `makeGreaterThanOrEqualFilter(...)`
   - `makeInFilter(...)`
   - `makeBetweenFilter(...)`
5. Register your parser factory with `ExprToSubfieldFilterParser::registerParserFactory(...)`.

The unit test `velox/expression/tests/ExprToSubfieldFilterTest.cpp` already contains a minimal example:

- `custom_eq(a, 42)` is lowered to the same filter form as `a = 42`
- unsupported custom calls stay unsupported

## Minimal Mental Model

Use this rule of thumb:

- If your UDF is just syntax sugar for `col OP constant`, it can probably be pushed down.
- If your UDF needs full row evaluation, it is a remaining filter, not parquet pushdown.

## Recommended Checklist For A New UDF

Before calling it "pushdown supported", verify all of the following:

- The parsed typed expression reaches your custom parser.
- The parser returns a `common::Filter`, not `nullptr`.
- The filter attaches to the correct `Subfield`.
- The same query against parquet shows correct results with and without pushdown.
- Row-group or page skipping actually improves, if that is your goal.
- Unsupported shapes fall back cleanly to `remainingFilter`.

## Practical Recommendation

If the UDF is logically equivalent to an existing comparison or range predicate, push down the lowered predicate, not the original UDF semantics.

That gives you:

- reuse of existing `common::Filter` behavior,
- compatibility with `ScanSpec`,
- compatibility with `MetadataFilter`,
- compatibility with parquet row-group and page pruning.

If the UDF cannot be lowered that way, keep it as `remainingFilter`.

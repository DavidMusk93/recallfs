# Dict2 execution rule design

This document summarizes the **new dict execution rule** implemented after commit `06ae330ceedd40213571bb7f5a249938385b47de`, centered on `starry-core`'s `dict2` package.

## 1. Scope and commit range

Relevant commits after the base commit:

1. `7ca5bf44` — initial `dict2` rule, native expressions, basic tests
2. `e43708cb` — adapt `DataSourceV2Relation` / SQL-style tests
3. `e5a92461` — preserve original column name inside dict filter JSON generation
4. `37e12ceb` — adapt `DataSourceV2ScanRelation`
5. `e12e4a08` — keep Project index-only only when required by Aggregate context
6. `0df68643` — decode before RPC sink / carry output-decode signal into physical plan
7. `e2547d0f` — lazy decode insertion and alias preservation
8. `ec4edaba` — stop forwarding `dict_idx` across mixed-expression boundaries
9. `a3560d62` — fix `ORDER BY` on dict columns / aliases

Main touched files:

- `starry/starry-core/src/main/scala/org/apache/spark/sql/execution/dict2/RewriteWithGlobalDict.scala`
- `starry/starry-core/src/main/scala/org/apache/spark/sql/execution/dict2/LowCardDictEncoding.scala`
- `starry/starry-core/src/main/scala/com/prx/starry/StarryPlugin.scala`
- `starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/extension/ColumnarTransformRule.scala`
- `starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/extension/plan/ColumnarTideSinkExec.scala`
- `starry/starry-core/src/test/scala/org/apache/spark/sql/execution/dict2/RewriteWithGlobalDictSuite.scala`
- `starry/starry-core/src/test/scala/org/apache/spark/sql/execution/dict2/RewriteWithGlobalDictSQLSuite.scala`

## 2. Problem the rule is solving

Some string columns are backed by a global dictionary. The rule tries to keep those columns as **integer dictionary indices** for as much of the query plan as possible, because index-based processing is cheaper than string-based processing.

However, not every operator can safely continue to use indices:

- `Filter` may be turned into a native dict predicate (`low_card_dict_execution`) if the predicate can be expressed against one dict column.
- `Project` usually forwards the encoded index, but becomes the preferred place to **decode and stop propagation** when a parent operator needs actual string semantics.
- `Sort` and some `Aggregate` expressions need string/value semantics and therefore require decode boundaries.
- final result delivery to RPC/client should decode back to user-visible string values.

So the rule is not “encode everything forever”; it is:

> **Keep dict columns encoded by default, decode only at semantic boundaries, and delay the final output decode to the physical sink boundary.**

## 3. High-level pipeline

### 3.1 Rule injection

`Starry.injectExtensions` registers `RewriteWithGlobalDict` as an optimizer rule:

- file: `com/prx/starry/StarryPlugin.scala`
- hook: `sparkSessionExtensions.injectOptimizerRule(_ => RewriteWithGlobalDict)`

This means dict2 acts on the **logical plan** before physical columnar planning.

### 3.2 Logical phase

`RewriteWithGlobalDict` runs in two passes:

1. **probe (top-down)**
   - decides where decode must happen
   - sets tags only
   - does not rewrite expressions or plan structure

2. **build (bottom-up)**
   - rewrites leaf outputs to dict indices
   - rewrites operator expressions using child dict mappings
   - inserts decode Projects only where required
   - propagates / rebases / drops dict mappings

### 3.3 Physical/output phase

Later physical code reads the logical tag `OUTPUT_DECODE_TAG` and ensures final output is decoded for RPC sink:

- `ColumnarTransformRule.createGlobalLimit` propagates output-decode intent to limit exec nodes
- `ColumnarTideSinkExec` checks for that tag and wraps dict-backed output fields in `LowCardDictDecode`

## 4. Core data model

### 4.1 `DictInfo`

`DictInfo(tenant, table, column)` identifies a global dictionary.

Derived fields:

- `dictName = s"$tenant/$table/$column"`
- `version = -1` currently hard-coded

### 4.2 Dict discovery

`CatalogTableDictProvider` discovers dict columns from table properties:

- `tenant` or `fringedb.tenant`
- `table` or `fringedb.table`
- `tide.sql.dict.columns`

Supported logical leaf types:

- `LogicalRelation`
- `DataSourceV2Relation`
- `DataSourceV2ScanRelation`

### 4.2.1 Metadata-aware config lookup

`dict2` config providers are now metadata-aware.

For both:

- `DictServiceConfigProvider`
- `DictAdaptivePushdownConfigProvider`

the source lookup order is:

1. bound table metadata collected from the current logical plan
2. `SQLConf`

Within each source, config key priority is:

1. hot key: `tide.sql.*`
2. cold key: `spark.sql.starry.*`

This is a classic **hot & cold config** pattern:

- **hot config**: `tide.sql.*`
  - intended for fast override close to table semantics
  - suitable for metadata / per-table / per-query adjustments
- **cold config**: `spark.sql.starry.*`
  - intended as the stable base config
  - suitable for global defaults and long-lived deployment settings

This makes dict behavior easy to control per table or per relation subtree while still keeping
existing global/base config compatible.

The supported config namespaces are:

- hot:
  - `tide.sql.dictLiteralIndex.*`
  - `tide.sql.dictAdaptivePushdown.*`
- cold:
  - `spark.sql.starry.dictLiteralIndex.*`
  - `spark.sql.starry.dictAdaptivePushdown.*`

If a table metadata map already contains these keys, that value takes precedence over
session/global config; and if both key families are present in the same source, `tide.sql.*`
wins over `spark.sql.starry.*`.

### 4.3 Encoded scan output

For each dict-backed string column, the leaf rewrite replaces output attribute:

- from: `domain: StringType`
- to: `domain_dict_idx: IntegerType`

and attaches metadata:

- `dictName`
- `dictVersion`

This metadata is later used by the sink-side final decode.

## 5. Tags and what they mean

The design depends heavily on tags rather than immediately mutating the whole plan.

### 5.1 `DICT_MAPPING_TAG`

Type:

- `Map[ExprId, (DictInfo, Attribute)]`

Meaning:

- original **value-column ExprId** -> `(dict info, currently in-scope encoded attribute)`

This is the main state threaded upward through the build phase.

### 5.2 `REQUIRED_DECODE_TAG`

Type:

- `Set[ExprId]`

Meaning:

- these original value columns must be decoded at this boundary, usually at a `Project`

### 5.3 `CHILD_DECODE_TAG`

Type:

- `Set[ExprId]`

Meaning:

- the operator expects its child subtree to stop forwarding indices for these columns
- currently used by `Sort` and `Aggregate`

### 5.4 `OUTPUT_DECODE_TAG`

Type:

- `Boolean`

Meaning:

- final output of this subtree must be decoded back to string values
- typically set on `GlobalLimit`, then carried into the physical path

## 6. Two-phase rewrite in detail

### 6.1 Probe phase

Implemented in `RewriteWithGlobalDict.probe`.

The probe phase computes dict dependencies without changing plan shape.

#### Sort probe

`Sort` is a value-semantics boundary:

- sorting by dict index is wrong
- lexical order of the string value must be respected

Important late fix from `a3560d62`:

- if the sort key is not the original dict attribute, but an alias produced by child `Project` or `Aggregate`, probe must walk back to the bound expression and find dict dependencies there
- this is why `dictExprIdsIn` checks both exact `exprId` matches and dict column names

Probe outputs:

- `CHILD_DECODE_TAG` on `Sort`
- `REQUIRED_DECODE_TAG` on `Sort.child`

#### Aggregate probe

`Aggregate` keeps index form only for safe cases:

- plain grouping key `group by dictCol` is safe to keep encoded
- compound grouping expressions or aggregate expressions involving dict columns are not

Examples needing decode:

- `group by upper(domain)`
- `group by split(domain, '.', 1)`
- aggregate expressions containing dict-based compound expressions

Probe outputs:

- `CHILD_DECODE_TAG` on `Aggregate`
- `REQUIRED_DECODE_TAG` on `Aggregate.child`

#### Project probe

`Project` is the preferred boundary where the rewrite chain stops.

If a project expression is compound and consumes dict columns, probe marks that project with `REQUIRED_DECODE_TAG`.

This became more important after `ec4edaba`:

- once mixed/compound expressions appear, the plan should stop forwarding raw `dict_idx` blindly upward
- instead, decode at the Project that owns the expression

#### Generic propagation

For non-Project nodes, probe propagates `REQUIRED_DECODE_TAG` downward until it reaches a Project that can materialize the decode.

### 6.2 Build phase

Implemented in `RewriteWithGlobalDict.build`.

Bottom-up traversal ensures child mappings are already available when rewriting a parent.

#### Leaf relations

Methods:

- `rewriteRelation`
- `rewriteV2Relation`
- `rewriteScanRelation`

Behavior:

- replace dict output column with `${name}_dict_idx: IntegerType`
- create mapping from original value exprId to encoded attribute
- store mapping in `DICT_MAPPING_TAG`

#### `rewriteOther`

Fallback behavior for operators without custom strategy:

- replace mapped attribute references with the encoded in-scope attribute
- propagate the same mapping upward unchanged

This means most operators will keep using encoded attributes unless a specific strategy breaks the chain.

## 7. Operator strategies

### 7.1 Project strategy

`ProjectRewriteStrategy` is the core operator.

Default behavior:

- replace dict value reference with encoded attribute
- preserve original output name and exprId by wrapping in `Alias`
- rebase mapping to the new Project output attribute

If decode is required:

- emit `LowCardDictDecode(encodedAttr, dictName, version)`
- do **not** carry that column’s mapping upward
- this is the “break the chain” behavior

Key invariant:

- Project preserves user-visible schema identity (`name`, `exprId`) even when internal representation changes

This was tightened by `e2547d0f`:

- decoding should be lazy / boundary-driven
- aliases should be preserved rather than destabilized by eager decode insertion

### 7.2 Filter strategy

`FilterRewriteStrategy` first rewrites mapped attributes to `LowCardDictDecode(...)`, then tries to fold eligible predicates into `LowCardDictExecution(...)`.

Conditions for conversion:

- predicate contains exactly one distinct `LowCardDictDecode`
- after replacing that decode with a fake unbound string attribute named as the original column, the predicate references only that column
- `ExpressionJsonConverter` can serialize it to native JSON

If successful:

- `decode(column_idx)` becomes `low_card_dict_execution(column_idx, dictName, version, json)`

If not:

- keep the predicate in decoded expression form

Why `e5a92461` matters:

- JSON conversion needs the original column name, not `_dict_idx`
- the code reconstructs `columnName` from `dictName.split("/").last`

Current supported shape:

- one-column conjunctive predicate pieces

Current not-optimized shape:

- predicates comparing two dict columns
- anything that fails JSON conversion

### 7.3 Aggregate strategy

`AggregateRewriteStrategy` uses `CHILD_DECODE_TAG` to see which dict values must already be materialized.

It then:

1. calls `Dict2RewriteUtils.buildDecodeAndBreakProject` on `agg.child`
2. rewrites grouping expressions using the remaining mapping
3. rewrites aggregate expressions using the remaining mapping
4. rebuilds output mapping from surviving encoded output expressions

Safe case:

- plain grouping key remains encoded

Unsafe case:

- any local compound expression depending on dict columns triggers child-side decode

Important nuance from later commits:

- aggregate outputs may be either direct `AttributeReference` or `Alias(childAttr, ...)`
- output mapping must be rebuilt from **encoded exprIds currently in scope**, not from original assumptions
- this is part of the `a3560d62` fix set

### 7.4 Sort strategy

`SortRewriteStrategy` is parallel to Aggregate:

1. read `CHILD_DECODE_TAG`
2. identify unsatisfied dict columns still in mapping
3. call `buildDecodeAndBreakProject` on child
4. rewrite order expressions using whatever encoded mapping still survives

Meaning:

- if sort depends on actual dict value semantics, a decode Project is inserted below Sort
- if the child has already broken the mapping, Sort simply uses the post-decode child output

Main tricky case fixed late:

- `ORDER BY alias` where alias came from dict-backed expression in child Project or Aggregate

### 7.5 GlobalLimit strategy

`GlobalLimitRewriteStrategy` does two things:

- rewrites references to encoded attributes if needed
- sets `OUTPUT_DECODE_TAG = true`

This is not the actual decode insertion point. It is only the logical marker that later physical planning / sink logic should decode final output.

## 8. Decode helper

`Dict2RewriteUtils.buildDecodeAndBreakProject(child, mapping, decodeExprIds)` is the central helper for Aggregate/Sort fallback.

Behavior:

- insert a new `Project(child.output.map(...), child)`
- for each targeted dict column, replace encoded attr with `Alias(LowCardDictDecode(attr, dictName, version), originalColumnName)(origExprId, qualifier)`
- drop those exprIds from mapping
- tag the new Project with reduced mapping

This helper guarantees two things:

1. local semantic boundary gets value form
2. parent nodes stop treating that column as still dict-encoded

## 9. Native expressions

Defined in `LowCardDictEncoding.scala`:

- `LowCardDictDecode`
- `LowCardDictExecution`

Both deliberately throw in JVM eval/codegen paths:

- they are markers intended for the native engine
- if they survive to unsupported local execution, that is a correctness problem

Semantics:

- `LowCardDictDecode(index, dictName, version)` => string value
- `LowCardDictExecution(index, dictName, version, jsonFilter)` => boolean filter result

## 10. Physical/output decode path

The logical rule does **not** eagerly decode final output everywhere.

### 10.1 `ColumnarTransformRule`

`createGlobalLimit` checks the logical plan tag `OUTPUT_DECODE_TAG` and copies it to the physical limit exec.

### 10.2 `ColumnarTideSinkExec`

For RPC sink:

- scan child plan for `OUTPUT_DECODE_TAG`
- if enabled and output attribute metadata contains `dictName`, wrap the native field attr in `LowCardDictDecode`
- convert to native JSON expressions and build the sink plan

This path was added by `0df68643`.

Design consequence:

- logical plan can stay encoded much longer
- final decode occurs only at client-facing sink boundary
- this reduces unnecessary decode / re-encode churn inside the plan

## 11. Main invariants

These are the most important invariants to preserve when changing the rule.

1. **Mapping is keyed by original value exprId**
   - not by current encoded attr exprId
   - but mapping values point to the current in-scope encoded attribute

2. **Projects preserve schema identity**
   - even when forwarding encoded attrs, outputs should keep original semantic name / exprId where needed

3. **Decode breaks mapping propagation**
   - once a column is decoded at a boundary, parent operators must stop treating it as encoded

4. **Plain grouping can stay encoded**
   - only compound/local-eval expressions require decode

5. **Sort requires value semantics**
   - especially `ORDER BY alias` / projected expression cases

6. **Final output decode is separate from logical correctness decode**
   - logical decode handles semantic boundaries
   - physical/sink decode handles user-visible output

7. **Native dict expressions must not fall back to local JVM execution**

## 12. Comparison with old `dict` rule

Old rule:

- lives under `org/apache/spark/sql/execution/dict`
- is more monolithic
- mixes rewrite and decode logic more tightly
- relies on older dict expression/tagging model

New `dict2` rule:

- is intentionally smaller and more explicit
- uses a **probe/build** split
- models decode boundaries directly with tags
- focuses on logical encoded forwarding + late output decode
- is easier to reason about for Sort/Aggregate boundary bugs

The biggest conceptual upgrade is:

> `dict2` separates “where do we need values for correctness?” from “where do we need values for final output?”

## 13. Tested behavior in current checkout

Focused suites run:

```bash
./mvnw -pl starry/starry-core test \
  -DfailIfNoTests=false \
  -DwildcardSuites=org.apache.spark.sql.execution.dict2.RewriteWithGlobalDictSuite,org.apache.spark.sql.execution.dict2.RewriteWithGlobalDictSQLSuite
```

Observed result in current workspace:

- 30 tests discovered
- 30 passed

Relevant scalar-expression coverage now includes:

- `RewriteWithGlobalDictSQLSuite`
- test: `Rewrite split(domain,'.')[0] to scalar form after dict decode`

Observed behavior:

- rewritten Project contains `low_card_dict_decode(...)`
- scalar extraction still appears as `split(...)[0]`
- the test validates decode placement plus preserved split-scalar semantics, without requiring unrelated `substring_index(...)` normalization

## 14. Likely debugging hotspots for future work

When investigating a dict2 bug, check these in order:

1. **Dict discovery**
   - are table properties present?
   - is relation type one of `LogicalRelation` / `DataSourceV2Relation` / `DataSourceV2ScanRelation`?

2. **Leaf rewrite**
   - did the scan output actually become `${col}_dict_idx`?
   - does metadata contain `dictName` / `dictVersion`?

3. **Probe tags**
   - does the right node carry `REQUIRED_DECODE_TAG`?
   - does `Sort` / `Aggregate` carry `CHILD_DECODE_TAG`?

4. **Project boundary**
   - did Project rebase mapping correctly?
   - when decode is required, was mapping dropped for that column?

5. **Aggregate output mapping**
   - after child decode, are surviving encoded outputs remapped from current in-scope attr exprIds?

6. **Sort alias case**
   - if ordering by alias, did probe trace the alias back to a dict-dependent child expression?

7. **Filter JSON conversion**
   - does the predicate reference only one dict column after unbinding?
   - does JSON conversion preserve original column name rather than `_dict_idx`?

8. **Final output decode**
   - did `GlobalLimit` set `OUTPUT_DECODE_TAG`?
   - did `ColumnarTransformRule` propagate it?
   - did `ColumnarTideSinkExec` inject decode for metadata-tagged outputs?

## 15. Mental model for reasoning about bugs

A good shortcut is to classify the issue first:

### A. Wrong scan/output schema
Look at leaf rewrite and mapping setup.

### B. Wrong filter pushdown / JSON predicate
Look at `FilterRewriteStrategy.optimizeCondition`.

### C. Wrong aggregate/group-by behavior
Ask whether the expression is:

- plain attribute -> keep encoded
- compound/local-eval -> decode before Aggregate

### D. Wrong order-by behavior
Ask whether sorting key is:

- direct dict attr
- alias from Project
- alias from Aggregate output

### E. Wrong client-visible output
Look at `OUTPUT_DECODE_TAG` and `ColumnarTideSinkExec`.

## 16. Open questions / likely extension points

1. `Join` strategy is still TODO.
2. `dictVersion` is currently fixed at `-1`; real version handling may be needed later.
3. The current `dictExprIdsIn` name-based fallback helps alias/sort handling, but also creates a place where name collisions should be watched carefully.
4. Filter optimization currently handles only shapes that can be isolated to a single decode dependency.
5. `split(domain,'.')[0]` is currently covered as a decode + scalar extraction case; if future work wants canonical `substring_index(...)` normalization, that should be treated as a separate expression-normalization change rather than a dict-mapping bug.

## 17. Short summary

`dict2` is a logical optimization that:

- rewrites dict-backed string columns to integer dict indices at scans
- carries a mapping from original value exprIds to current encoded attrs
- uses a top-down probe pass to decide where decode is required
- uses a bottom-up build pass to rewrite operators
- prefers decoding at Project boundaries to stop the encoded chain cleanly
- keeps plain grouping encoded, but decodes for compound Aggregate/Sort semantics
- delays final result decode to the RPC sink via `OUTPUT_DECODE_TAG`

If you remember only one thing, remember this:

> **The rule is a boundary-management system. Most bugs are really about “where should encoded form stop, and who is responsible for decoding?”**

# TideScanBuilder Study: Can We Push Down `low_card_dict_execution`?

## Updated Context

This note is based on the following stronger assumption:

1. `dict2` rewrite runs before scan filter pushdown
2. we are allowed to add missing classes / bridge logic

Under that assumption, the answer changes.

## Short Answer

Yes, **in principle we can make `TideScanBuilder` push down `low_card_dict_execution`**.

But the current repo is still missing the bridge.

So the real conclusion is:

```text
architecturally feasible,
currently not wired.
```

More precisely:

- `low_card_dict_execution` already exists as a Catalyst/native expression form
- `TideScanBuilder` already knows how to send scan predicates through `fringedb.filter.velox`
- what is missing is the transport layer that carries `LowCardDictExecution` from the optimized logical filter into the scan pushdown payload

## What `TideScanBuilder` Actually Pushes Down

`TideScanBuilder` implements `SupportsPushDownFilters` and receives an `Array[org.apache.spark.sql.sources.Filter]` in `pushFilters()`.

Inside `pushFilters()` it does four important things:

1. rewrites view aliases back to origin column names
2. converts supported datasource filters into Catalyst expressions with `filterToExpr()`
3. splits them into single-reference predicates with `FilterSplitter.splitToSingleReferenceFilters()`
4. serializes each predicate with `ExpressionConverter.convertToNativeJson(...)` and stores the result in `tideOptions("fringedb.filter.velox")`

That means the scan pushdown path is fundamentally:

```text
DataSource Filter
  -> simple Catalyst boolean expression
  -> native Velox JSON
  -> scan option: fringedb.filter.velox
```

This is the current scan-builder contract for plain scan predicates.

If we want `low_card_dict_execution` pushdown, we need to extend this contract or add a bridge before / around this contract.

## Where `low_card_dict_execution` Comes From

`low_card_dict_execution` is introduced by the dict2 optimizer rule, not by the scan builder.

Relevant flow:

1. `StarryPlugin` injects `RewriteWithGlobalDict` as an optimizer rule.
2. `RewriteWithGlobalDict` rewrites dict-backed attributes in logical operators such as `Filter`, `Project`, `Aggregate`, and `Sort`.
3. In `FilterRewriteStrategy`, the rule first replaces dict-backed attributes with `LowCardDictDecode(...)`.
4. `Dict2RewriteUtils.optimizeConjunctiveCondition(...)` then collapses eligible boolean predicates into `LowCardDictExecution(child, dictName, dictVersion, jsonFilter)`.

So the shape is:

```text
SQL predicate
  -> logical Filter condition
  -> LowCardDictDecode(...)
  -> LowCardDictExecution(...)
```

This happens in the Catalyst optimizer layer, after Spark has already modeled scan pushdown using datasource `Filter`s.

## Why It Is Not Working In Current Code

### 1. `pushFilters()` still only receives datasource `Filter`

`TideScanBuilder.pushFilters()` does not receive arbitrary Catalyst expressions. It only receives Spark datasource filters such as:

- `EqualTo`
- `GreaterThan`
- `LessThan`
- `In`
- `IsNull`
- `IsNotNull`
- `And`

There is no datasource-filter representation for:

```text
low_card_dict_execution(col_dict_idx, dictName, version, json)
```

So in the current code, even if the logical plan contains `LowCardDictExecution`, there is no visible bridge in this repo that turns that node into something `TideScanBuilder.pushFilters()` can consume.

### 2. The rewrite exists, but the handoff is missing

`RewriteWithGlobalDict` is injected through `SparkSessionExtensions.injectOptimizerRule(...)`.

This is no longer a reason to reject the idea outright, because we are explicitly assuming `dict2` runs before filter pushdown.

But it still tells us what work is required:

```text
LowCardDictExecution in optimized logical plan
  -> bridge / carrier
  -> TideScanBuilder pushdown payload
```

So this is not just "add one case in `TideScanBuilder`". The main missing piece is the bridge.

### 3. `filterToExpr()` only supports ordinary predicate operators today

`TideScanBuilder.filterToExpr()` explicitly supports a small set of filter operators and throws for unsupported ones.

It does not have a path to construct:

```scala
LowCardDictExecution(child, dictName, dictVersion, jsonFilter)
```

This means current gateway code cannot consume `LowCardDictExecution` directly, even if some earlier layer could expose it.

### 4. `fringedb.filter.velox` is built for scan predicates, but can likely be extended

The scan builder creates:

```scala
Map[String, Seq[String]]
```

where the key is the referenced column name and the value is a list of native JSON predicates for that column.

This works well for scan predicates like:

- `col_dict_idx = 123`
- `col_dict_idx in (...)`
- `ts >= ...`
- `ts < ...`

`low_card_dict_execution(...)` carries more information than ordinary scan predicates:

- an encoded child column
- dict identity (`dictName`)
- dict version
- a nested JSON predicate string for decoded values

Nothing in current gateway-catalyst code shows an existing producer for this richer shape.

But the payload format is already JSON-based and ultimately passed through `tideOptions`, so extending it is realistic if Tide/native scan side accepts the new contract.

### 5. The repo has partial pieces, not the full chain

Within the JVM codebase:

- `TideScanBuilder` is the only place that writes `fringedb.filter.velox`
- dict2 docs explicitly discuss `DictExecutionPushdown`
- `RewriteWithGlobalDict` already produces `LowCardDictExecution`
- `ExpressionConvertMapping` has explicit support for `LowCardDictDecode`, but no explicit converter registration for `LowCardDictExecution`
- gateway-catalyst has no code today that extracts `LowCardDictExecution` from logical filters

So the missing part is not the idea, but the end-to-end wiring.

## Feasible Design Directions

### Option A: Add A Custom Pushdown Carrier

This is the most direct interpretation of your requirement.

Add a custom filter-like carrier, for example:

```scala
case class DictExecutionPushdownFilter(
    encodedColumn: String,
    dictName: String,
    dictVersion: Int,
    jsonFilter: String) extends Filter
```

Then add a bridge rule that:

1. finds `LowCardDictExecution` conjuncts in optimized `Filter.condition`
2. converts them into `DictExecutionPushdownFilter`
3. feeds those carrier filters into `TideScanBuilder.pushFilters()`
4. leaves unsupported predicates as residual filters

Then extend `TideScanBuilder` so that:

1. it recognizes `DictExecutionPushdownFilter`
2. it does **not** route it through the ordinary `filterToExpr()` path
3. it serializes it directly into the `fringedb.filter.velox` payload

This is conceptually simple, but depends on whether the Spark pushdown path in your environment allows custom filter carriers to survive into `pushFilters()`.

### Option B: Add A Tag / Scan-Option Bridge

This may be cleaner in this repo.

The repo already uses logical-plan tags heavily in `RewriteWithGlobalDict`, and physical planning can still see logical-plan metadata through `LOGICAL_PLAN_TAG`.

So another design is:

1. after `dict2`, extract pushdown-eligible `LowCardDictExecution` predicates
2. attach them to the scan relation or logical scan node via `TreeNodeTag`
3. when the `TideScan` is built or wrapped, merge those extracted dict-execution predicates into `tideOptions`
4. let the native Tide scan consume them from the same JSON config path

This approach avoids overloading Spark's datasource `Filter` abstraction with something it was not originally designed to carry.

### Option C: Add Explicit Native Conversion For `LowCardDictExecution`

Even if Option A or B is chosen, it is still helpful to add explicit conversion support for `LowCardDictExecution` in the expression conversion registry instead of relying on generic fallback behavior.

That gives us:

1. clearer semantics
2. a single owned place for serialization rules
3. better tests
4. less risk that the generic function-call path serializes the JSON payload in an incompatible way

## Which Option Looks Best

For this repo, the most realistic order looks like:

1. **Primary recommendation:** Option B, tag / scan-option bridge
2. **If `pushFilters()` must be the only handoff:** Option A, custom carrier filter
3. **In either case:** add explicit conversion / serialization handling for `LowCardDictExecution`

Why Option B is attractive:

1. `RewriteWithGlobalDict` already uses tags extensively
2. the physical side already serializes `tideOptions` into the native Tide source config
3. it avoids depending too much on Spark's built-in filter abstraction
4. it is easier to keep ordinary filters and dict-execution payloads separate

## Option C Detailed Plan

The preferred interpretation of Option C is:

```text
explicit LowCardDictExecution serialization first,
thin pushdown bridge second,
residual rebuild third.
```

The goal is to make `LowCardDictExecution` the canonical pushdown form for complex single-column dict predicates, instead of trying to squeeze it through the ordinary datasource `Filter -> filterToExpr()` path.

### End-to-End Flow

```text
SQL
 |
 v
Spark logical plan
 |
 v
RewriteWithGlobalDict
 |
 +-----------------------------------------------+
 | direct simple dict equality                    |
 |   col = 'x'                                    |
 |   -> col_dict_idx = 123                        |
 +-----------------------------------------------+
 |
 +-----------------------------------------------+
 | complex single-column dict predicate           |
 |   like / in / bool subtree / shared dict case  |
 |   -> LowCardDictExecution(...)                 |
 +-----------------------------------------------+
 |
 v
Pushdown extraction bridge
 |
 +------------------------+----------------------+
 | ordinary native filter | dict execution filter|
 | EqualTo / In / Range   | LowCardDictExecution |
 +------------------------+----------------------+
 |                        |
 v                        v
fringedb.filter.velox
 |
 +------------+-----------+
              |
              v
TideScan / ColumnRelation / native Tide source
              |
              v
scan-side execution
              |
              v
residual Filter above scan only for predicates not selected for pushdown
```

### Phase 1: Add Explicit Converter

Add explicit conversion support for `LowCardDictExecution` in `starry-core`.

Suggested file:

```text
starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/expressions/convert/LowCardDictExecutionConverter.scala
```

Then register it in `ExpressionConvertMapping`.

Responsibilities of this converter:

1. own the exact serialization contract of `LowCardDictExecution`
2. serialize:
   - encoded child column
   - `dictName`
   - `dictVersion`
   - `jsonFilter`
3. avoid relying on generic fallback in `ExpressionConverter`

Why this matters:

1. the fourth argument is itself a JSON payload
2. explicit handling is easier to reason about and test
3. later scan-side pushdown can reuse the same serialization semantics

### Phase 2: Define Scan Payload Contract

Keep a single scan pushdown option:

1. `fringedb.filter.velox`

Do not introduce any additional pushdown config key, and do not change the engine-facing payload shape.

Suggested payload shape:

```json
{
  "ts": [
    "<serialized native predicate #1>"
  ],
  "account_id_dict_idx": [
    "<serialized direct equality predicate #2>"
  ],
  "domain_dict_idx": [
    "<serialized low_card_dict_execution #1>",
    "<serialized low_card_dict_execution #2>"
  ]
}
```

This keeps the current logical shape unchanged:

```text
Map[String, Seq[String]]
```

where:

1. key = pushed physical / encoded column name
2. value = serialized predicate list for that column

Why this shape is required:

1. engine-side parsing logic is fixed
2. `TideScanBuilder` already writes `fringedb.filter.velox` in this shape
3. `LowCardDictExecution` pushdown must fit into the existing contract instead of redefining it

Practical implication:

1. ordinary native predicates and serialized `LowCardDictExecution` become peer entries in the same per-column array
2. the distinction is carried by each serialized expression itself, not by a wrapper section in the payload

### Phase 3: Add Extraction Bridge

Create a helper that extracts pushdown-eligible `LowCardDictExecution` conjuncts from optimized `Filter.condition`.

Suggested helper name:

```text
DictExecutionPushdownExtractor
```

Input:

1. optimized filter condition
2. adaptive pushdown config

Output:

1. `selectedPushdownExprs: Seq[LowCardDictExecution]`
2. `residualCondition: Option[Expression]`

Rules:

1. only extract conjunctive predicates
2. only extract pushdown-eligible `LowCardDictExecution`
3. keep unsupported predicates in residual condition
4. keep ordering deterministic

### Phase 4: Config-Gated Pushdown Rule

Add the following pushdown condition:

```text
tide.sql.dictLiteralIndex.enabled=false
&& tide.sql.dictAdaptivePushdown.enabled=true
```

Under this condition:

```text
at most tide.sql.dictAdaptivePushdown.maxDictPushdownCount
LowCardDictExecution predicates may be pushed down.
```

These keys are metadata-aware in `starry-core` and can be controlled directly from bound table
metadata. Within the same source, `tide.sql.*` is the hot config and
`spark.sql.starry.*` remains the cold config.

This is the usual hot & cold config pattern:

- `tide.sql.*` is hot config
- `spark.sql.starry.*` is cold config

Interpretation:

1. when literal-index rewrite is disabled, direct `col_dict_idx = literal_index` pushdown is unavailable
2. if adaptive pushdown is enabled, `LowCardDictExecution` becomes the bounded fallback pushdown form
3. only the top-K selected dict predicates should enter scan pushdown

Recommended selection rule:

1. split the `AND` chain
2. collect candidate `LowCardDictExecution` conjuncts
3. score or preserve stable order according to existing adaptive pushdown policy
4. select at most `tide.sql.dictAdaptivePushdown.maxDictPushdownCount`
5. push selected candidates to scan
6. keep the remaining candidates as residual upper-layer predicates

This should be documented in the same place as adaptive pushdown behavior so the bounded fallback remains explicit and observable.

### Phase 5: Attach To Scan Planning

Attach selected dict-execution pushdown specs via `TreeNodeTag` or an equivalent scan-planning bridge step.

For this repo, tag-based transport still looks cleaner because:

1. `RewriteWithGlobalDict` already uses tags heavily
2. it keeps ordinary filter pushdown and dict-execution pushdown separate
3. it avoids trying to make datasource `Filter` carry more than it naturally wants to carry

Suggested transport DTO:

```scala
case class DictExecutionPushdownSpec(
    encodedColumn: String,
    dictName: String,
    dictVersion: Int,
    nativeJson: String)
```

This is preferable to carrying raw Catalyst trees all the way to the scan builder.

### Phase 6: Extend TideScanBuilder

Extend `TideScanBuilder` with a unified `fringedb.filter.velox` payload builder.

Current path:

1. ordinary datasource filters
2. convert with `filterToExpr()`
3. serialize to `fringedb.filter.velox`

New path:

1. read extracted dict-execution pushdown specs
2. group by encoded column
3. merge them into the same `fringedb.filter.velox` JSON payload
4. preserve the existing `Map[String, Seq[String]]` shape exactly

Important constraint:

```text
do not route LowCardDictExecution through filterToExpr()
```

because that path is for ordinary datasource filters and does not preserve the intended dict-execution semantics cleanly.

### Phase 7: Residual Rebuild

This is the key correctness step.

For an optimized condition like:

```text
A AND B AND C AND D
```

where:

1. `A` = timestamp range
2. `B` = direct encoded equality
3. `C` = `LowCardDictExecution(...)`
4. `D` = unsupported expensive residual expression

the final execution split should be:

```text
scan pushdown:
  A, B, C

upper residual filter:
  D
```

ASCII view:

```text
original Filter.condition
   =
   A AND B AND C AND D

split AND
   |
   +--> A -> ordinary scan pushdown
   +--> B -> ordinary scan pushdown
   +--> C -> dict_execution scan pushdown
   +--> D -> residual

rebuild
   |
   +--> scan options:
   |      fringedb.filter.velox = {
   |        ts: [A],
   |        account_id_dict_idx: [B],
   |        domain_dict_idx: [C]
   |      }
   |
   +--> upper Filter = D
```

If this step is skipped, we risk:

1. double evaluation
2. no real pushdown benefit
3. hard-to-debug correctness issues

### Phase 8: Native Tide Consumption

Validate or extend Tide/native scan-side behavior so it can consume:

```text
fringedb.filter.velox
```

One of the following must be true:

1. native scan already accepts serialized `LowCardDictExecution` entries in the existing per-column list
2. native scan is extended only in expression handling, not in outer payload parsing

This is the main integration checkpoint for Option C.

### Phase 9: Tests

Add tests at four levels.

1. Converter unit tests
   - `LowCardDictExecution` serializes successfully
   - serialized output is stable
2. Extractor unit tests
   - only eligible conjuncts are extracted
   - residual condition is preserved
3. Scan builder tests
   - writes only `fringedb.filter.velox`
   - preserves `Map[String, Seq[String]]` shape exactly
   - appends serialized `LowCardDictExecution` entries into the correct per-column list
4. End-to-end tests
   - simple equality still prefers direct literal-index path
   - complex dict predicate uses dict-execution pushdown
   - when `tide.sql.dictLiteralIndex.enabled=false && tide.sql.dictAdaptivePushdown.enabled=true`, pushed dict-execution count never exceeds `tide.sql.dictAdaptivePushdown.maxDictPushdownCount`
   - unsupported dict predicates remain residual

## Minimum Required Changes

If we really want this to become a deal, the smallest serious implementation scope looks like this:

1. extract pushdown-eligible `LowCardDictExecution` conjuncts after dict2 rewrite
2. define a stable transport format for scan-side dict execution
3. add gateway-side builder logic to write that format into `fringedb.filter.velox`
4. add explicit conversion support for `LowCardDictExecution` if expression serialization is still used
5. add Tide/native scan-side handling for the new contract
6. keep residual predicates above scan for correctness
7. add e2e tests for mixed cases:
   - partition filter + dict execution
   - direct literal-index equality + dict execution
   - multiple dict-execution conjuncts with adaptive selection
   - unsupported dict predicate remains residual

## What Already Works Better For Simple Cases

Even with this broader pushdown work, direct encoded predicates are still the easiest and safest win:

```text
col_dict_idx = literal_index
```

So the intended strategy should probably remain:

1. simplest equality / direct-index cases -> direct scan pushdown
2. more complex single-column dict predicates -> `low_card_dict_execution` pushdown via new bridge
3. everything else -> residual upper-layer execution

That matches the adaptive-pushdown design docs much better than an all-or-nothing approach.

## Revised Final Conclusion

With your stated assumption, the answer is:

```text
yes, this can be made to work.
```

But also:

```text
the current codebase does not implement the bridge yet.
```

So the accurate statement is:

- this is **not** blocked by principle
- this **is** blocked by missing plumbing
- the work is cross-module, but still reasonable

If I had to commit to one sentence:

```text
`low_card_dict_execution` pushdown in TideScanBuilder is feasible,
provided we add an explicit transport bridge from dict2 rewrite output to Tide scan options.
```

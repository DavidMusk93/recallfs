# SQL AST Analyzer Plan

## Goal

Build a Java project that accepts a batch of SQL statements, parses each SQL into
an AST, preprocesses and normalizes it, extracts structured features, stores the
results in DuckDB, and generates multi-dimensional reports.

The first target input is:

```text
/Users/bytedance/Downloads/vqos_mirror.xlsx
```

The workbook contains a `Query Result` sheet with these columns:

```text
start_time | req_id | diff_ms | sql | error_message
```

Observed input profile:

```text
total SQL rows   : 13574
unique SQL rows  : 13561
main table       : ti.vqos_view
main dialect     : ClickHouse-style SELECT SQL
common functions : toStartOfInterval, toDateTime, toDate, toHour, concat, lpad,
                   toString, sum, cast, uniq
common clauses   : where, group by, having, order by, limit
not observed     : join, union, with/cte, subquery, arrays, prewhere, settings
```

Input timing fields:

```text
diff_ms:
          input column name for query execution latency.
          Stored as query_duration_ms in DuckDB.
          This is not the queried time window.

query_range_seconds:
          derived by the analyzer from primary-key range filters.
          Example: ts >= start AND ts < end -> end - start.
```

## Technology Stack

The project uses Java as the main implementation language.

```text
+-----------------------+-----------------------------------------------+
| Area                  | Choice                                        |
+-----------------------+-----------------------------------------------+
| Language              | Java 21                                       |
| SQL parser generator  | ANTLR4                                        |
| SQL dialect base      | ClickHouse ANTLR grammar                      |
| AST conversion        | Generated parser + custom Visitor             |
| Storage               | DuckDB via JDBC                               |
| Build                 | Maven                                         |
| Tests                 | JUnit 5                                       |
+-----------------------+-----------------------------------------------+
```

Java is part of the project architecture for these reasons:

- ANTLR4 has first-class Java support.
- ClickHouse grammar can be modified when unsupported syntax appears.
- AST modeling, visitors, expression rewrites, and extraction rules benefit from
  Java's type system.
- DuckDB JDBC is enough for storage and report generation.

Python is outside the parser and normalization core. Python usage is limited to
notebooks, exploratory analysis, and ad-hoc report prototyping.

## End-to-End Flow

```text
             +--------------------+
             | SQL batch input    |
             | xlsx / csv / text  |
             +---------+----------+
                       |
                       v
             +--------------------+
             | SQL row loader     |
             | req_id, sql, meta  |
             +---------+----------+
                       |
                       v
             +--------------------+
             | ClickHouse parser  |
             | ANTLR parse tree   |
             +---------+----------+
                       |
             success?  |
          +------------+-------------+
          |                          |
          v                          v
+--------------------+     +--------------------+
| AST builder        |     | parse error record |
| custom SQL AST     |     | reason + location  |
+---------+----------+     +----------+---------+
          |                           |
          v                           |
+--------------------+                |
| constexpr eval     |                |
| fold/prove filters |                |
+---------+----------+                |
          |                           |
          v                           |
+--------------------+                |
| preprocessing      |                |
| normalize/template |                |
+---------+----------+                |
          |                           |
          v                           |
+--------------------+                |
| feature extraction |                |
| time/filter/expr   |                |
+---------+----------+                |
          |                           |
          +------------+--------------+
                       |
                       v
             +--------------------+
             | DuckDB storage     |
             | normalized tables  |
             +---------+----------+
                       |
                       v
             +--------------------+
             | reports            |
             | sql/html/csv/md    |
             +--------------------+
```

## Parser Strategy

Start from ClickHouse ANTLR grammar files:

```text
src/main/antlr/
  ClickHouseLexer.g4
  ClickHouseParser.g4
```

Generated parser output is an internal detail. Business logic depends on the
project-owned AST model, not directly on ANTLR parse tree classes.

```text
ANTLR parse tree
      |
      v
ClickHouseAstBuilder visitor
      |
      v
Project-owned AST
      |
      +--> normalizer
      +--> constexpr evaluator
      +--> time range extractor
      +--> and-chain filter extractor
      +--> report feature extractor
```

When a SQL fails to parse:

```text
failed SQL
    |
    v
classify failure
    |
    +--> lexer issue
    +--> parser rule issue
    +--> unsupported dialect construct
    +--> input quality issue
    |
    v
add regression test
    |
    v
patch .g4 grammar
```

## And-Chain Filter Extraction

Filter extraction splits only the top-level `AND` chain. It must not split
inside `OR` expressions or otherwise change boolean semantics.

After splitting, the extractor removes redundant wrapper parentheses around an `OR` chain when
the parentheses only wrap the whole extracted filter unit.

Example:

```sql
a > 1 AND b = '1' AND (c > 2 OR d != 'xyz')
```

Extracted filter units:

```text
1. a > 1
2. b = '1'
3. c > 2 OR d != 'xyz'
```

Algorithm:

```text
splitAndChain(expr):
  if expr is AND:
    return splitAndChain(expr.left) + splitAndChain(expr.right)
  else:
    return [unwrapWholeExprParentheses(expr)]
```

Parenthesis unwrapping rule:

```text
unwrapWholeExprParentheses(expr):
  while expr is parenthesized and removing the outer pair keeps the same AST:
    expr = expr.inner
  return expr
```

This rule removes parentheses around a whole `OR` unit:

```text
(c > 2 OR d != 'xyz') -> c > 2 OR d != 'xyz'
```

This rule does not remove parentheses that are needed inside a larger expression:

```text
a > 1 AND (b = '1' OR c = '2') -> split first, then unwrap the second unit
```

Examples:

```text
Input:
  (a > 1 AND b = '1') AND (c > 2 OR d != 'xyz')

Output:
  a > 1
  b = '1'
  c > 2 OR d != 'xyz'

Input:
  a > 1 AND (b = '1' OR c = '2')

Output:
  a > 1
  b = '1' OR c = '2'
```

## Filter Set Rules

Some AND-chain units are set constraints. When the same column and operator
appear more than once, the engine can reduce them with set algebra before
template generation.

The first rule is `InListIntersectionRule`.

Example:

```sql
col in ('a','b','c') AND col in ('b','c','a','d')
```

Both filters have the same pattern:

```text
subject  : col
operator : in
value    : finite literal set
```

The reducer sorts each literal set and intersects constraints for the same
`subject + operator` group:

```text
{a,b,c} intersect {a,b,c,d} = {a,b,c}
```

Final filter:

```sql
col in ('a','b','c')
```

Flow:

```text
AND-chain filters
       |
       v
+----------------+      +----------------------+
| match col IN   |----->| group column/operator|
+--------+-------+      +----------+-----------+
         |                         |
         v                         v
+----------------+      +----------------------+
| sort literals  |----->| intersect same group |
+--------+-------+      +----------+-----------+
         |                         |
         v                         v
+----------------+      +----------------------+
| render raw     |----->| render template      |
+----------------+      +----------------------+
```

Rules:

```text
same column + same operator only
literal lists are sorted for stable rendering
raw filters keep original literal values
template filters replace the reduced list with one placeholder
empty intersection becomes false
unsupported list items stay unchanged
```

## Normalization Rules

Literal and text normalization:

```text
integer literals -> 1
float literals   -> 2.0
string literals  -> '3'
remove redundant spaces
remove redundant parentheses
remove unnecessary aliases
```

Canonical and template normalization rules:

```text
+--------------------------+--------------------------------------------+
| Rule                     | Example                                    |
+--------------------------+--------------------------------------------+
| keyword/function case    | SELECT SUM(x) -> select sum(x)             |
| quoted identifiers       | "count" -> `count` or stable quote style   |
| comparison operators     | <> -> !=                                   |
| not-in canonicalization  | not a in (...) -> a not in (...)           |
| in-list normalization    | a in ('x','y','z') -> a in ('3')           |
| in-list intersection     | a in ('x','y') and a in ('y') -> a in ('y')|
| limit extraction         | limit 20000 -> limit_value = 20000         |
| limit template           | template_sql excludes LIMIT by default     |
| alias safety             | keep alias if referenced by group/order    |
| constant metadata        | preserve raw value and normalized value    |
+--------------------------+--------------------------------------------+
```

Normalization sequence:

```text
raw AST
  |
  +--> time range extraction runs before destructive literal replacement
  |
  v
canonical normalization
  |
  v
template normalization
```

The system stores two normalized outputs:

```text
canonical_sql : Semantics-preserving stable SQL text.
template_sql  : Constant-replaced SQL for grouping and template reports, with
                LIMIT and primary-key filters removed into separate columns.
```

Separated normalization outputs:

```text
limit_value                 : raw LIMIT value, stored separately
primary_key_filters         : filters derived from or directly constraining primary_key
primary_key_filter_template : primary-key filters after constexpr simplification,
                              with original constants preserved
template_sql                : SQL template with non-primary-key business filters
```

Primary-key filters are not literal-templated. They are extracted before
destructive literal replacement, removed from `where_and_chain` / `template_sql`,
and stored in dedicated columns with original constants preserved.
After constexpr evaluation removes redundant partition/index hint expressions,
the remaining primary-key filter template is stored separately.

## Time Range Extraction

Extract time constraints from WHERE/HAVING filters before replacing constants.

Common patterns from the current input:

```sql
ts >= 1775129400 and ts <= 1775133299
ts >= 1775526900 and ts < 1775526960
ts >= '2026-04-09 08:00:00 +08:00'
toDate(ts) = '2026-04-07'
concat(toDate(ts), ' ', lpad(toString(toHour(toDateTime(ts))), 2, '0 ')) >= '2026-04-02 19'
```

Primary-key timestamp constants may be numeric epoch seconds or timestamp
strings. Timestamp strings are converted to epoch seconds for proof and range
duration calculation, while the raw filter text remains unchanged for audit.

Supported timestamp examples:

```text
'2026-04-09 08:00:00 +08:00' -> epoch seconds using explicit offset
'2026-04-09T08:00:00+08:00'  -> epoch seconds using explicit offset
'2026-04-09 08:00:00'        -> epoch seconds using EvalContext timezone
```

Extraction output:

```text
time_column      : ts
start_value      : raw lower bound
end_value        : raw upper bound
start_inclusive  : true/false
end_inclusive    : true/false
source_expr      : original filter expression
```

Range duration output:

```text
query_range_seconds = upper_bound_epoch_seconds - lower_bound_epoch_seconds
```

## Predicate Reduction Engine

The project includes a production-grade predicate reduction engine. Its purpose
is not only to fold expressions that are already constant. It also proves that
one primary-key filter makes another primary-key derived filter redundant by
binding the primary-key value into a deterministic expression.

The key case is:

```sql
concat(toDate(ts), ' ', lpad(toString(toHour(toDateTime(ts))), 2, '0 '))
    >= '2026-04-07 04'
AND ts >= 1775508660
```

Both filters constrain the same primary key. The first filter is a derived
primary-key expression; the second filter is a direct primary-key bound. Because
the operators are aligned (`>=` and `>=`), the direct bound can be substituted
into the derived expression under local time:

```text
ts := 1775508660

toDate(ts)                         -> '2026-04-07'
toDateTime(ts)                     -> 2026-04-07 04:51:00 localtime
toHour(toDateTime(ts))             -> 4
toString(4)                        -> '4'
lpad('4', 2, '0 ')                 -> '04'
concat('2026-04-07', ' ', '04')    -> '2026-04-07 04'
'2026-04-07 04' >= '2026-04-07 04' -> true

true AND ts >= 1775508660          -> ts >= 1775508660
```

Engine scope:

```text
+-------------------------------+---------------------------------------------+
| Capability                    | Scope                                       |
+-------------------------------+---------------------------------------------+
| literal folding               | numbers, strings, dates, booleans, null     |
| deterministic evaluation      | registered deterministic scalar functions   |
| symbolic binding              | bind field constraints into derived exprs   |
| operator alignment            | use compatible bound/op pairs safely        |
| boolean reduction             | and/or/not with true/false/unknown          |
| range reduction               | merge same-field ranges, detect conflict    |
| rule plugins                  | import domain-specific binding/eval rules   |
| audit trace                   | record every proof step and rewrite reason  |
+-------------------------------+---------------------------------------------+
```

Core flow:

```text
raw where expr
     |
     v
+-------------------+
| split AND units   |
+---------+---------+
          |
          v
+-------------------+       +----------------------+
| classify pattern  +------>| direct field bound   |
| field / func / op |       | ts >= 1775508660     |
+---------+---------+       +----------+-----------+
          |                            |
          |                            v
          |                 +----------------------+
          |                 | binding environment  |
          |                 | ts := 1775508660     |
          |                 +----------+-----------+
          |                            |
          v                            v
+-------------------+       +----------------------+
| derived expr rule |------>| substitute and eval  |
| f(ts) >= const    |       | f(1775508660) >= c   |
+---------+---------+       +----------+-----------+
          |                            |
          v                            v
+-------------------+       +----------------------+
| proof decision    |<------| true / false / unk   |
+---------+---------+       +----------------------+
          |
          v
+-------------------+
| boolean reducer   |
| true AND x -> x   |
+---------+---------+
          |
          v
simplified filters
```

### Trivial Nest Flattening

Some generated queries wrap a simple scan in a transparent subquery:

```sql
select starttime - starttime % 1 as t, count(1) as c
from (
    select *
    from ti.dwd_tlb_flow_log_nginx_access_log_hi_view
    where psm = '3' and url_path = '3' and scheme = '3' and aid = 1
) as data_tiview_temp_t
group by t
```

This pattern should be flattened before predicate normalization:

```text
outer select
    |
    v
from (select * from base where inner_filter) as alias
    |
    v
from base where inner_filter
```

Formal rewrite:

```text
SELECT outer_exprs
FROM (SELECT * FROM T WHERE P) AS A
<outer_tail>

==>

SELECT outer_exprs
FROM T
WHERE P
<outer_tail>
```

If the outer query already has a `WHERE Q`, combine filters:

```text
FROM (SELECT * FROM T WHERE P) AS A
WHERE Q

==>

FROM T
WHERE (Q) AND (P)
```

The rule is intentionally conservative. It only fires when the inner query is a
single `select *`, has exactly one base table, has a `where` clause, and has no
`distinct`, `prewhere`, `group by`, `having`, `order by`, `limit`, `settings`,
`join`, or `union`.

### Formal Model

A filter unit is normalized into:

```text
Predicate := Subject Operator Constant
Subject   := Field(name) | Derived(function_tree, base_field)
Operator  := = | != | > | >= | < | <= | in | not in
Constant  := typed literal
```

A direct primary-key bound creates a binding candidate:

```text
Binding := base_field := boundary_constant under operator direction

ts >= 1775508660 -> Binding(ts, lower, inclusive, 1775508660)
ts >  1775508660 -> Binding(ts, lower, exclusive, 1775508660)
ts <= 1775512319 -> Binding(ts, upper, inclusive, 1775512319)
ts <  1775512320 -> Binding(ts, upper, exclusive, 1775512320)
```

A derived predicate is reducible when:

```text
P1 = Derived(f, ts) op_d c_d
P2 = Field(ts)     op_b c_b

base(P1) = field(P2)
direction(op_d) = direction(op_b)
f is deterministic and monotone enough for the registered rule
eval(f(c_b) op_d c_d) = true
```

Then:

```text
P1 AND P2  ==  P2
```

under the registered rule's monotonicity contract.

For the ClickHouse hour-bucket expression:

```text
bucket(ts) =
  concat(toDate(ts), ' ', lpad(toString(toHour(toDateTime(ts))), 2, '0 '))
```

the rule declares `bucket(ts)` as non-decreasing for epoch-second `ts` under the
configured timezone when rendered as `yyyy-MM-dd HH`. Therefore:

```text
ts >= c  =>  bucket(ts) >= bucket(c)
ts <= c  =>  bucket(ts) <= bucket(c)
```

If `bucket(c) >= expected` is true, then every row satisfying `ts >= c` also
satisfies `bucket(ts) >= expected`. The derived predicate can be removed from an
AND-chain without changing the result set.

For lower-bound operators:

```text
op_d in {>, >=}
op_b in {>, >=}
```

For upper-bound operators:

```text
op_d in {<, <=}
op_b in {<, <=}
```

For equality:

```text
op_d = =
op_b = =
eval(f(c_b) = c_d) = true
```

If `eval(...) = false`, the conjunction is unsatisfiable:

```text
P1 AND P2 == false
```

If the rule cannot prove determinism, compatible direction, or monotonicity, the
result stays `unknown` and no filter is removed.

### Boolean Reduction

The reducer uses three-valued proof results:

```text
Truth := true | false | unknown
```

Conjunction:

```text
true  AND x      -> x
false AND x      -> false
x     AND true   -> x
x     AND false  -> false
unknown AND true -> unknown
unknown AND x    -> keep both unless x is false
```

Disjunction:

```text
true  OR x       -> true
false OR x       -> x
x     OR true    -> true
x     OR false   -> x
unknown OR false -> unknown
unknown OR x     -> keep both unless x is true
```

The first production target is top-level AND-chain reduction. OR-chain reduction
is only allowed inside a self-contained expression when the engine can prove the
whole expression to `true` or `false`.

### Rule Plugin Model

Rules are imported as plugins so dialect and domain knowledge stay outside the
core reducer.

```text
+-----------------------+---------------------------------------------+
| Rule type             | Responsibility                              |
+-----------------------+---------------------------------------------+
| FunctionRule          | evaluate deterministic function calls       |
| BindingRule           | produce field bindings from direct filters  |
| DerivedPredicateRule  | match f(field) op const patterns            |
| MonotonicityRule      | declare safe operator direction alignment   |
| RenderRule            | render simplified filters for storage       |
+-----------------------+---------------------------------------------+
```

Public interfaces:

```java
interface PredicateReducer {
    ReductionResult reduce(ExpressionNode predicate, ReductionContext context);
}

interface FunctionRule {
    Optional<ConstValue> evaluate(String name, List<ConstValue> args, EvalContext context);
}

interface BindingRule {
    Optional<FieldBinding> bind(PredicateUnit predicate, ReductionContext context);
}

interface DerivedPredicateRule {
    Optional<ProofStep> prove(
            PredicateUnit derived,
            FieldBinding binding,
            ReductionContext context);
}

record ReductionContext(
        ZoneId timezone,
        String primaryKey,
        FunctionRegistry functions,
        List<ReductionRule> rules,
        NullSemantics nullSemantics
) {}
```

The first bundled plugin is `ClickHouseTimeKeyRules`:

```text
functions:
  toDate(epoch_seconds)
  toDateTime(epoch_seconds)
  toHour(datetime)
  toString(value)
  lpad(value, size, pad)
  concat(...)

derived subjects:
  toDate(ts)
  toHour(toDateTime(ts))
  concat(toDate(ts), ' ', lpad(toString(toHour(toDateTime(ts))), 2, '0 '))
```

Trace output is mandatory:

```text
proof.step[0] pattern=direct-bound expr="ts >= 1775508660"
proof.step[1] pattern=derived-bound expr="concat(...) >= '2026-04-07 04'"
proof.step[2] bind ts=1775508660
proof.step[3] eval "concat(...)" -> "2026-04-07 04"
proof.step[4] compare "'2026-04-07 04' >= '2026-04-07 04'" -> true
proof.step[5] rewrite "true AND ts >= 1775508660" -> "ts >= 1775508660"
```

Development plan:

```text
1. Predicate units
   [ ] Parse AND-chain units into PredicateUnit(subject, op, constant)
   [ ] Keep unsupported units as opaque symbolic predicates

2. Rule registry
   [ ] Add FunctionRegistry for deterministic function plugins
   [ ] Add ReductionRule loading from built-in Java ServiceLoader plugins
   [ ] Add ClickHouseTimeKeyRules as the first bundled plugin

3. Binding environment
   [ ] Extract direct primary-key bounds
   [ ] Keep lower/upper/equality bindings separately
   [ ] Preserve inclusivity for >, >=, <, <=

4. Derived predicate proof
   [ ] Match field_func op constant patterns
   [ ] Check base field equality
   [ ] Check operator direction compatibility
   [ ] Substitute boundary constant into derived expression
   [ ] Evaluate via FunctionRegistry
   [ ] Emit ProofStep with true/false/unknown

5. Boolean reducer
   [ ] Remove true units from top-level AND-chain
   [ ] Collapse false AND-chain to constant false
   [ ] Keep unknown units unchanged
   [ ] Preserve original raw filters for audit columns

6. Storage integration
   [ ] Store raw primary-key filters
   [ ] Store reduced primary-key filters separately
   [ ] Remove reduced true hints from template_sql and where_and_chain
   [ ] Add proof_trace JSON column or trace side table

7. Regression tests
   [ ] Lower-bound hour hint: derived >= const AND ts >= const -> ts >= const
   [ ] Upper-bound hour hint: derived <= const AND ts <= const -> ts <= const
   [ ] Contradiction: derived >= later_bucket AND ts <= earlier_epoch -> false
   [ ] Unknown plugin gap: unsupported function keeps both filters
   [ ] Timezone regression: Asia/Shanghai boundary around midnight
```

## Partition Hint Filters

Some filters are generated as partition or primary-key index hints rather than
business filters. In the current input, `toDate(ts)` and `toHour(toDateTime(ts))`
patterns usually serve this purpose.

Examples:

```sql
toDate(ts) = '2026-04-07'
concat(toDate(ts), ' ', lpad(toString(toHour(toDateTime(ts))), 2, '0 ')) >= '2026-04-02 19'
concat(toDate(ts), ' ', lpad(toString(toHour(toDateTime(ts))), 2, '0 ')) <= '2026-04-02 20'
```

These filters are classified as `partition_hint` when they are derived from the
same base time column as the real time range filter.

Example:

```sql
toDate(ts) = '2026-04-07'
and ts >= 1775526900
and ts < 1775526960
```

The canonical time range comes from `ts >= ... and ts < ...`. The `toDate(ts)`
filter is retained as an extracted filter unit, but it is marked as a partition
hint and excluded from business filter reports by default.

Dry-run simplification:

```text
raw filter units
    |
    v
classify primary-key predicates
    |
    v
build direct field bindings
    |
    v
match derived primary-key predicates
    |
    v
substitute boundary constants
    |
    v
deterministic eval + operator proof
    |
    +--> redundant hint: covered by canonical ts range
    +--> contradiction: whole AND-chain becomes false
    +--> active hint: no canonical ts range covers it
```

When a dry-run engine proves that a partition hint is fully implied by the
canonical `ts` range, the filter is marked as redundant. The original filter
text remains stored for audit and parse coverage, but normalization and template
hashing use a version with redundant partition hints removed.

Storage and report behavior:

```text
business reports         : exclude redundant partition_hint filters by default
template_sql             : remove all primary-key and redundant partition_hint filters
primary_key_filters      : preserve raw primary-key and partition/index hint filters
primary_key_filter_template:
                           store constexpr-simplified primary-key filters
                           without replacing their constants
canonical_sql            : preserve the original semantic filter structure
time range reports       : use canonical ts range, not partition hint strings
parse/debug reports      : include all filter units
```

## DuckDB Schema

DuckDB storage uses a wide analytical table. DuckDB is column-oriented and
supports nested types, so AST-derived features are stored as arrays, structs,
and JSON columns instead of many normalized tables. The final reporting workload
primarily returns to string/list analysis, so the schema is optimized for simple
scans and aggregations.

```sql
create table sql_analysis (
    query_id varchar primary key,
    raw_sql varchar not null,
    canonical_sql varchar,
    template_sql varchar,
    template_hash varchar,
    limit_value bigint,
    parse_status varchar not null,
    error_message varchar,
    source_file varchar,
    source_row bigint,
    start_time timestamp,
    req_id varchar,
    query_duration_ms bigint,
    query_range_seconds bigint,
    primary_key varchar,

    table_refs varchar[],
    select_columns varchar[],
    function_names varchar[],

    where_and_chain varchar[],
    where_and_chain_normalized varchar[],
    where_business_filters varchar[],
    primary_key_filters varchar[],
    primary_key_filter_template varchar[],

    having_and_chain varchar[],

    referenced_columns varchar[],
    dimension_columns varchar[],
    literal_values json,

    time_ranges struct(
        time_column varchar,
        start_value varchar,
        end_value varchar,
        start_inclusive boolean,
        end_inclusive boolean,
        source_expr varchar
    )[],

    filter_metadata struct(
        expr_text varchar,
        normalized_expr varchar,
        root_operator varchar,
        filter_role varchar,
        is_redundant boolean,
        covered_by_time_range_index integer,
        referenced_columns varchar[]
    )[],

    parse_error_location struct(
        line integer,
        column_index integer
    )
);
```

Convenience views can explode nested columns for specific reports:

```sql
create view v_where_filter as
select
    query_id,
    idx as ordinal,
    filter as expr_text
from sql_analysis,
unnest(where_and_chain) with ordinality as t(filter, idx);

create view v_function_usage as
select
    query_id,
    function_name
from sql_analysis,
unnest(function_names) as t(function_name);
```

The base table remains the source of truth. Exploded views are report helpers,
not storage ownership boundaries.

## Reports

Report dimensions:

```text
+-------------------------+---------------------------------------------+
| Report                  | Questions                                   |
+-------------------------+---------------------------------------------+
| parse coverage          | How many SQLs parse successfully?           |
| template frequency      | Which normalized templates are most common? |
| slow templates          | Which templates have high query_duration_ms?|
| filter frequency        | Which filters appear most often?            |
| dimension columns       | Which columns are used in WHERE filters?    |
| function usage          | Which functions are used most frequently?   |
| time range distribution | What time windows are queried?              |
| parse failures          | Which syntax forms need grammar patches?    |
+-------------------------+---------------------------------------------+
```

Report flow:

```text
DuckDB tables
    |
    +--> SQL views
    |
    +--> CSV / Markdown / HTML report
    |
    +--> optional notebook exploration
```

## Implementation Milestones

```text
M1 Project skeleton
  [x] Maven project
  [x] Package layout
  [x] Plan document
  [x] DuckDB schema

M2 Parser POC
  [ ] Add ClickHouse grammar files
  [ ] Parse sample SQL
  [ ] Capture parse errors with location

M3 AST and extraction
  [ ] Build project-owned AST
  [ ] Implement top-level AND-chain splitter
  [ ] Implement time range extractor
  [ ] Implement select/function/table extractors

M4 Constexpr evaluation
  [x] Implement constant folding
  [x] Implement constexpr boolean evaluation
  [ ] Implement predicate unit classifier
  [ ] Implement field binding extraction
  [ ] Implement pluggable function/rule registry
  [ ] Implement derived primary-key substitution proof
  [ ] Implement boolean AND-chain reduction
  [ ] Implement simple range algebra
  [ ] Implement partition hint redundancy detection
  [ ] Emit proof trace for every removed filter

M5 Normalization
  [x] Implement canonical_sql
  [x] Implement template_sql
  [x] Implement template hash
  [x] Implement IN-list intersection rule
  [ ] Add alias safety rules

M6 DuckDB storage
  [ ] Create all tables
  [x] Batch insert query results
  [ ] Add report SQL views

M7 Full input coverage
  [x] Load vqos_mirror.xlsx or exported CSV
  [x] Parse all 13574 SQL rows
  [ ] Patch grammar until target coverage is reached
  [ ] Generate first report pack
```

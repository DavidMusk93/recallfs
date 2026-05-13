# dict2 rewrite rules (logical vs physical)

This note summarizes the rewrite behavior implemented in the dict2 module.

## What “logical” vs “physical” means here

- **Logical plan (Catalyst logical operators)**: describes query semantics (Project/Filter/Aggregate/Sort/Limit, etc.). The dict2 logical rewrite focuses on:
  - switching dict-backed columns from value -> index representation,
  - preserving user-visible output names,
  - inserting decode only at correctness boundaries (where local evaluation requires values),
  - attaching and propagating dict metadata via tags for later stages.
- **Physical plan (Spark exec operators / native engine integration)**: decides execution stages (local vs global), codegen/native execution, and where to inject a *final* decode so the client sees String outputs. The dict2 physical rules may:
  - insert an additional Project that decodes indices to values at the final output boundary,
  - use tags set on the logical/physical operators (e.g. `OUTPUT_DECODE_TAG`) to decide where that output decode is required.

## Core idea

- Leaf relations expose dictionary-backed columns as integer indices (`${col}_dict_idx`) and attach dictionary metadata (`dictName`, `dictVersion`) to the attribute metadata.
- Most logical operators should **forward indices** (keep index-typed attributes) while preserving the **original output names** (so the logical schema still looks like `${col}`).
- Decoding (`index -> value`) is represented by `low_card_dict_decode` and is expected to be executed by the native engine.
- A separate “output decode” decision is made at the output boundary (typically in physical planning). The logical plan can mark this intent via `OUTPUT_DECODE_TAG` (commonly on `GlobalLimit`) so the physical rules know to inject a final decode Project.

## Filter rule (safe)

- In logical `Filter`, dict-backed columns are rewritten to `low_card_dict_execution(index, dictName, dictVersion, jsonFilter)` when the predicate can be expressed as a native JSON filter.
- This is safe because it keeps the plan index-based and pushes filtering into the dict engine.

## Decode boundary rules (tricky)

Decoding is inserted only when an operator **evaluates dict-dependent expressions in a local stage** and index forwarding would no longer be valid across operator boundaries.

- **Aggregate**
  - Plain grouping key `groupBy(dictCol)` can stay index-based.
  - If Aggregate contains **compound expressions** involving dict columns (e.g. `groupBy(f(dictCol))`, aggregate function inputs, or other non-trivial expressions), decode must happen **before** the Aggregate’s local evaluation.
  - The rewrite *prefers* to stop the dict rewrite chain in the **parent Project** that owns the compound expression: that Project will emit `low_card_dict_decode(${col}_dict_idx) AS ${col}` and drop the mapping for `${col}`.

- **Sort**
  - If Sort contains dict columns (single or compound) in ordering expressions, decode must happen **before** Sort.
  - The rewrite *prefers* to stop the dict rewrite chain in the **parent Project** that produces the ordering keys: that Project will emit `low_card_dict_decode(${col}_dict_idx) AS ${col}` and drop the mapping.
  - For `ORDER BY alias` (e.g. `ORDER BY k` where `k := domain`), the probe phase inspects the child Project’s `projectList` to infer dict dependencies and requests decoding at that Project boundary.

## Two-phase rewrite (probe & build)

The logical rewrite is implemented as two distinct passes to keep responsibility clear and make the behavior easier to maintain.

- **Probe (top-down)**: deduce which dict columns must be decoded at which Project boundary and record the decision as tags.
- **Build (bottom-up)**: perform expression/operator rewrites using the child dict mapping tag and the tags produced by probe.

### Tags

- `DICT_MAPPING_TAG`: `ExprId(value_col) -> (DictInfo, encoded_attr_in_scope)` propagated bottom-up.
- `REQUIRED_DECODE_TAG` (on Project): value-column ExprIds that must be decoded at this Project boundary, and whose mapping should be dropped (stop rewrite chain).
- `CHILD_DECODE_TAG` (on Sort/Aggregate): value-column ExprIds that these operators require their child subtree to have already decoded; used to detect “unsatisfied” decode requirements.

## Output boundary (GlobalLimit) and final decode

- `GlobalLimit` is commonly used as a stable logical boundary close to the final output (e.g. `collect`, `show`, `take`, some sinks).
- We mark `GlobalLimit` with `OUTPUT_DECODE_TAG` to signal: “this subtree’s final output should be decoded to values”.
- The logical rewrite still prefers index-forwarding everywhere else; the physical stage can then inject a final decode Project above/below the appropriate exec node(s) without polluting upstream operators with `low_card_dict_decode`.

## Breaking the rewrite chain

When a column is decoded at a boundary Project:

- The Project outputs `low_card_dict_decode(${col}_dict_idx) AS ${col}` (preserving name and exprId for plan stability).
- The dict mapping tag for that column is **removed** so parent operators stop rewriting that column as dict-encoded.

## Fallback behavior (when no Project exists)

Some logical subtrees may not have a Project layer that can naturally “own” the compound expression (or that layer has been removed by earlier optimizer rules). In that case, the build phase applies a minimal fallback (in practice, it may introduce a small Project) so that Sort/Aggregate correctness is preserved.

## Mental model (quick)

- Logical rewrite answers: “Where can we safely stay index-based, and where must we decode for correctness?”
- Physical planning answers: “Where is the final output boundary, and where should the final decode be injected so users see values?”

## Build & test commands

From repo root (`gateway.dict/`):

```bash
# compile starry-core (and required deps)
./mvnw compile -pl starry/starry-core -am
```

```bash
# run dict2-focused ScalaTest suites (starry-core)
./mvnw -pl starry/starry-core test \
  -DfailIfNoTests=false \
  -DwildcardSuites=org.apache.spark.sql.execution.dict2.RewriteWithGlobalDictSuite,org.apache.spark.sql.execution.dict2.RewriteWithGlobalDictSQLSuite
```

```bash
# generate jacoco report (starry-core)
./mvnw -pl starry/starry-core jacoco:report
```

Optional: compute dict2 package line coverage from the generated CSV:

```bash
python3 - <<'PY'
import csv
path='starry/starry-core/target/site/jacoco/jacoco.csv'
miss=cover=0
with open(path,newline='') as f:
    r=csv.DictReader(f)
    for row in r:
        if row['PACKAGE']=='org.apache.spark.sql.execution.dict2':
            miss += int(row['LINE_MISSED'])
            cover += int(row['LINE_COVERED'])
print('dict2 line coverage %0.2f%% (%d covered, %d missed)' % (cover/(cover+miss)*100, cover, miss))
PY
```

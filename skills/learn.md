# Learn

Goal: turn web resources into durable knowledge and the smallest complete
engineering artifact that serves the user's actual goal. A demo is one possible
experiment, not the default definition of done.

## 1. Trigger

Use this workflow when the user provides a URL, blog post, paper, article, or open-source implementation and asks to learn, reproduce, extract, or build knowledge from it.

## 2. Output Location

Use the `learning/` module.

```text
learning/
  sources/
  studies/YYYYMMDD-short-topic/
    README.md
    source.md
    exploration.md
    demo/ | lib/ | tool/ | benchmark/
      CMakeLists.txt
      README.md
      src/
      include/
    evidence/
```

## 3. Workflow

```text
+---------------------------+
| Receive URL and goal      |
+-------------+-------------+
              |
              v
+---------------------------+
| Name the study            |
| YYYYMMDD-short-topic      |
+-------------+-------------+
              |
              v
+---------------------------+
| Archive the source page   |
| keep URL and date         |
+-------------+-------------+
              |
              v
+---------------------------+
| Extract technical claims  |
+-------------+-------------+
              |
              v
+---------------------------+---- no ---->+---------------------------+
| Needs executable proof?   |             | Write structured summary  |
+-------------+-------------+             +---------------------------+
              | yes
              v
+---------------------------+
| Select artifact by goal   |
| demo/lib/tool/benchmark   |
+-------------+-------------+
              |
              v
+---------------------------+
| Record exploration path   |
+-------------+-------------+
              |
              v
+---------------------------+
| Write final learning note |
+---------------------------+
```

## 4. Source Archiving

- Save the source content under `learning/sources/YYYYMMDD-short-topic.md` when possible.
- Record the original URL in `source.md`.
- Record access date and any fetch limitations.
- Record the source revision, version, or digest when a claim depends on a
  particular artifact state.
- If the page cannot be fully archived, save a summary and note the limitation.

## 5. Technical Learning Artifacts

- Identify the smallest mechanism worth reproducing, then select the artifact
  type from the requested outcome.
- Use a `demo/` only for a bounded experiment or explanation.
- Use a `lib/` when callers need a reusable API, a `tool/` for an operational
  workflow, and a `benchmark/` for a performance claim.
- If the user requests production use, a demo or happy-path prototype is not a
  valid final deliverable.
- A production library must define ownership, error and retry semantics,
  compatibility/versioning, resource bounds, concurrency boundaries,
  persistence/recovery where applicable, and installation/consumption steps.
- Generalize domain types and interfaces when the mechanism is reusable. Do not
  bake a source article's sample types or one workload into the core unless the
  user explicitly requests that specialization.
- Prefer the smallest complete implementation, not the fewest source lines.
  Completeness includes failure paths, observability, tests, and documentation.
- If the user asks to extract an open-source module, isolate the smallest
  reusable base implementation first.
- Every executable artifact must be a directory with a standard project layout,
  not a loose source file.
- Prefer CMake for C/C++ artifacts and use out-of-source builds.
- Use the natural input shape of the target problem. For example, a sorting demo should accept a sequence such as `std::vector<int>`, not an artificial record type unless the record is the real subject.
- Add comments for non-obvious concepts, especially names introduced by the article. A reader should understand what the concept means before reading the implementation details.
- Use practical test data that represents realistic usage, not only hand-picked toy values.
- Add comparison tests against a baseline or simpler alternative. If an idea cannot show when it helps, it is not yet useful guidance for practice.
- Record build commands, run commands, observed output, and mismatches.
- Do not claim production readiness from unit tests alone. Match evidence to the
  risk surface: differential or independent oracles, corruption and fault
  injection, recovery/reopen tests, sanitizer/safety tooling, release builds,
  install/consumer tests, and target-machine benchmarks when performance is a
  claim.

Recommended C/C++ demo layout:

```text
demo/
  CMakeLists.txt
  README.md
  src/
  include/
  build/        # generated locally; do not commit
```

macOS notes:

- Record compiler path and version when a build issue appears.
- Prefer `/usr/bin/clang++` or the CMake-selected system compiler when custom toolchains cause runtime library issues.
- Record `dyld`, `@rpath`, SDK, deployment target, and architecture issues in `exploration.md`.
- Do not commit binaries, `build/`, temporary object files, or generated project files.

## 6. Documentation Rules

- `README.md` contains final conclusions, architecture, demo result, and next steps.
- `source.md` contains URL, archive path, author/source metadata, and access date.
- `exploration.md` records the step-by-step path, including failed attempts.
- The selected executable artifact directory contains a runnable standard
  project, including build files and source directories.
- `evidence/` contains logs, outputs, screenshots, and benchmark data.
- Demo source, fixtures, and tests are maintained evidence unless their subtree
  declares an explicit generation contract. Generated build output remains
  disposable.
- A behavior-bearing study must include reconciliation anchors: concrete
  inputs, exact expected outputs or tolerances, and the command or probe that
  verifies each anchor.

### Language and Mathematical Notation

- Write the final study report in `README.md` in Chinese unless the user
  explicitly requests another language.
- Keep established technical terms, protocol names, API names, identifiers,
  and code symbols in English. Do not force awkward Chinese translations for
  domain vocabulary.
- Use Markdown LaTeX for mathematical notation: `$...$` for inline expressions
  and `$$...$$` for displayed equations or derivations.
- Do not put equations in `text` or code fences, and do not emulate subscripts,
  fractions, products, or arrows with plain-text underscores and ASCII.
- Reserve `text` fences for actual ASCII architecture, topology, state-machine,
  or control-flow diagrams.
- `source.md` may preserve original titles and quotations. `exploration.md` and
  `evidence/` may preserve raw tool output in its original language.

## 7. Agent Checklist

- Is the study name date-prefixed and descriptive?
- Is the original URL preserved?
- Is the source revision, version, or digest recorded where relevant?
- Is the source archived or limitation documented?
- Is the core technical claim identified?
- Does the artifact type match the user's goal rather than defaulting to a demo?
- If production use was requested, is there a reusable API plus explicit
  compatibility, failure, concurrency, recovery, and verification contracts?
- Is the executable artifact a standard project folder rather than loose source?
- Does it use a reusable input model rather than an accidental example type?
- Are important article-specific concepts explained in comments?
- Does the demo include realistic data and baseline comparison?
- Are commands and results reproducible?
- Are behavior claims tied to explicit reconciliation anchors?
- Are macOS pitfalls and toolchain details recorded when relevant?
- Are conclusions separated from exploration notes?
- Is the final report Chinese while established technical terms remain English?
- Are mathematical definitions and derivations rendered with Markdown LaTeX
  rather than plain-text code blocks?

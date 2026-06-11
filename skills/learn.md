# Learn

Goal: standardize long-term learning from web resources into archived sources, reproducible demos, and reusable technical knowledge.

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
    demo/
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
| Needs reproduction?       |             | Write structured summary  |
+-------------+-------------+             +---------------------------+
              | yes
              v
+---------------------------+
| Build minimal demo        |
| C++ or extracted module   |
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
- If the page cannot be fully archived, save a summary and note the limitation.

## 5. Technical Blog Reproduction

- Identify the smallest mechanism worth reproducing.
- Prefer a minimal C++ prototype for systems topics.
- If the user asks to extract an open-source module, isolate the smallest base implementation first.
- Keep the demo small and focused on the article's core claim.
- Record build commands, run commands, observed output, and mismatches.

## 6. Documentation Rules

- `README.md` contains final conclusions, architecture, demo result, and next steps.
- `source.md` contains URL, archive path, author/source metadata, and access date.
- `exploration.md` records the step-by-step path, including failed attempts.
- `demo/` contains runnable prototypes or extracted modules.
- `evidence/` contains logs, outputs, screenshots, and benchmark data.

## 7. Agent Checklist

- Is the study name date-prefixed and descriptive?
- Is the original URL preserved?
- Is the source archived or limitation documented?
- Is the core technical claim identified?
- Is there a demo, or a clear reason why no demo is needed?
- Are commands and results reproducible?
- Are conclusions separated from exploration notes?

# Learning Module

This module stores long-lived technical learning records. Each study starts from a source URL, archives the source content, reproduces the key idea with a demo when possible, and preserves the exploration path for future agents.

## 1. Directory Layout

```text
learning/
  README.md
  sources/      # Archived web pages or source snapshots
  studies/      # One directory per learning topic
  templates/    # Reusable study templates
```

Study directory convention:

```text
learning/studies/YYYYMMDD-short-topic/
  README.md          # Final learning note and conclusion
  source.md          # Source URL, archive path, metadata
  exploration.md     # Step-by-step exploration log
  demo/              # Standard demo project
    CMakeLists.txt   # Preferred for C/C++ demos
    README.md        # Build, run, and platform notes
    src/             # Demo implementation
    include/         # Optional public headers
    build/           # Local generated build output; do not commit
  evidence/          # Logs, outputs, screenshots, benchmark records
```

## 2. Naming Rules

| Item | Format | Example |
| --- | --- | --- |
| Archived source | `YYYYMMDD-short-topic.md` | `20260601-lsm-compaction.md` |
| Study directory | `YYYYMMDD-short-topic/` | `20260601-lsm-compaction/` |
| Demo directory | `demo/` inside study | `learning/studies/20260601-lsm-compaction/demo/` |
| Evidence directory | `evidence/` inside study | `learning/studies/20260601-lsm-compaction/evidence/` |

Rules:

- Use lowercase English words and `-`.
- Include the date when the learning is started.
- Keep the topic short but meaningful.
- Preserve the original URL in `source.md`.

## 3. Learning Workflow

```text
+---------------------------+
| User provides URL/topic   |
+-------------+-------------+
              |
              v
+---------------------------+
| Create study directory    |
| YYYYMMDD-short-topic      |
+-------------+-------------+
              |
              v
+---------------------------+
| Archive source content    |
| record URL and metadata   |
+-------------+-------------+
              |
              v
+---------------------------+
| Extract core idea         |
| claims, mechanism, limits |
+-------------+-------------+
              |
              v
+---------------------------+---- no demo ---->+---------------------------+
| Is it technical content?  |                  | Write summary and lessons |
+-------------+-------------+                  +---------------------------+
              | yes
              v
+---------------------------+
| Build standard demo       |
| project folder, CMake     |
+-------------+-------------+
              |
              v
+---------------------------+
| Record exploration path   |
| commands, failures, fixes |
+-------------+-------------+
              |
              v
+---------------------------+
| Write final study note    |
| conclusion and next steps |
+---------------------------+
```

## 4. Study Rules

- Archive the source before summarizing it.
- Always record the source URL and access date.
- For technical blogs, reproduce the core mechanism with a demo when feasible.
- A demo can be a fresh C++ prototype or an extracted module from an open-source project.
- Every demo must be stored as a project folder, not as loose source files.
- Prefer CMake for C/C++ demos and use out-of-source builds under `demo/build/`.
- Keep generated build files and binaries out of Git.
- Exploration must be recorded as decisions, commands, failures, and fixes.
- On macOS, record toolchain pitfalls such as `dyld`, `@rpath`, SDK, architecture, and deployment-target issues.
- Prefer tables for comparisons, assumptions, parameters, and results.
- Use ASCII graphs for architecture and data flow.
- Keep conclusions separate from raw notes.

## 5. Demo Project Rules

Recommended C/C++ layout:

```text
demo/
  CMakeLists.txt
  README.md
  src/
  include/
  build/        # generated locally; do not commit
```

| Rule | Reason |
| --- | --- |
| Use a project folder | Keeps demos reproducible as they grow |
| Use CMake for C/C++ | Makes compiler, flags, and build steps explicit |
| Use out-of-source builds | Avoids mixing generated files with source |
| Record macOS issues | macOS toolchains often fail through SDK, RPATH, or `dyld` differences |
| Save run logs in `evidence/` | Keeps validation separate from source |

## 6. Promotion Rules

| Condition | Action |
| --- | --- |
| Demo becomes reusable | Move or copy it into a project-specific source directory |
| Study creates a general method | Promote it to `skills/` |
| Study becomes a design basis | Link it from `designs/` or project docs |
| Source becomes obsolete | Keep the archive and add a note; do not delete history |

## 7. Agent Checklist

- Did the study directory use `YYYYMMDD-short-topic`?
- Did `source.md` include the original URL?
- Was the source archived under `learning/sources/`?
- Was the technical claim reproduced or explicitly marked as not reproduced?
- Is the demo a standard project folder with build instructions?
- Are generated build files and binaries excluded from Git?
- Are macOS toolchain pitfalls recorded when encountered?
- Are commands, logs, and failures recorded in `exploration.md`?
- Does `README.md` contain the final conclusion and next steps?

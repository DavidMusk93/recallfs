# Study Title

## 1. Conclusion

- Summary:
- What was verified:
- What remains uncertain:

## 2. Source

| Field | Value |
| --- | --- |
| URL |  |
| Access Date |  |
| Archive |  |
| Topic |  |

## 3. Core Idea

- Claim:
- Mechanism:
- Constraints:
- Expected result:

## 4. Architecture

```text
+----------------------+
| source idea          |
+----------+-----------+
           |
           v
+----------------------+
| minimal reproduction |
+----------+-----------+
           |
           v
+----------------------+
| observed result      |
+----------------------+
```

## 5. Demo

| Item | Value |
| --- | --- |
| Path | `demo/` |
| Language |  |
| Project Layout | `CMakeLists.txt`, `README.md`, `src/`, optional `include/` |
| Build Command | `cmake -S demo -B demo/build && cmake --build demo/build` |
| Run Command | `demo/build/<binary>` |
| macOS Notes | Compiler, SDK, architecture, `dyld`, or `@rpath` issues |

```text
demo/
  CMakeLists.txt
  README.md
  src/
  include/
  build/        # generated locally; do not commit
```

## 6. Exploration Log

| Step | Action | Result | Evidence |
| --- | --- | --- | --- |
| 1 |  |  |  |

## 7. Lessons

- Reusable insight:
- Failure pattern:
- Follow-up:

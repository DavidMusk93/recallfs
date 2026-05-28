## Commit Message Guidelines

We use a two-part format: a short subject line plus a longer body.

### Format

```
<short subject in imperative mood>

Context: <what problem / why now>
Change: <what changed at a high level>
Behavior: <what behavior/semantics changed, if any>
Testing: <how it was verified>
Notes: <tradeoffs / follow-ups / risks>
```

### Rules

- Subject line:
  - Keep it short (preferably <= 50 chars).
  - Use imperative mood (e.g. "Refactor ...", "Fix ...", "Add ...").
  - No trailing period.
- Body:
  - Wrap lines at ~72 chars when possible.
  - Explain "why" before "what".
  - Explicitly call out any behavior/semantic changes.
  - Always include a Testing line.
  - Describe only content that is actually included in the commit.

### Pitfall: `\n` in `git commit -m`

- Avoid writing commit bodies like:

```
git commit -m "Title" -m "Context: ...\nChange: ...\nTesting: ..."
```

- Depending on how the shell passes the string, `\n` may end up recorded
  literally in the commit message body instead of becoming real line breaks.
- Prefer multiple `-m` flags instead, one paragraph per `-m`:

```
git commit -m "Title" \
  -m "Context: ..." \
  -m "Change: ..." \
  -m "Testing: ..."
```

### Example

```
Refactor partition table event consumer

Context: Multiple lagging consumers could retain historical tables.
Change: Split weak/deque implementations behind a factory.
Behavior: Default uses weak refs; deque remains opt-in via env.
Testing: blade test //:partition_consumer_ut --bundle=release
Notes: Docs kept local until reviewed.
```

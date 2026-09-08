# Sequence-Lane Mechanism Contract

Use a sequence-lane mechanism when the conclusion depends on **event order
across multiple actors and shared mutable state**. Typical subjects include
races, TOCTOU failures, lock handoffs, retries, cache invalidation, leader
changes, transaction recovery, and delayed corruption.

Do not compress these mechanisms into an ASCII pseudo-table. ASCII is still
appropriate for small topology and control-flow diagrams, but it becomes
ambiguous when one column is state rather than an actor or when a bad state is
created in one event and consumed later.

## 1. Reading Contract

A reader must be able to answer, without reading surrounding prose:

1. In which direction does time move?
2. Which columns are active actors?
3. Which column is shared state rather than an actor?
4. What does each state variable mean?
5. At which step does the first invalid state appear?
6. At which later step does that invalid state become user-visible damage?

Use this model:

```text
rows    = monotonically ordered moments
columns = time + actor lanes + explicit shared-state lane
cell    = what that actor does, or what state is true, at that moment
```

The shared-state lane must say that it is not an actor. Name its storage
location when known, such as `wal-index / -shm`, a catalog row, a replicated
log, or a process cache.

## 2. Information Model

Before the lane board:

- state the time direction;
- define symbols and state fields;
- distinguish private snapshots from live shared values;
- state any ordering predicate, such as `m < L`.

Inside the board:

- number every row;
- use one row per causally meaningful moment;
- keep actor names stable across the entire board;
- show idle/waiting actors when their non-action matters;
- show the live shared state after each mutation;
- label the race window explicitly;
- label the first invalid invariant explicitly;
- put delayed damage in a separate final row.

Do not rely on tint alone. Pair warning/danger colors with text such as
`race window`, `invalid invariant`, and `data loss`.

## 3. Semantic Markup

Use one visible structure for desktop and mobile. Do not duplicate the facts
into separate hidden desktop/mobile diagrams.

```html
<figure class="sequence-figure" aria-labelledby="sequence-title">
  <figcaption>
    <strong id="sequence-title">
      Time moves from top to bottom.
    </strong>
    <span>
      The final lane is shared state, not another actor.
    </span>
  </figcaption>

  <dl class="sequence-terms" aria-label="State field definitions">
    <div>
      <dt>generation</dt>
      <dd>Identity of the current state generation.</dd>
    </div>
  </dl>

  <div
    class="sequence-board"
    data-sequence-lanes="time,actor-a,actor-b,shared"
  >
    <div class="sequence-head">
      <span data-sequence-lane="time" data-sequence-kind="time">Step</span>
      <span data-sequence-lane="actor-a" data-sequence-kind="actor">
        Actor A
      </span>
      <span data-sequence-lane="actor-b" data-sequence-kind="actor">
        Actor B
      </span>
      <span data-sequence-lane="shared" data-sequence-kind="shared-state">
        Shared state <small>not an actor</small>
      </span>
    </div>

    <ol class="sequence-rows">
      <li class="sequence-row">
        <div class="sequence-time" data-sequence-lane="time">1</div>
        <div class="sequence-cell" data-sequence-lane="actor-a">
          <span class="mobile-lane">Actor A</span>
          <strong>Reads generation G</strong>
          <p>Keeps a private snapshot.</p>
        </div>
        <div class="sequence-cell" data-sequence-lane="actor-b">
          <span class="mobile-lane">Actor B</span>
          <strong>Advances to generation G + 1</strong>
          <p>Publishes new shared state.</p>
        </div>
        <div class="sequence-cell" data-sequence-lane="shared">
          <span class="mobile-lane">Shared state</span>
          <strong>Generation G + 1</strong>
          <p>The old snapshot is now stale.</p>
        </div>
      </li>

      <li class="sequence-row sequence-loss">
        <div class="sequence-time" data-sequence-lane="time">2</div>
        <div
          class="sequence-outcome"
          data-sequence-span="actor-a,actor-b,shared"
        >
          <strong>Delayed consequence</strong>
          <p>A later operation trusts the poisoned state.</p>
        </div>
      </li>
    </ol>
  </div>
</figure>
```

`data-sequence-lanes` is the ordered lane manifest. Every ordinary row must
contain exactly one direct child for each lane in that order. A spanning
outcome uses `data-sequence-span` to declare the lanes it replaces.

## 4. Alignment Contract

The lane board owns the column definition. Header and rows consume the same
CSS custom property:

```css
.sequence-board {
  --sequence-columns:
    72px minmax(0, 1fr) minmax(0, 1fr) minmax(0, 1.25fr);
  width: 100%;
  overflow: hidden;
}

.sequence-head,
.sequence-row {
  display: grid;
  grid-template-columns: var(--sequence-columns);
}

.sequence-row {
  width: 100%;
  max-width: none;
}
```

Hard rules:

- Never repeat the track list independently in header and row selectors.
- Reset inherited prose constraints such as `li { max-width: 72ch; }`.
- The board fills the report's mechanism width; do not use `width:
  max-content` for a multi-lane sequence.
- Header and ordinary rows have the same lane count and lane order.
- Every lane boundary must match the corresponding header boundary within
  `0.75px`.
- A spanning outcome starts at the first declared span lane and ends at the
  last declared span lane.
- A glossary above the board is a separate legend. Do not draw unrelated
  vertical separators that imply alignment with the lane tracks.
- Do not fake alignment with per-cell margins, transforms, or hand-tuned
  padding.

## 5. Responsive Contract

Desktop keeps the lane matrix. At the report's mobile breakpoint, preserve
event order by stacking the lanes inside each numbered row:

```css
@media (max-width: 700px) {
  .sequence-head {
    display: none;
  }

  .sequence-row {
    grid-template-columns: 52px minmax(0, 1fr);
  }

  .sequence-time {
    grid-row: 1 / 4;
  }

  .sequence-cell,
  .sequence-outcome {
    grid-column: 2;
  }

  .mobile-lane {
    display: block;
  }
}
```

Mobile lane labels must be real text in the DOM. Do not create semantic labels
only with CSS `content`. Do not shrink the whole desktop board until its text
becomes unreadable.

## 6. Visual Hierarchy

- Keep normal rows neutral.
- Tint only the actor/state cells involved in the race window.
- Give the first bad invariant the strongest emphasis.
- Keep the delayed consequence visually distinct from the point where the bad
  state was created.
- Use a monospace face for state equations, not for explanatory prose.
- The mechanism is a full-width technical surface, not a small centered card.

## 7. Browser Gate

Run the complete source of:

```text
../scripts/html_sequence_alignment_probe.js
```

A pass returns:

```json
{
  "ok": true,
  "errors": []
}
```

The probe checks:

- lane manifest, header kinds, and shared-state declaration;
- full-width row geometry;
- header/row lane-boundary alignment on desktop;
- declared outcome spans;
- visible mobile lane labels;
- component and page overflow.

Capture focused screenshots of the full mechanism at desktop and 390px. Read
the screenshots as a human: the probe cannot decide whether the event
decomposition is causally correct or whether the shared-state explanation is
understandable.

## 8. Failure Patterns

Reject a sequence-lane mechanism when:

- a state column looks like another actor;
- time direction is implicit;
- symbols appear before they are defined;
- header and rows each own a separate track list;
- global prose width rules narrow `<li>` rows inside a full-width board;
- a three-column glossary sits directly above a four-column board with
  misleading vertical alignment;
- the diagram marks only the final failure and not the first bad invariant;
- mobile hides headers without adding visible lane labels;
- color is the only indication of the race or failure.

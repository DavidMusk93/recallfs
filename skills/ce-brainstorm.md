# CE Brainstorm Skill (`/ce-brainstorm`)

> Origin: [EveryInc/compound-engineering-plugin](https://github.com/EveryInc/compound-engineering-plugin)

## Overview

`/ce-brainstorm` is an interactive skill for scoping, pressure-testing, and defining software architecture and product features before implementation planning.

## Core Rules

1. **One Question at a Time**: Ask a single, high-leverage question per turn to avoid overwhelming the user or diluting choices.
2. **Gap Lenses Pressure Test**: Scan the request for gaps:
   - **Evidence**: Is user demand or behavior grounded in observation?
   - **Specificity**: Are core actors and scenarios well-defined?
   - **Counterfactual**: What happens if nothing changes or an alternative path is taken?
   - **Attachment**: Is a pre-selected implementation restricting better solutions?
3. **Approach Exploration**: Explore 2-3 distinct approaches (with pros/cons and a clear recommendation) before locking down the design.
4. **Artifact Generation**: Save output to `designs/` or `docs/plans/` as a structured design/requirements document.

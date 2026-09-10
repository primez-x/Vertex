# Persistent wall constraints v1

Constraint entities store relationships to semantic wall endpoints. Wall
baselines remain the geometry authority; constraint records do not contain a
second editable point collection. This format is part of project format v1.
Interactive constraint authoring remains under construction.

## Stable envelope

```json
{
  "id": "wall-horizontal",
  "type": "constraint",
  "required": false,
  "properties": {
    "version": 1,
    "relation": "horizontal",
    "wall_ids": ["wall-a"],
    "bindings": [
      {"owner_id": "wall-a", "feature": "baseline", "role": "start"},
      {"owner_id": "wall-a", "feature": "baseline", "role": "end"}
    ]
  },
  "extensions": {}
}
```

`wall_ids` is sorted and unique and must contain exactly the owners referenced
by `bindings`. Each owner must be an existing wall. V1 known bindings use
`baseline` and the named `start` or `end` role; array indexes and inferred
coincidence do not create stable references. Duplicate endpoint bindings are
invalid. Unknown metadata stays separate from the recognized semantic fields.

| Relation | Bindings | Additional semantics |
| --- | --- | --- |
| `horizontal` | Two points | Equal Y within the linear tolerance |
| `vertical` | Two points | Equal X within the linear tolerance |
| `coincident` | Two points | Coincident within the linear tolerance |
| `fixed_length` | Two points | Positive `length_m` and exact quantity receipt |
| `parallel` | Two ordered endpoint pairs | Parallel directions, including opposite directions |
| `perpendicular` | Two ordered endpoint pairs | Perpendicular directions |
| `fixed_anchor` | One point | Desired fixed position in `anchor_m: [x, y]` |

Pairs may refer to separate walls. A known v1 relation cannot constrain an arc
baseline. Curved walls themselves remain supported by the architectural engine;
their persistent curve constraints require a later codec and solver extension.

For a fixed length, the additional properties are:

```json
{
  "length_m": 0.1016,
  "quantity_entries": {
    "/length_m": {
      "version": 1,
      "original_expression": "1/3 ft",
      "entered_unit": "ft",
      "exact_metres": {"numerator": 127, "denominator": 1250}
    }
  }
}
```

The expression, recorded unit, normalized rational and canonical metre value
must agree. Unit conversion uses the shared quantity parser; display rounding
cannot rewrite a lock. The canonical receipt follows the same structure as
architectural dimension receipts. Optional unrelated entries are opaque data.
Versioned codec edits preserve unknown metadata and may not silently migrate
an unsupported relation/version into a known relation.

## State integrity

The solver-free validator runs in the document transition boundary after
structural reference checks. It independently evaluates every known hard
relation against coordinates derived from the candidate walls. Linear and
angular tolerances are shared with the solver adapter in
`constraint_tolerances.hpp`: `1e-6` metre and `1e-8` radian.

Constraint-owner walls and their hosted openings use the same pure semantic
validator as solid construction. It rejects nonfinite or unrepresentable
dimensions, degenerate baselines, out-of-bounds or overlapping openings, and
openings whose exact unwrapped union removes the entire wall. OCCT still owns
solid construction and numerical kernel validity; near-zero retained solids
are not qualified by the pure predicate alone.

Raw entity edits cannot bypass hard relations. Geometry and relation changes
must form one valid candidate state before any history is appended. Explicit
constraint removal plus geometry change is one reversible operation. Exact
baseline reversal requires an atomic remap of surviving endpoint bindings.

Known malformed or violated relations reject creation, mutation or restoration.
A well-formed unsupported version/relation is preserved, but the document
becomes read-only. Unsupported lock semantics anywhere in retained history
also make a restored document read-only, preventing undo into a state whose
locks cannot be evaluated. The optional `required` flag does not permit the
application to ignore a constraint.

## Current verification and remaining work

Codec tests exercise all seven relations and exact quantities. Document tests
cover direct edit/delete rejection, independent residual checks, compound
removal, reversal/remapping, hosted-opening validity, save/reopen, and
unsupported historical locks. CLI fixtures independently construct valid
project manifests to distinguish semantic rejection from checksum failure.

The preview/Apply service, anchor and connected-movement controls, boundary
vertex identities, curve constraints, branch/topology propagation, level
dependencies, and interactive conflict repair remain required. This format
and its focused tests do not certify the complete constraint workflow.

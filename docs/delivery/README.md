# Vertex delivery registry and resume entry

[plan.md](plan.md) is the current finite delivery plan.
[requirements.json](requirements.json) is the single delivery crosswalk for the
130 original Apex rows, 13 Pinc adoption requirements, 134 Pinc operations,
450 unique manual scenarios (U001–U449 plus U100a), binding user feedback and
83 dated implementation plans. The original requirement, comparison, inventory,
checklist, feedback and dated plan files retain their original text and evidence.
The registry links them; mapping a row does not accept it.

The G1–G10 production gates map to one accountable package each, D01–D10.
Each Apex row gets exactly one owner from its existing gate membership, rather
than its older construction-sequence package number. That original number and
the complete original source row remain in `source_snapshot`. Pinc operations
are owned by their actual document, geometry, drawing, architectural or output
package. Adoption requirements use those owners plus D10 for full paired
qualification. All still belong to the required Pinc production baseline. Manual rows
retain full titles, exact expected results and existing steps, with no added
engineering clauses. `null` test, fixture or evidence references mean no such
reference is recorded; a source filename is not a passing test result.

To resume, read the current plan and [progress.md](progress.md), then the
registry's `current_package`. D00 migration precedes all work. D03 first takes
the same-head snapshot and validates document/history continuity. The next
integrated milestone is the ordinary drawing job. Geometry and room work,
architectural authoring, adapters/OCR and distribution can then proceed under
separate ownership. The full release gate remains last. Continue the current
bounded package and its concrete acceptance work; there is no automatic goal
loop, restart of completed scope, reduced release, or preferred appraisal host.

For each row, implement its `remaining_behavior`, exercise its
`required_variants`, and compare the actual result with `expected_result` and
`assertions`. Those fields derive from existing acceptance and blocker text;
they are obligations to verify, not newly generated pass claims. Package
dependencies identify required interfaces without splitting ownership. They do
not require an entire upstream package to certify before every downstream
subtask can proceed. External compatibility acquisition does not block
component or document authority work; root sequences concrete interface needs.
Record newly established tests, fixtures and evidence by repository-relative
path. Keep private fixture locations and machine-specific checkout paths out
of tracked files.

The four states remain distinct:

- `implementation`: source reports or newly reviewed implementation work.
- `focused_verification`: recorded focused checks for named behavior.
- `installed_verification`: observations against the identified installed build.
- `final_acceptance`: observed acceptance with supporting references and an
  accountable observer; initialized to `not_accepted` for every row.

`source_reported_verified` preserves the old ledger's label and note. It does
not promote that claim to a current focused or installed pass. Historical
verification references and dated proofs remain available separately. Old
one-gap execution cadence is superseded by the consolidated plan; its past
results are not erased. Registry integrity tests verify mapping and bookkeeping
only and cannot establish product, compatibility or production acceptance.

Use these finite local checks from the checkout:

```powershell
python -B scripts/delivery_registry.py --check
python -B -m unittest discover -s tests -p test_delivery_registry.py
```

After a source acceptance or mapping input changes, refresh with
`python -B scripts/delivery_registry.py --write`, then run the checks. Refresh
preserves progress when the semantic definition and applicable source/build
bindings remain unchanged. Definition changes or changed applicable inputs
reset current checks and acceptance, preserving old records in
`qualification_history`. Notes, implementation reports and current package
remain available. Publication validates the payload and atomically replaces
the registry through a unique sibling temporary file; a failed replacement
preserves existing progress.

Each row's definition hash covers its behavior, acceptance, variants, assertions,
owner, interface dependencies and package pass condition. The conservative
product-input hash covers source, headers, assets, resources, CMake configuration,
tests/scripts, bundled help and top-level dependency/packaging manifests. Generated delivery
state, Git metadata and build output are excluded. A product-input change
invalidates all current qualification rather than guessing affected coverage.
Focused passes bind their evidence to the definition/source and current build
binding, which can be null for source-only checks. Installed passes and final
acceptance require an explicit candidate build or manifest SHA-256 digest;
`--write --build-binding <digest>` records that identity without inferring a
successful build. Copy the exact `applicability_binding` values into the recorded
verification/acceptance `binding` and attach observed evidence. A changed build
binding invalidates existing bound passes. Root owns integration, candidate
identity and final engineering review.

The manual checklist's initial task-count prose was historically stale at 423;
the source IDs contain 450 scenarios. The canonical registry verifies exact
IDs rather than trusting that prose. Plan files and source inventories remain
historical references, not additional accepted releases or new denominators.

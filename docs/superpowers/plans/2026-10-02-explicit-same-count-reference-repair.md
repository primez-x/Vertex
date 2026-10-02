# Explicit reference repair for changed exterior geometry

The existing source-repair dialog refuses changed geometry when edge count stays
the same. Equal counts do not establish corresponding walls or corners. Complete
the repair by assigning fresh topology and asking the user to map or remove
attached references, using the existing redraw planner.

Outcome: a replacement wall shell can have different geometry and retain the
existing appraisal area's facts, name, style, factors, deductions and revision
history. Equivalent shells continue to preserve their stable children directly.
Never silently guess a dimension or constraint's new edge.

Ownership: core worker owns typed intent, integrity, persistence reader floors
and core fixtures. UI worker owns main_window.cpp and the review/clone consumers.
Root owns desktop fixtures, integration, builds, documentation, generators, Git
and delivery. All writers freeze before builds; unrelated temp.txt is preserved.

1. Record actual failing native same-count repair and core fresh-topology intent
   checks before editing production code.
2. Append explicit fresh_topology intent to redefinition. Strict version 4
   requires true, reference-plan fields and replacement-wall IDs (which may be
   empty for a generic redraw). Keep older encodings unchanged. Require native
   17/exchange 15 across retained history and imported derivations.
3. Reuse existing changed-topology integrity checks for fresh equal-count child
   sets: disjoint IDs, explicit mapping/removal, one-to-one references and valid
   locks. Regenerate automatic dimensions; retain manually positioned ones only
   after reviewed mapping. Preserve geometry/source provenance in one command.
4. Enable changed-geometry source proposals with fresh topology. Open the existing
   reference planner only on Apply. Cancel and every stale context remain detached.
   Show readable wall descriptions, source details and original/proposed geometry.
5. Verify straight/curved shells in both unit systems, manual references and
   constraints, nested cancellation, appraisal quantities and deductions, one-owner
   history, clone/paste, native reopen and downgrade refusal. Review the integrated
   integrity change independently, fix required gaps, commit/push and install the
   matching build. The full production release remains the terminal goal.

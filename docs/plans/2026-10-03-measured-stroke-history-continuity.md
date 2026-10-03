# Measured-stroke drawing continuity through history

An active measured stroke must remain drawable after undoing or redoing its own
committed edge. Restore its actual pen, anchor, model and drawing context from
history; preserve exact receipts and stable identities. A new edge after Undo
uses that restored pen and replaces the abandoned Redo branch. Each edge stays
one document operation.

Intentional Finish, Escape and right-click still finish the session while
keeping committed geometry. An uncommitted anchor can be cancelled without
undoing unrelated project work. Stale, foreign or changed-context state must not
revive a drawing session. Save/reopen remains ordinary document navigation and
does not silently resume drawing.

The worker owns desktop history/session implementation and a new native
interaction regression. Root owns build registration, docs/checklist, integrated
review, focused verification, source-kit generation, Git and installed delivery.
All native writers freeze during builds and executable checks. No project-format
change is planned because geometry and committed history remain authoritative.

Verify a behavioral failure first, then actual Ctrl+Z/Ctrl+Y and click
continuation, typed receipt retention, first-edge Undo, Redo, branching,
stale-context refusal, finish semantics and save/reopen. Run affected existing
measured-line and area tests. This is part of the full accepted production plan,
not a replacement for its remaining Apex compatibility or ANSI gates.

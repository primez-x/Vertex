# Wall workflow repair

Outcome: make the ordinary click-to-draw wall workflow consistent through
closure and later editing, and supply one explicit current build to test.

Observed in the latest packaged application with native mouse input: idle
clicks create physical wall segments, deliberately offset clicks align to the
preceding corner, endpoint clicks close the perimeter, and retained lengths
appear. Closure currently leaves the chain active. Source inspection also
shows that matching endpoint coordinates do not create durable connections.
Two different executable versions are open locally; preserve their unsaved
work and avoid replacing loaded executables.

Source discovery also found that menu/command wall drawing could stay in an
architectural projection, where wall snapping is disabled. Library activation
already routes to the conventional plan. Centralize that routing for every
explicit wall and measurement authoring intent; projected coordinates must not
be accepted as world-XY drawing input.

Implementation: finish a chain when it returns to its original anchor. Persist
satisfied endpoint coincidence relations when walls are authored together in
the same drawing context. Commit a wall and its relations atomically. Reuse the
constraint solver for later length editing, without silently breaking locked
measurements. Explain that library doors/windows are hosted variants of the
same opening model used by the quick insertion tools.

Ownership: wall writer owns MainWindow and a separate connection regression;
workflow reviewer owns the existing wall/opening regression. Root owns CMake,
integration, documentation, builds, package delivery and Git.

Verification: actual click closure and snapping, retained dimensions, connected
length edit, context isolation, atomic undo/redo and save/reopen. Recheck wall
opening placement and exterior appraisal measurement. Inspect the final
rendered result and provide its precise executable path. These repairs do not
certify the entire production scope.

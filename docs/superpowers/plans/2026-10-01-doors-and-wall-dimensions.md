# Door mechanisms and wall dimension layout

Continue the production application goal by repairing two observed gaps:
automatic wall lengths overlap exterior dimensions, and catalog Double/Sliding
Glass doors currently manufacture the same unspecified closed leaf.

Root owns desktop integration, schedules, IFC exchange, documentation,
integration checks, native verification, packaging and Git. A door worker owns
the operation codec and analytical/manufactured geometry. A canvas worker owns
automatic linear-label placement and renderer/picking verification. Preserve
unrelated dirty work.

Automatic wall labels follow the wall, keep their chosen outward side and seek
readable positions against other label footprints using actual screen/output
font metrics. Authored dimensions keep their persisted geometry and placement.
Rendering, hit testing, selection and fitted output must share the layout.

Double doors receive two leaves and two swings. Sliding glass doors receive a
fixed and movable panel on separate physical tracks, with bounded editable
travel. Persist the mechanism without changing existing v1 hinged records;
all new geometry must derive from that stored model in plans and 3D. Catalog
placement, undo/redo, dimension edits, schedules and exchange preserve it.

Run regressions demonstrating each failure before its fix. Verify affected core
and desktop paths, inspect the integrated changes and actual native app, then
commit/push and provide an updated build. These repairs do not complete the
original production gate. Pocket/bifold/barn/garage mechanisms, specialized
window/roof hosting, Apex fixtures/adapters and other recorded gaps remain in
the full goal and require further implementation and qualification.

Native verification found the 3D renderer retaining its original 480 x 360
backing viewport after a larger split workspace opened. Repair that lifecycle
before delivering this checkpoint: synchronize the actual native render size
after layout without resetting navigation. A visualization worker owns its
view implementation, header and focused native regression; root verifies the
new installed application against the preserved failure screenshot.

The corrected extent also exposed a first-fit camera using the pre-layout
aspect. Defer that initial fit until layout settles, and drain it before the
first navigation gesture. Subsequent resize and show cycles must retain the
camera. Verify both the first framed model and preserved navigation.

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

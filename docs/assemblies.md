# Reusable assembly semantics

`AssemblyModel` supplies a bounded, geometry-independent semantic foundation for
ARCH-MOD-008. Types declare text properties, named material slots, and explicit
per-instance quantities. Instances reference one type and store overrides
separately, including overrides whose value equals the type default. Material
slots reference a validated local catalog.

Resolution copies type defaults and replaces only explicitly overridden keys.
Quantities use explicit SI dimensions (`count`, `m`, `m2`, `m3`, `kg`), are finite
and nonnegative, and counts are integral. An override must name a declared
property or slot; quantity overrides must retain the declared dimension.
Quantities are authored semantic values, not computed geometry measurements or
totals. There is no implicit multiplication, conversion, or material takeoff.

`preview_type_update` validates a full replacement and returns before/after
resolved values and retained override provenance for every referencing instance,
ordered by instance ID. `with_type` applies those exact rules to a detached new
model. Inherited values follow the replacement; explicit overrides survive.
Removing an overridden key or changing its quantity dimension rejects the whole
replacement. Removing an unoverridden key is allowed. `with_instance` replaces an
existing instance; removing an override restores inheritance. Unknown replacement
identities fail rather than silently creating records.

Models have no mutable accessors. Retaining the previous snapshot enables callers
to restore it for undo without reverse calculations. JSON uses
`sketch.assemblies.v1`, preserves source defaults and overrides separately, sorts
catalogs by ID and map keys lexicographically, rejects unexpected fields and
invalid references, and round-trips without losing override provenance. Returned
JSON and resolved values are detached copies.

The Windows Architectural workspace exposes an **Assembly catalog** from the
More menu and command palette. It creates the typed `assembly_model` record on
demand, adds and removes reusable types and placed instances, renames types,
and edits the local material catalog through normal revision-fenced Document
history. A type cannot be removed while an instance still references it, so the
catalog never leaves a dangling placement.

The catalog's Type schema tab edits named text properties, material slots, and
dimensioned quantities. The Instance overrides tab edits explicit per-instance
property, material-slot, and quantity overrides. Each save is a single
undoable replacement validated by `AssemblyModel`: undeclared keys, unknown
materials, negative or non-finite values, fractional counts, and quantity-unit
changes are rejected without partial mutation. The list views show the stored
defaults and overrides, including values that intentionally equal their type
default.

Architectural objects can now reference a catalog material directly from the
inspector. The optional entity property `material_assignment` contains integer
`version: 1`, `catalog_id` (the `assembly_model` entity ID), and `material_id`
(the catalog's internal material ID). Walls, openings, rooms and room boundaries,
slabs, roofs, stairs, columns, and beams support this binding. Document validation
rejects unsupported versions, unsupported object roles, and missing targets.
Removing a referenced material or catalog requires detaching its users in the
same atomic command. Assignment and removal use normal revision-checked history
and survive project save/reopen. This does not bind objects to assembly instances.

Material schedules resolve the current catalog name and report one assigned
object per row. Names and counts are read-only with source provenance; renaming
the catalog material updates derived rows. Visibility filtering applies to the
object, even if the catalog itself is outside the view. Assigned objects do not
reuse legacy authored `volume_m3` as a computed material quantity. Unassigned
legacy material rows retain their existing authored name/volume behavior.
The catalog currently supplies names, not appearance or physical properties;
assignment does not change 3D shading or calculate material volume.

Assembly geometry bindings and placement, quantity takeoff integration, nested
assemblies, material physical properties, and publication workflows remain
open. Semantic snapshot restoration plus the desktop history checks do not
establish full production assembly qualification.

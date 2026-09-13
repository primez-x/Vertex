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
JSON and resolved values are detached copies. Catalogs with a material color use
`sketch.assemblies.v2`; uncolored catalogs continue to emit v1. In v2, a material
may have `color_srgb`, a `#RRGGBB` string (case-preserving, opaque sRGB). Invalid
colors and appearance fields in v1 are rejected. Removing every color allows v1
encoding again. Existing v1 projects load without inventing appearance values.

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
slabs, roofs, stairs, railings, columns, and beams support this binding. Document validation
rejects unsupported versions, unsupported object roles, and missing targets.
Removing a referenced material or catalog requires detaching its users in the
same atomic command. Assignment and removal use normal revision-checked history
and survive project save/reopen. This does not bind objects to assembly instances.

An instance may also carry an explicit `placement` in assembly schema v3. The
placement stores a host entity ID, model-space X/Y translation, rotation in
radians, and a positive uniform scale. Document validation treats the host as a
typed nested reference and accepts only geometry-bearing architectural objects;
removing or changing a host is therefore blocked by the same dangling-reference
rules as ordinary entity links. The Windows plan canvas renders a retained
transformed copy of the host boundary, and the same value feeds print and image
output. Clearing placement returns the catalog to v1/v2-compatible encoding.

Copy captures the referenced material subset with its names and colors, omitting
unrelated catalog entries, types, and instances. Paste uses the payload's root
identity independently of the destination selection. It maps catalog entity IDs
while preserving material IDs and names in their separate catalog namespace.
An existing catalog with the same identity is reused only when all copied
materials still match; otherwise a new local catalog preserves the captured
values. Catalog dependencies and geometry enter the document in one undoable
command. Cut/delete act on geometry and do not remove shared catalogs. Older
clipboard v1 payloads without `root_id` use their first entity as the root.

Material schedules resolve the current catalog name and report one assigned
object per row. Names and counts are read-only with source provenance; renaming
the catalog material updates derived rows. Visibility filtering applies to the
object, even if the catalog itself is outside the view. Assigned objects do not
reuse legacy authored `volume_m3` as a computed material quantity. Unassigned
legacy material rows retain their existing authored name/volume behavior.
The architectural schedule additionally measures net solid volume for assigned
walls, slabs, columns, beams, stairs, railings, and roofs. It uses the same document decoders
and solid builders as the native view, including wall openings, slab holes, and
roof cuts. Composite wall and horizontal slab assemblies add one material row per
assigned layer, including its authored thickness and net layer volume. A hidden hosted opening still cuts its wall. The volume cell is
read-only and identifies the source object and its hosted openings. Invalid or
unsupported solids leave volume absent with an explicit diagnostic. Rooms and
opening objects currently have no material solid volume.

This quantity treats each assigned object as one homogeneous material when it has
no layers. Layered walls and slabs use one measured solid per layer, with shared
openings or holes applied to each layer. It does not subtract intersections with
other objects, add waste, or infer quantities for assembly instances. The Materials tab edits names and
colors by stable material ID; clearing the color uses the object's default
appearance. Assigned solid objects use that color in the native 3D view. Catalog
color changes refresh cached presentations without rebuilding unchanged solids,
and normal undo/redo restores the appearance. Colors are converted from sRGB by
the rendering engine. Textures, transparency, roughness, and physical properties
remain open; a surface color alone is not a physically specified material.

Assembly geometry bindings and placement, composite material takeoff, nested
Placed assembly instances now have a read-only Assembly schedule row exposing
the resolved type, host, transform, material slots, and declared quantities
(including count, length/area/volume, and mass units). Scoped schedules follow
the placed host's visibility. The geometric binding is a deterministic
transformed copy of the host solid. Placed instances now project through the
same plan, elevation, and section view adapters as their host, while retaining
the host as the source of truth. Independent assembly profiles, nested
assemblies, composite material takeoff, material physical properties, and
publication workflows remain open. Semantic snapshot
restoration plus the desktop history checks do not establish full production
assembly qualification.

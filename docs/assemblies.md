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

This is not a complete architecture workspace feature. Document persistence,
entity or geometry bindings, user-visible override editing, command history and
undo UI, quantity-engine/schedule integration, nested assemblies, placement,
material physical properties, and publication workflows remain open. Semantic
snapshot restoration does not establish user-observed application undo behavior.

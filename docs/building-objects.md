# Parametric building solids

`sketch/building_objects.hpp` contains the semantic parameters and derived
Open CASCADE Technology (OCCT) solids for a small architectural-object core.
All distances are metres and all angles are radians.  The input structs are
the authoritative parameters; a `TopoDS_Shape` returned by a builder is a
derived geometry cache and must not replace those parameters in a document.

Every public builder rejects non-finite coordinates, angles, or dimensions,
dimensions outside the supported range, degenerate frames, and a result that
OCCT cannot validate as a non-zero-volume solid.  The current conservative
limits are coordinates within 1,000,000,000 metres, individual dimensions up
to 1,000,000 metres, and at most 10,000 stair risers.  These limits protect
the modeling kernel from accidental numeric explosions; they are not design
or code requirements.

`make_rectangular_column` makes a vertical oriented box centered on
`base_center` in plan.  `width` and `depth` follow the local axes after
`rotation_radians`, and `height` extends along global +Z.  The circular form
uses the same base-center convention with an exact OCCT cylinder.

`make_beam` makes a rectangular prism from the finite `start` to `end`
segment.  `up` is projected onto the plane normal to that segment and controls
the section depth direction.  The section width direction is chosen so that
width, depth, and the axis form a right-handed local frame.  A zero-length
axis, zero-length up vector, or up vector parallel to the axis is rejected.

`make_stair_flight` makes one connected stepped side profile extruded across
`width`.  `base_position` is the lower front-left corner of the first tread;
the run is rotated counter-clockwise from +X by `orientation_radians` and the
width extends to its left.  `riser_count` is explicit, `total_rise` is divided
evenly among the risers, and `going` is the horizontal depth of each step.
The optional `top_landing` is a real slab beginning at the end of the flight.
Its lower face is `total_rise - thickness` and its upper face is exactly the
finished flight elevation, so its added volume is
`width * depth * thickness`.  Landing thickness cannot extend below the
flight base.

`make_sloped_roof_panel` makes a rectangular planar prism.  `base_position` is
the lower-left corner of the un-overhung horizontal footprint at eave
elevation.  `run` rises by `rise` in the local run direction, `span` is
transverse, and `pitch_radians` must satisfy
`rise = run * tan(pitch_radians)`.  `overhang` extends the plane horizontally
on all four footprint edges, including the sloped run ends.  Thickness is
measured normal to the panel.  A flat roof is represented by this same typed
form with exactly `rise = 0` and `pitch_radians = 0`; its footprint remains a
real, thickened solid and is accepted by the editor, codec, plan projection,
and building-view projections.  A nonzero rise paired with zero pitch (or a
nonzero pitch paired with zero rise) remains invalid.

`make_gable_roof` makes two symmetric planar panel solids from a rectangular
footprint.  `base_position` is the footprint center at eave elevation,
`length` follows the ridge, and `span` is the full eave-to-eave distance.
`rise` and `pitch_radians` describe one half-span, so
`rise = (span / 2) * tan(pitch_radians)`.  The slope is extrapolated through
the overhang.  Each panel is vertically clipped at the ridge before applying
its normal thickness offset.  The result is a valid compound of two closed
solids sharing the ridge face with zero positive-volume overlap; this keeps
compound volume calculations from double-counting the normal-offset region.
The implementation requires the inward normal shift to remain inside each
half-span domain.  A ridge cap, fascia, framing, sheathing joints, flashing,
drainage, structural analysis, code checks, and material assemblies are
outside this bounded geometry package.

These functions construct deterministic geometric primitives and do not claim
production-complete roof or stair authoring, regulatory compliance, detailing,
or structural suitability.  Higher-level document commands, stable IDs,
materials, schedules, linked views, and persistence remain separate layers.

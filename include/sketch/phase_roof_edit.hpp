#pragma once

#include "sketch/phase_roof_form_edit.hpp"
#include "sketch/phase_roof_opening_edit.hpp"
#include "sketch/phase_roof_pose_edit.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/phase_roof_transform.hpp"
#include "sketch/phase_roof_uniform_transform.hpp"

namespace sketch {

// One roof edit can change its envelope, openings and placement together.
// Every component binds the same actual source owner; component staging never
// substitutes an intermediate roof for the captured source.
struct RoofEditIntent {
    std::string roof_id;
    std::optional<RoofProfileEditIntent> profile;
    std::optional<RoofOpeningEditIntent> openings;
    std::optional<RoofPoseEditIntent> pose;
    std::optional<RoofFormEditIntent> form;
    std::optional<RoofRigidTransformIntent> transform;
    std::optional<RoofPlanResizeIntent> resize;
    std::optional<RoofUniformTransformIntent> uniform_transform;
    // Explicit new replay authority; historical v1-v5 never move catalogs.
    bool coordinate_world_hosted_geometry{false};
};

// Strict version one: version, roof_id, profile, openings, pose. At least one
// nonnull typed component is required; all component owner IDs must agree.
// Conversion uses strict version two: those five fields plus nonnull form,
// with profile null. Historical same-form edits retain the exact v1 wire.
// Rigid movement uses strict version three with form and transform fields;
// transform is exclusive and reads the actual mathematical operation.
// Plan resize uses strict version four with an additional resize field;
// resize is exclusive and derives native footprint dimensions from source.
// Uniform XYZ scaling uses strict version five: the eight v4 fields plus
// uniform_transform, exclusively nonnull. Actual-map replay supplies its datum.
// Hosted coordination opts into strict version six: all nine v5 keys plus
// coordinate_world_hosted_geometry:true. Component combinations retain v1-v5
// exclusivity and require an actual component. Only actual-map replay may
// coordinate affected catalogs, retaining untouched rows. Composite/plan-resize
// edits keep type-owned profile dimensions and fixed local offsets from the
// roof base, following only its admitted base/yaw change. Form/pitch changes
// do not infer roof-surface anchoring. Explicit uniform transforms still scale.
// Skylight roster edits use strict version seven with the same ten keys as
// version six and a boolean coordination flag. Its openings component must be
// version two; historical composite versions cannot admit that new authority.
// Opening transfer uses strict version eight with those same ten fields and a
// version-three openings component. Earlier composites cannot admit transfers.
// Rotation uses strict version nine with the same ten fields and a required
// version-four openings component. Earlier composites cannot borrow angles.
[[nodiscard]] nlohmann::json encode_roof_edit_intent(const RoofEditIntent& intent);
[[nodiscard]] RoofEditIntent decode_roof_edit_intent(const nlohmann::json& value);

// Source-admitted component deltas compose before final native admission.
// A resize and cut move/removal therefore need not fit a transient envelope.
// Single-owner replay refuses hosted coordination. Opted-in actual-map replay
// returns roof and catalog consequences together without changing inventory.
[[nodiscard]] Entity replay_roof_edit_entity(const Entity& source, const RoofEditIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_edit_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofEditIntent>& intents);

// Inference is accepted only after the complete candidate equals independent
// typed replay, including retained opaque values and exact receipt metadata.
[[nodiscard]] std::optional<RoofEditIntent> capture_roof_edit(
    const Entity& original, const Entity& candidate);

[[nodiscard]] std::vector<RoofOpeningEditIntent> roof_edit_opening_intents(
    const std::vector<RoofEditIntent>& intents);

} // namespace sketch

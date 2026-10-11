#pragma once

#include "sketch/document.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace sketch {

// Closed v1/v2/v3 source-producer request. Exact v1/v2 envelope fields are version,
// expected_revision, transaction_id, architectural, embedded, annotations,
// references and message. Each target carries its own explicit transform:
// architectural {entity_id,transform}, embedded {catalog_id,instance_id,transform},
// annotations {owner_id,child_id,transform}, references {entity_id,transform}.
// Architectural/embedded transforms have pivot/offset XYZ arrays,
// rotation_z_radians, positive scale and both flip flags. Presentation transforms
// have pivot/offset XY arrays, rotation_radians and both flip flags; no scale.
// Version two additionally requires a positive scale in each presentation
// transform, with source-relative uniform size and common-pivot anchor motion.
// Version one retains its rigid-only exact fields and original meaning.
// Version three inherits version two's presentation fields and adds geometry:
// null or {request,room_reviews}. request is the exact canonical typed v1
// selection-geometry request, with enclosing revision/message and sorted roots;
// room_reviews contains at most 32 exact canonical physical-wall-room intents.
// Explicit geometry roots share the aggregate target limit and cannot overlap
// any persisted object, catalog, annotation owner or reference owner. Geometry
// currently admits rigid XY operators only; scale is not a geometry field.
// Canonicalization sorts structural target identities and retains losslessly
// admitted numeric representations. There are 1..1000 total targets, with a
// conservative 4 MiB compact-wire bound, 100000 values and maximum depth 64.
// The nonempty UTF-8 action message is bounded to 1024 bytes.
// Raw candidate entities, paths, copy IDs and producer/render aliases confer no
// authority. Properties and phase replacement are not lanes. Walls/boundaries
// require version three's typed geometry and complete explicit room review.
[[nodiscard]] nlohmann::json validate_ordinary_selection_edit_request(
    const nlohmann::json& request);

// Replays every nonempty producer against the same editable original snapshot,
// independently admits its complete map, composes those maps, and independently
// admits the final exact raw difference. No source/live document is mutated and
// no identities are generated. Identity still validates all requested targets.
// The caller owns captured-source, profile provenance and compatible stored-frame
// selection authority; a matching revision alone is not a publication fence.
// This historical raw-command API accepts v1/v2 only: raw entity changes cannot
// carry version three's typed geometry/room-review authority.
[[nodiscard]] ApplyEntityChanges replay_ordinary_selection_edit(
    const DocumentSnapshot& source, const nlohmann::json& request);

// Returns the complete independently derived candidate map without publication
// authority. v1/v2 reuse the historical fully admitted replay. v3 replays all
// nongeometry producers and typed reviewed geometry against the same original
// snapshot, then composes their complete maps through the reviewed composer.
// Authenticating the editable source and revision is required even for identity.
// The enclosing command still owns final admission and room identity lifetimes.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_ordinary_selection_edit_candidate(
    const DocumentSnapshot& source, const nlohmann::json& request);

// Validates the closed request and returns v3's canonical room review intents in
// their declared replay order, or an empty vector for v1/v2 or null geometry.
[[nodiscard]] std::vector<nlohmann::json> ordinary_selection_edit_room_reviews(
    const nlohmann::json& request);

// Sorted closed structural roster: {kind:"object",entity_id},
// {kind:"embedded",catalog_id,instance_id},
// {kind:"annotation",owner_id,child_id}, {kind:"reference",entity_id}.
// Version three also includes {kind:"geometry",entity_id} for explicit roots.
// This validates the request shape, not source target existence or provenance.
[[nodiscard]] nlohmann::json ordinary_selection_edit_semantic_targets(
    const nlohmann::json& request);

} // namespace sketch

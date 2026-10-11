#pragma once

#include "sketch/document.hpp"

#include <nlohmann/json.hpp>

namespace sketch {

// Closed v1/v2 source-producer request. Exact envelope fields are version,
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
// Canonicalization sorts structural target identities and retains losslessly
// admitted numeric representations. There are 1..1000 total targets, with a
// conservative 4 MiB compact-wire bound, 100000 values and maximum depth 64.
// The nonempty UTF-8 action message is bounded to 1024 bytes.
// Raw candidate entities, paths, copy IDs and producer/render aliases confer no
// authority. Walls, boundaries, Properties and phase replacement are not lanes.
[[nodiscard]] nlohmann::json validate_ordinary_selection_edit_request(
    const nlohmann::json& request);

// Replays every nonempty producer against the same editable original snapshot,
// independently admits its complete map, composes those maps, and independently
// admits the final exact raw difference. No source/live document is mutated and
// no identities are generated. Identity still validates all requested targets.
// The caller owns captured-source, profile provenance and compatible stored-frame
// selection authority; a matching revision alone is not a publication fence.
[[nodiscard]] ApplyEntityChanges replay_ordinary_selection_edit(
    const DocumentSnapshot& source, const nlohmann::json& request);

// Sorted closed structural roster: {kind:"object",entity_id},
// {kind:"embedded",catalog_id,instance_id},
// {kind:"annotation",owner_id,child_id}, {kind:"reference",entity_id}.
// This validates the request shape, not source target existence or provenance.
[[nodiscard]] nlohmann::json ordinary_selection_edit_semantic_targets(
    const nlohmann::json& request);

} // namespace sketch

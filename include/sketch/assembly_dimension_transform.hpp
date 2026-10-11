#pragma once

#include "sketch/assembly_dimension.hpp"

namespace sketch {

enum class AssemblyDimensionTransformMode { move, clone };

// Pure dimension contribution for an already derived component transform or
// clone. Both maps must be complete actual maps captured/derived by the caller;
// neither map nor this result establishes command, phase or publication authority.
// Roots are exact catalog/instance/document identities with empty part paths,
// and must retain their embedded/independent form. Move requires equal roots
// and no identity mappings. Clone requires distinct roots, a destination absent
// from source, and the original component row retained exactly in candidate.
// Candidate must still contain every associated original dimension unchanged.
//
// Only supported v6 assembly extents on the exact source root are carried.
// Nested paths and axes stay exact. Each complete map is measured in one batch
// of at most 1000 targets; retired/nonmeasurable paths refuse. Saved XY text and
// its direction follow the admitted selected-node authored similarity frames.
// Independent Site presentation is not baked into saved authored coordinates;
// embedded selected-node frames already express their world-authored placement.
// Text height and all other presentation/context/opaque fields are retained.
// Move returns changed existing dimension entities only; clone returns all new
// dimension entities in source-ID order while preserving every original.
// An exact original-dimension-ID to fresh-document-ID mapping is mandatory for
// clone, including nested dimensions; missing/extraneous/duplicate/colliding
// IDs refuse. IDs are supplied by the caller, never allocated here.
//
// Complete raw envelopes and catalog work are bounded before decoding/copying.
// An empty associated roster returns empty after root framing/clone retention
// checks without geometry expansion. Unsupported and older dimension semantics
// remain untouched. Invalid input/arithmetic throws std::invalid_argument.
//
// Ordinary callers must independently admit active/editable source and host
// dependencies, derive the actual rigid/similarity component edit, reserve fresh
// IDs through retained history, register copies, and publish atomically with
// this contribution. Phase callers additionally establish their exact baseline
// replacement/alternative permissions and coordinated references themselves.
// This helper infers none of those permissions and creates no Document/snapshot.
[[nodiscard]] std::vector<Entity> carry_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source,
    const AssemblyDocumentEntities& candidate,
    const AssemblyInstancePropertyEditTarget& source_root,
    const AssemblyInstancePropertyEditTarget& destination_root,
    AssemblyDimensionTransformMode mode,
    const std::map<std::string, std::string, std::less<>>& fresh_dimension_ids = {});

} // namespace sketch

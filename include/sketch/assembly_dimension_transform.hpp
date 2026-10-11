#pragma once

#include "sketch/assembly_dimension.hpp"

#include <span>

namespace sketch {

enum class AssemblyDimensionTransformMode { move, clone };

struct AssemblyDimensionTransformRequest {
    AssemblyInstancePropertyEditTarget source_root;
    AssemblyInstancePropertyEditTarget destination_root;
    AssemblyDimensionTransformMode mode{AssemblyDimensionTransformMode::move};
    std::map<std::string, std::string, std::less<>> fresh_dimension_ids;
};

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

// Atomic contribution for 1..1000 requests under the same contracts above.
// Exact source roots and destination roots are each unique; a destination may
// overlap a source only for that request's move. Fresh dimension IDs are unique
// across the entire batch. Both complete maps and all output carriers share one
// raw/catalog admission, and at most 1000 associated dimensions share one
// source and one candidate measurement batch. Output is in source-dimension-ID
// order, independent of request order; no partial contribution is returned.
[[nodiscard]] std::vector<Entity> carry_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source,
    const AssemblyDocumentEntities& candidate,
    std::span<const AssemblyDimensionTransformRequest> requests);

// Separate current ordinary-authoring policy check for existing dimensions
// actually changed or removed by a caller. Empty input is a no-op; otherwise
// IDs must be unique supported v6 source dimensions (at most 1000). Inactive
// dimensions and baseline dimensions protected by a saved active alternative
// require their dedicated replacement lane. Pure carry does not invoke this:
// phase producers and staging for fresh clones retain their own authority.
void validate_ordinary_assembly_dimension_changes(
    const AssemblyDocumentEntities& source, std::span<const std::string> dimension_ids);

} // namespace sketch

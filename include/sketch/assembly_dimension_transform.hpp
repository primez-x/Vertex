#pragma once

#include "sketch/assembly_dimension.hpp"
#include "sketch/slab_geometry_edit.hpp"

#include <cstddef>
#include <span>

namespace sketch {

enum class AssemblyDimensionTransformMode { move, clone, detached_clone };

struct AssemblyDimensionTransformRequest {
    AssemblyInstancePropertyEditTarget source_root;
    AssemblyInstancePropertyEditTarget destination_root;
    AssemblyDimensionTransformMode mode{AssemblyDimensionTransformMode::move};
    std::map<std::string, std::string, std::less<>> fresh_dimension_ids;
};

struct AssemblyDimensionReviewedTransformRequest {
    AssemblyDimensionTransformRequest transform;
    // Sorted unique exact source nodes, including their complete part paths.
    std::vector<AssemblyInstancePropertyEditTarget> reviewed_profile_nodes;
    // Every v7 dimension on those nodes requires an explicit decision. Null
    // retires its contribution; a value retains/remaps its own profile selector.
    std::map<std::string, std::optional<AssemblyProfileDimensionTarget>, std::less<>>
        profile_dimension_decisions;
};

struct AssemblyDimensionReviewedCarryResult {
    std::vector<Entity> upserts;
    // Actual source removals for move requests only. Clone retirements preserve
    // their originals and contribute neither an upsert nor a removal ID.
    std::vector<std::string> retired_source_dimension_ids;
};

// Source-only inventory for exact embedded catalog/instance roots (at most
// 4096). Complete raw/catalog admission precedes discovery; affected malformed,
// unsupported or future targets refuse. Independent owners remain untouched.
// Returns supported v6/v7 dimension IDs and their complete source component target,
// including nested paths, in ascending ID order. At most 1000 associations share
// one actual-source measurement batch. This grants no clone or phase authority.
[[nodiscard]] std::map<std::string, AssemblyInstancePropertyEditTarget, std::less<>>
inspect_embedded_assembly_dimension_clone_roster(
    const AssemblyDocumentEntities& source,
    std::span<const std::pair<std::string, std::string>> roots);

// For an already discovered supported embedded v6 extent or v7 profile. Releases its
// understood qualified target fields for an enclosing dependency scan; opaque
// property/extension siblings remain exact and must still be checked by callers.
[[nodiscard]] Entity assembly_dimension_clone_dependency_remainder(const Entity&);

// Pure dimension contribution for an already derived component transform or
// clone. Both maps must be complete actual maps captured/derived by the caller;
// neither map nor this result establishes command, phase or publication authority.
// Roots are exact catalog/instance/document identities with empty part paths,
// and retain their embedded/independent form for move/clone. Move requires equal roots
// and no identity mappings. Clone requires distinct roots, a destination absent
// from source, and the original component row retained exactly in candidate.
// Detached clone alone admits an embedded source and fresh independent owner
// in the same catalog, with that entire original catalog retained exactly.
// Candidate must still contain every associated original dimension unchanged.
//
// Supported v6 extents and v7 profile dimensions on the exact root are carried.
// Nested paths, axes and local topology selectors stay exact. Each complete map is measured in one batch
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

// Separate opt-in topology review contribution; historical carry overloads
// retain their exact-selector contracts. Each reviewed node belongs to its
// request's source root, with 1..1000 reviewed nodes across the batch. Decisions
// cover exactly all associated v7 dimensions on those nodes, preserving their
// measurement kind and component/path. Explicit unchanged selectors are valid;
// surviving selectors must resolve in the complete final body candidate.
// Other v6/v7 targets keep their original selectors. The complete association
// roster, including retirements, is bounded to 1000 and measured in source.
// Retired destination nodes retain their actual root/path/type and valid pose
// without requiring the removed selector to measure. Original dimensions must
// still be present unchanged in candidate before this contribution is applied.
// Move callers remove returned retired source IDs at atomic publication. Clone
// callers preserve those originals, receive no removal ID and allocate no clone for a retirement;
// fresh_dimension_ids covers exactly surviving associated dimensions. Upserts
// and retired IDs are in source-ID order, with no partial result on refusal.
// This supplies no geometry-edit authority: enclosing reviewed property/phase
// producers must bind these nodes to their explicit topology-edit authority.
[[nodiscard]] AssemblyDimensionReviewedCarryResult
carry_reviewed_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source,
    const AssemblyDocumentEntities& candidate,
    std::span<const AssemblyDimensionReviewedTransformRequest> requests);

// Pure contribution for embedded roots hosted by an already admitted physical
// dependency roster in a complete, already derived body candidate. Host IDs are
// unique (at most 100000) and must retain their exact persisted identity and
// supported wall/column/beam/slab/roof/stair/railing type in both maps. The caller
// supplies the complete actual dependency roster, including attached rail hosts
// when applicable; this helper establishes no host-edit or phase authority.
// Only dimension-bearing embedded roots whose original placement names a supplied
// host participate. Their qualified identity, host binding and every row field
// outside placement stay exact; independent owners are untouched. Unchanged raw
// placement remains eligible because host-derived component frames can change.
// Complete raw maps/catalog work are bounded before discovery. At most 1000
// associated v6/v7 dimensions share the existing carry's single source/candidate
// measurement batches and envelope preservation. Empty association returns empty
// after host/raw framing. Ordinary policy and publication remain the caller's job.
[[nodiscard]] std::vector<Entity> carry_hosted_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source,
    const AssemblyDocumentEntities& candidate,
    std::span<const std::string> actual_host_ids);

// Same contribution with a caller's remaining shared association budget
// (0..1000). Every associated target counts before measurement, including
// unchanged frames that produce no entity upsert. The three-argument overload
// retains its historical 1000-target bound.
[[nodiscard]] std::vector<Entity> carry_hosted_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source,
    const AssemblyDocumentEntities& candidate,
    std::span<const std::string> actual_host_ids,
    std::size_t maximum_associated_dimensions);

// Separate opt-in contribution for actual slab edits containing at least one
// coordinated v4 directional resize. The 1..4096 typed intents are replayed
// together once from source with hosted geometry before association. Replay
// authenticates unique actual targets, including unit directional members;
// every candidate target host must match that independently derived entity.
// Only changed target hosts and authenticated v4 unit hosts supply the roster.
// Dimension-bearing original embedded roots retain their exact qualified row
// from replay, including host binding, profiles, topology and poses. Their
// catalog envelope, definitions/materials and ordered row identities also match
// replay; unrelated phase copies under other catalog identities remain allowed.
// Complete raw maps/catalog work share admission before target indexing. At
// most maximum_associated_dimensions (0..1000) targets use the existing carry's
// single source/candidate measurement batches with unchanged path/selectors and
// original dimension envelopes. No fresh topology, remapping, Properties or
// phase authority is inferred. Unit/no-association work returns no upserts after
// replay and framing; associated unit targets still consume the shared budget.
// Historical hosted overloads retain their original placement-only contracts.
[[nodiscard]] std::vector<Entity> carry_hosted_assembly_directional_resize_dimension_entities(
    const AssemblyDocumentEntities& source,
    const AssemblyDocumentEntities& candidate,
    std::span<const SlabGeometryEditIntent> actual_intents,
    std::size_t maximum_associated_dimensions = 1000);

// Same bounded contribution for actual wall/opening hosts after an admitted
// wall edit. Openings may retain raw local station fields while their parent
// wall changes, so the caller supplies that complete physical dependency roster.
// Historical hosted overloads retain their original supported host families.
[[nodiscard]] std::vector<Entity> carry_wall_hosted_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source,
    const AssemblyDocumentEntities& candidate,
    std::span<const std::string> actual_host_ids);

// Separate current ordinary-authoring policy check for existing dimensions
// actually changed or removed by a caller. Empty input is a no-op; otherwise
// IDs must be unique supported v6/v7 source dimensions (at most 1000). Inactive
// dimensions and baseline dimensions protected by a saved active alternative
// require their dedicated replacement lane. Pure carry does not invoke this:
// phase producers and staging for fresh clones retain their own authority.
void validate_ordinary_assembly_dimension_changes(
    const AssemblyDocumentEntities& source, std::span<const std::string> dimension_ids);

} // namespace sketch

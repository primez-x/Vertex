#pragma once

#include "sketch/boundary_authoring_session.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace sketch {

// A replay result is the recomputed construction authority for an accepted
// chain. Its boundary geometry is rebuilt from normalized receipts and
// captured construction context; the source Segment values are never used as
// inputs. Dimensions are retained only after their stable references and
// placement semantics have been validated against the rebuilt boundary.
struct BoundaryConstructionReplay {
    Vec2 anchor{};
    IdentifiedBoundary boundary;
    std::string classification;
    std::vector<BoundaryDimension> dimensions;
    std::vector<ConstructionReceipt> receipts;
    bool classified{};

    bool operator==(const BoundaryConstructionReplay& other) const noexcept;
};

// Rebuilds every edge in source order from its receipt. This validates exact
// Quantity/AngleInput representations, captured starts/chord endpoints,
// stable topology, strict closure and dimension references. Invalid or
// inconsistent source data throws std::invalid_argument without returning a
// partial replay. The caller should pass the immutable options captured with
// the session; the default is suitable for the default boundary type.
[[nodiscard]] BoundaryConstructionReplay replay_accepted_chain(
    const AcceptedBoundaryChain& source,
    const BoundaryAuthoringOptions& options = {});

// Performs replay and requires the source's displayed geometry, dimensions and
// receipts to equal the authoritative result. The function throws on any
// mismatch and never mutates the source.
void verify_accepted_chain(const AcceptedBoundaryChain& source,
                           const BoundaryAuthoringOptions& options = {});

// Verifies the accepted chain and returns the strict lower-layer
// boundary_authoring envelope for an identified-boundary Document adapter.
// The adapter owns the Entity/property placement; this function does not
// mutate a Document or infer geometry from a canvas representation.
[[nodiscard]] nlohmann::json boundary_construction_envelope(
    const AcceptedBoundaryChain& source,
    const BoundaryAuthoringOptions& options = {});

}  // namespace sketch

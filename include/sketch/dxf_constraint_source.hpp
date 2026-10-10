#pragma once

#include "sketch/document.hpp"

#include <map>
#include <string_view>
#include <utility>

namespace sketch {
struct NativeDxfWallSourceWorkBudget;

using NativeDxfConstraintOwnerMap = std::map<std::string, std::string, std::less<>>;
// The pair names an original document owner and an element inside that owner.
// Segment and vertex namespaces are separate, even when their spelling agrees.
using NativeDxfConstraintElementMap = std::map<std::pair<std::string, std::string>, std::string>;

[[nodiscard]] bool native_dxf_constraint_source_type(std::string_view type) noexcept;
// Supported persisted relations only. Unknown versions/relations refuse;
// metadata never supplies an invented owner or an inferred solver identity.
// Values are exact required owner types where known (baseline/wall_ids: wall),
// otherwise empty for the supported identified-boundary/measured-stroke family.
[[nodiscard]] NativeDxfConstraintOwnerMap native_dxf_constraint_source_dependencies(
    const Entity& source, NativeDxfWallSourceWorkBudget* work_budget = nullptr);

// Rewrites a private raw copy, retaining Entity.id, quantities, numeric forms,
// binding order, extensions and unrelated fields. Every document dependency
// needs an injective owner map. Empty element maps retain owner-local IDs;
// a nonempty map must cover every referenced element in its own namespace and
// be injective within each original owner. Extra map entries are ignored.
// Only the codec's authoritative owner inventory is sorted after substitution.
// Caller owns Entity.id/top-level context remapping and complete graph proof.
[[nodiscard]] Entity remap_native_dxf_constraint_source_dependencies(
    const Entity& source, const NativeDxfConstraintOwnerMap& owner_ids,
    const NativeDxfConstraintElementMap& segment_ids = {},
    const NativeDxfConstraintElementMap& vertex_ids = {},
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);

// Raw admission and conservative repeated model/typed endpoint replay charges
// precede codecs. Uses the shared catalog-transfer ledger; failed work remains
// charged. This alone does not establish graph semantics or native evidence.
void admit_native_dxf_constraint_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget& budget);
// Proves actual raw source parity, binding owner types, endpoint identities and
// genuine arc/tangent structure, including retained inactive relationships.
// Relation satisfaction belongs to the complete graph validation below.
void validate_native_dxf_constraint_source(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
// Call once for the complete actual graph. Admits ambient document/model work,
// then invokes solver-free existing active-phase constraint integrity. Actual
// saved registries alone suspend inactive residuals; unknown semantics refuse.
void validate_native_dxf_constraint_source_graph(
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
} // namespace sketch

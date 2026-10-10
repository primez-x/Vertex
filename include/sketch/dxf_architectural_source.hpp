#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"
#include "sketch/project_organization.hpp"
#include <map>
#include <string_view>
#include <vector>

namespace sketch {
struct NativeDxfWallSourceWorkBudget;
// These additional authored families are carried only by the V8 source contract.
[[nodiscard]] bool native_dxf_architectural_source_type(std::string_view type) noexcept;
// Inventory contains document owners only. Local material/type/part and stair
// feature identities never enter the document owner namespace.
[[nodiscard]] std::vector<std::string> native_dxf_architectural_source_catalog_ids(const Entity& source);
[[nodiscard]] Entity remap_native_dxf_independent_assembly_source(const Entity& source,
    const std::map<std::string, std::string, std::less<>>& body_mapping,
    const std::map<std::string, std::string, std::less<>>& catalog_mapping);
[[nodiscard]] nlohmann::json native_dxf_architectural_source_dependencies(const Entity& source);
void remap_native_dxf_architectural_source_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& body_mapping);
void remap_native_dxf_architectural_source_context_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& context_mapping);
[[nodiscard]] std::vector<std::string> native_dxf_architectural_child_identity_ids(const Entity& source);
// Only a canonical straight-stair derived flight aliases a document body.
// The actual authored host proves that namespace before body dependencies move.
void remap_native_dxf_architectural_host_body_aliases(Entity& source, const Entity& authored_host,
    const std::map<std::string, std::string, std::less<>>& body_mapping);
void remap_native_dxf_architectural_child_identities(Entity& source,
    const std::map<std::string, std::string, std::less<>>& child_mapping,
    const Entity* authored_host = nullptr);
// Joins have closed properties and inherit only a proven common full context
// of their actual roof members. Conflicting contexts explicitly refuse.
[[nodiscard]] std::optional<DrawingContext> native_dxf_architectural_source_context(
    const Entity& source, const std::map<std::string, Entity, std::less<>>& authored,
    const ProjectOrganization& organization);
// Raw admission precedes codecs, placement, solid creation and projection.
// Attempts share the operation ledger and failed work remains charged.
void admit_native_dxf_architectural_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget& budget);
void validate_native_dxf_architectural_source(const Entity& source);
// Actual authored map is required for hosted railings, joins and assemblies.
[[nodiscard]] Boundary native_dxf_architectural_source_plan(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored);
} // namespace sketch

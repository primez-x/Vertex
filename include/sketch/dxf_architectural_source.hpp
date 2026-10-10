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

// Separate phase-authoring primitives. These do not expand the V8 family
// allowlist or admit a V9 transport envelope. Terrain point IDs are model-local.
[[nodiscard]] bool native_dxf_phase_auxiliary_source_type(std::string_view type) noexcept;
[[nodiscard]] std::vector<std::string> native_dxf_phase_auxiliary_source_dependencies(const Entity& source);
// Patches only typed wall_ids, preserving the raw owner, dialect and metadata.
// Complete actual source/destination maps must be validated around this pass.
void remap_native_dxf_phase_auxiliary_source_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& body_mapping);
[[nodiscard]] std::optional<DrawingContext> native_dxf_phase_auxiliary_source_context(
    const Entity& source, const std::map<std::string, Entity, std::less<>>& authored,
    const ProjectOrganization& organization);
// Call before validation/placement/codecs/projection, sharing architectural_work
// with the V8 helpers. Failed work stays charged to the operation ledger.
void admit_native_dxf_phase_auxiliary_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget& budget);
// authored is the complete actual graph; source is its candidate at source.id.
// Wall-member ownership is checked globally, including inactive joins.
void validate_native_dxf_phase_auxiliary_source(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored);
[[nodiscard]] Boundary native_dxf_phase_auxiliary_source_plan(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored);

// Corner aggregates use graph V5 inside the V9 complete-source carrier. They
// never enter the historical V8 family allowlist or single-host contracts.
// Owners return their four sorted host/cut IDs; marked opening children return
// their owner ID (their wall_id is handled by the ordinary opening contract).
// Either backlink marker requires a complete typed pair and a bare indexed cut.
[[nodiscard]] std::vector<std::string> native_dxf_corner_window_source_dependencies(const Entity& source);
// Changes only wall_ids/opening_ids or corner_window_id, retaining metadata and
// numeric representation. Owner identity is changed separately. Atomic on
// failure; the complete destination map/reservation is the caller's authority.
void remap_native_dxf_corner_window_source_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& body_mapping);
[[nodiscard]] std::optional<DrawingContext> native_dxf_corner_window_source_context(
    const Entity& source, const std::map<std::string, Entity, std::less<>>& authored,
    const ProjectOrganization& organization);
// Reserve raw complete-state, full host-roster and ownership/placement work
// before any corner codec or global phase replay. Native construction consumers
// additionally request native_geometry; structural/dimension passes do not
// charge native nonlinear work or projected primitives. No-op for non-owners;
// attempts share architectural_work and failed work stays charged.
void admit_native_dxf_corner_window_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget& budget, bool native_geometry = false);
// Actual map identity/envelope equality and global saved-state ownership are
// required, including inactive alternatives and every actual hosted opening.
void validate_native_dxf_corner_window_source(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored);
// Uses the actual active graph, resolved physical hosts, derived cuts and the
// native fused frame/two-pane factory. Without native geometry it refuses.
[[nodiscard]] Boundary native_dxf_corner_window_source_plan(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored);
} // namespace sketch

#pragma once

#include "sketch/assembly_document_adapter.hpp"

namespace sketch {

struct AssemblyClipboardDetachRequest {
    std::string catalog_id;
    std::string instance_id;
    std::string independent_id;
};
struct AssemblyClipboardDetachedCapture {
    std::vector<Entity> roots;
    AssemblyDocumentEntities catalogs;
    nlohmann::json provenance = nlohmann::json::array();
};

// Passive, immutable capture only. Declared rows and actual catalog/host closure
// must be active and agree on any actual registry. No companion is copied or enrolled.
// Supplied independent roots must be exact actual source entities.
[[nodiscard]] AssemblyClipboardDetachedCapture capture_assembly_component_clipboard(
    const DocumentSnapshot& source,
    const std::vector<AssemblyClipboardDetachRequest>& requests,
    const std::vector<Entity>& independent_roots = {});

// Closed bounded record shape and internal links only. Digests are passive
// provenance, never authentication of a live foreign source or phase authority.
void validate_assembly_component_clipboard_provenance(const nlohmann::json& records);
// Call with the complete wire graph (roots and minimal catalogs), before any
// destination translation/remapping. Authenticates normalized raw source rows
// against their independent wire roots, without admitting foreign phase claims.
void validate_assembly_component_clipboard_source_rows(
    const nlohmann::json& records, const std::vector<Entity>& wire_entities);

// Check mapped placement rows against actual prepared destinations/catalogs.
// Catalog/document IDs and root XY translation may differ; type, overrides,
// nested poses and every other root pose component must remain unchanged.
// Fallback supplies actual retained catalogs only, never foreign authority.
void validate_assembly_component_clipboard_destinations(
    const nlohmann::json& mapped_records, const AssemblyDocumentEntities& additions,
    const AssemblyDocumentEntities& fallback = {});

} // namespace sketch

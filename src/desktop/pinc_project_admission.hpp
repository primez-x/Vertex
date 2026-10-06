#pragma once

#include "pinc_import_worker.hpp"
#include "sketch/pinc_presentation_admission.hpp"
#include "sketch/sheet_view_model.hpp"

namespace sketch::desktop {

// Every page needs an explicit choice. Equal group numbers intentionally share
// one floor; their calculation and interior layers always remain separate.
struct PincPageFloorChoice {
    std::size_t page_index{};
    std::size_t floor_group{};
    std::string floor_name;
};

struct PincProjectAdmission {
    Document document;
    std::vector<PincPageGeometryContext> contexts;
    SheetViewModel sheets;
    std::size_t current_page{};
    std::vector<PincImportDiagnostic> diagnostics;
    std::string original_asset_id;
    std::string sheet_view_entity_id;
    // Empty entries identify pages without an admitted decoded underlay.
    std::vector<std::string> page_reference_ids;
};

// Builds an independent, detached candidate. Rechecks typed source against the
// exact original bytes and requires broker attestation. The source basename is
// portable provenance only. No path is opened and no image decoder is invoked.
// Namespace is 1..99 ASCII alphanumeric/underscore/hyphen bytes; internal
// geometry and presentation each use a separate derived fresh namespace.
// Trusted symbol bindings must come from the reviewed bundled catalog. All
// imported entities/assets enter one command against a generic scaffold, so one
// Undo removes imported content and one Redo restores it. Throws atomically.
[[nodiscard]] PincProjectAdmission preparePincProjectAdmission(
    const PincWorkerProject& worker, std::vector<std::byte> original_bytes,
    std::string source_basename, std::span<const PincPageFloorChoice> floor_choices,
    std::span<const PincSymbolBinding> trusted_symbol_bindings,
    std::string_view fresh_namespace);

} // namespace sketch::desktop

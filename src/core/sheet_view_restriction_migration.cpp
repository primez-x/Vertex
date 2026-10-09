#include "sketch/sheet_view_restriction_migration.hpp"

#include "sketch/sheet_view_entity_codec.hpp"

#include <stdexcept>

namespace sketch {

Entity upgrade_sheet_view_entity_for_empty_restriction(const Entity& source) {
    // The existing codec supplies the schema/version/shape and graph admission.
    // In particular, an affected future model may not borrow this migration.
    const auto before = decode_sheet_view_entity(source);
    const auto version = source.properties.at("model").at("version").get<unsigned>();
    if (version >= 6) return source;

    Entity upgraded = source;
    auto& model = upgraded.properties.at("model");
    if (version < 4) {
        // Before v4 the reader unconditionally derives lexical sheet order,
        // even if raw input carries a sheet_order field. Preserve that field
        // only when v6 would interpret it identically; never overwrite it.
        const nlohmann::json order = before.sheet_order();
        if (model.contains("sheet_order") && model.at("sheet_order") != order)
            throw std::invalid_argument("sheet/view v6 restriction migration cannot preserve legacy sheet_order meaning");
        if (!model.contains("sheet_order")) model["sheet_order"] = order;
    }
    for (auto& view : model.at("views")) {
        // These are precisely the missing-field defaults in
        // SheetViewModel::from_json, not a reserialization of its normalized
        // snapshot. Existing numeric values and collection order stay raw.
        if (version == 1 && !view.contains("object_ids"))
            view["object_ids"] = nlohmann::json::array();
        if (version < 3 && !view.contains("overlays"))
            view["overlays"] = nlohmann::json::array();
        auto& presentation = view.at("presentation");
        if (version < 5 && !presentation.contains("crop"))
            presentation["crop"] = nullptr;
        if (!view.contains("restrict_to_objects")) view["restrict_to_objects"] = false;
        for (auto& overlay : view.at("overlays"))
            if (!overlay.contains("dimension_binding")) overlay["dimension_binding"] = nullptr;
    }
    model["version"] = 6;

    // Prove all interpreted view, overlay, sheet, schedule and order semantics
    // match before the caller prunes any sources. The returned entity remains
    // the copied raw source with the additions above, never an encoded snapshot.
    const auto after = decode_sheet_view_entity(upgraded);
    if (before.views() != after.views() || before.sheets() != after.sheets() ||
        before.schedule_ids() != after.schedule_ids() || before.sheet_order() != after.sheet_order())
        throw std::invalid_argument("sheet/view v6 restriction migration changed legacy model meaning");
    return upgraded;
}

} // namespace sketch

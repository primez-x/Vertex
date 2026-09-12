#include "sketch/sheet_view_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}
void identifier(const std::string& value) {
    require(!value.empty() && !std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); }), "sheet/view identity must not be blank");
}
void positive(double value) {
    require(std::isfinite(value) && value > 0, "sheet/view dimension or scale must be finite and positive");
}
void nonnegative(double value) {
    require(std::isfinite(value) && value >= 0, "sheet/view coordinate or depth must be finite and nonnegative");
}
template<class T> std::set<std::string> canonical(std::vector<T>& values) {
    std::set<std::string> ids;
    for (const auto& value : values) {
        identifier(value.id);
        require(ids.insert(value.id).second, "duplicate sheet/view identity");
    }
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return ids;
}
void bounds(const SheetRect& rect, const DrawingSheet& sheet) {
    nonnegative(rect.x_mm); nonnegative(rect.y_mm);
    positive(rect.width_mm); positive(rect.height_mm);
    require(rect.x_mm <= sheet.width_mm && rect.y_mm <= sheet.height_mm &&
        rect.width_mm <= sheet.width_mm - rect.x_mm &&
        rect.height_mm <= sheet.height_mm - rect.y_mm, "sheet placement exceeds page bounds");
}
void validate_view(const CoordinatedView& view) {
    switch (view.kind) {
    case CoordinatedViewKind::plan: case CoordinatedViewKind::elevation: case CoordinatedViewKind::section: break;
    default: throw std::invalid_argument("unknown coordinated view kind");
    }
    const auto& p = view.presentation;
    switch (p.detail) {
    case ViewDetail::coarse: case ViewDetail::medium: case ViewDetail::fine: break;
    default: throw std::invalid_argument("unknown view detail level");
    }
    double dot = 0, direction_length = 0, up_length = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        require(std::isfinite(view.origin_m[i]) && std::isfinite(view.direction[i]) &&
            std::isfinite(view.up[i]), "view frame must be finite");
        dot += view.direction[i] * view.up[i];
        direction_length += view.direction[i] * view.direction[i];
        up_length += view.up[i] * view.up[i];
    }
    require(std::abs(direction_length - 1) <= 1e-9 && std::abs(up_length - 1) <= 1e-9 &&
        std::abs(dot) <= 1e-9, "view direction and up must be orthonormal");
    nonnegative(p.cut_depth_m); positive(p.far_depth_m);
    require(p.cut_depth_m <= p.far_depth_m, "cut depth exceeds far depth");
    positive(p.cut_line_mm); positive(p.projection_line_mm); positive(p.hatch_scale);
    identifier(p.hatch_pattern);
}
// Reject unknown keys and wrong container lengths, including excess frame values.
// Numeric scalar types are checked by JSON decoding and semantic validation.
void shape(const nlohmann::json& input, const nlohmann::json& output) {
    if (output.is_object()) {
        require(input.is_object() && input.size() == output.size(), "invalid sheet/view JSON fields");
        for (const auto& [key, child] : output.items()) {
            require(input.contains(key), "missing sheet/view JSON field");
            shape(input.at(key), child);
        }
    } else if (output.is_array()) {
        require(input.is_array() && input.size() == output.size(), "invalid sheet/view JSON array");
        // Canonical ordering may differ, so compare structures by item identity.
        for (std::size_t i = 0; i < output.size(); ++i) {
            if (output[i].is_object() && output[i].contains("id")) {
                const auto found = std::find_if(input.begin(), input.end(), [&](const auto& item) {
                    return item.is_object() && item.contains("id") && item.at("id") == output[i].at("id");
                });
                require(found != input.end(), "missing sheet/view JSON identity");
                shape(*found, output[i]);
            } else shape(input[i], output[i]);
        }
    } else if (output.is_number()) {
        require(input.is_number(), "sheet/view JSON numeric field has wrong type");
    } else if (output.is_boolean()) {
        require(input.is_boolean(), "sheet/view JSON boolean field has wrong type");
    } else if (output.is_string()) {
        require(input.is_string(), "sheet/view JSON string field has wrong type");
    }
}
} // namespace

void to_json(nlohmann::json& value, const CoordinatedViewKind& kind) {
    switch (kind) {
    case CoordinatedViewKind::plan: value = "plan"; return;
    case CoordinatedViewKind::elevation: value = "elevation"; return;
    case CoordinatedViewKind::section: value = "section"; return;
    }
    throw std::invalid_argument("unknown coordinated view kind");
}
void from_json(const nlohmann::json& value, CoordinatedViewKind& kind) {
    if (value == "plan") kind = CoordinatedViewKind::plan;
    else if (value == "elevation") kind = CoordinatedViewKind::elevation;
    else if (value == "section") kind = CoordinatedViewKind::section;
    else throw std::invalid_argument("unknown coordinated view kind");
}
void to_json(nlohmann::json& value, const ViewDetail& detail) {
    switch (detail) {
    case ViewDetail::coarse: value = "coarse"; return;
    case ViewDetail::medium: value = "medium"; return;
    case ViewDetail::fine: value = "fine"; return;
    }
    throw std::invalid_argument("unknown view detail level");
}
void from_json(const nlohmann::json& value, ViewDetail& detail) {
    if (value == "coarse") detail = ViewDetail::coarse;
    else if (value == "medium") detail = ViewDetail::medium;
    else if (value == "fine") detail = ViewDetail::fine;
    else throw std::invalid_argument("unknown view detail level");
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ViewPresentation, cut_depth_m, far_depth_m, cut_line_mm,
    projection_line_mm, hatch_enabled, hatch_pattern, hatch_scale, detail)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CoordinatedView, id, name, kind, origin_m, direction, up, presentation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SheetRect, x_mm, y_mm, width_mm, height_mm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SheetViewport, id, view_id, bounds, scale_denominator)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SheetTitleBlock, project, title, author, issue_date)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SheetRevision, id, date, description)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SheetCallout, id, label, target_sheet_id, target_viewport_id, x_mm, y_mm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SheetSchedulePlacement, id, schedule_id, bounds)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DrawingSheet, id, number, width_mm, height_mm, title_block,
    revisions, viewports, callouts, schedules)

SheetViewModel SheetViewModel::create(std::vector<CoordinatedView> views,
    std::vector<DrawingSheet> sheets, std::vector<std::string> schedule_ids) {
    const auto view_ids = canonical(views);
    (void)canonical(sheets);
    std::set<std::string> schedules;
    for (const auto& id : schedule_ids) {
        identifier(id);
        require(schedules.insert(id).second, "duplicate schedule identity");
    }
    std::sort(schedule_ids.begin(), schedule_ids.end());
    for (const auto& view : views) validate_view(view);
    std::set<std::string> numbers;
    for (auto& sheet : sheets) {
        identifier(sheet.number);
        require(numbers.insert(sheet.number).second, "duplicate sheet number");
        positive(sheet.width_mm); positive(sheet.height_mm);
        (void)canonical(sheet.revisions); (void)canonical(sheet.viewports);
        (void)canonical(sheet.callouts); (void)canonical(sheet.schedules);
        for (const auto& viewport : sheet.viewports) {
            require(view_ids.contains(viewport.view_id), "viewport references unknown view");
            positive(viewport.scale_denominator);
            bounds(viewport.bounds, sheet);
        }
        for (const auto& placement : sheet.schedules) {
            require(schedules.contains(placement.schedule_id), "placement references unknown schedule");
            bounds(placement.bounds, sheet);
        }
    }
    for (const auto& sheet : sheets) for (const auto& callout : sheet.callouts) {
        nonnegative(callout.x_mm); nonnegative(callout.y_mm);
        require(callout.x_mm <= sheet.width_mm && callout.y_mm <= sheet.height_mm,
            "callout exceeds page bounds");
        const auto target = std::find_if(sheets.begin(), sheets.end(),
            [&](const auto& candidate) { return candidate.id == callout.target_sheet_id; });
        require(target != sheets.end(), "callout references unknown sheet");
        require(std::any_of(target->viewports.begin(), target->viewports.end(),
            [&](const auto& viewport) { return viewport.id == callout.target_viewport_id; }),
            "callout references unknown viewport on target sheet");
    }
    SheetViewModel result;
    result.views_ = std::move(views); result.sheets_ = std::move(sheets);
    result.schedule_ids_ = std::move(schedule_ids);
    return result;
}

SheetViewModel SheetViewModel::with_view(CoordinatedView replacement) const {
    auto changed = views_;
    const auto found = std::find_if(changed.begin(), changed.end(),
        [&](const auto& view) { return view.id == replacement.id; });
    require(found != changed.end(), "cannot replace unknown coordinated view");
    *found = std::move(replacement);
    return create(std::move(changed), sheets_, schedule_ids_);
}

SheetViewModel SheetViewModel::with_sheet(DrawingSheet replacement) const {
    auto changed = sheets_;
    const auto found = std::find_if(changed.begin(), changed.end(),
        [&](const auto& sheet) { return sheet.id == replacement.id; });
    require(found != changed.end(), "cannot replace unknown drawing sheet");
    *found = std::move(replacement);
    return create(views_, std::move(changed), schedule_ids_);
}

SheetViewModel SheetViewModel::with_added_sheet(DrawingSheet addition) const {
    auto changed = sheets_;
    changed.push_back(std::move(addition));
    // create() performs the complete detached graph validation, including
    // identity/number uniqueness and references from the new page to views,
    // schedules, and other sheets.
    return create(views_, std::move(changed), schedule_ids_);
}

SheetViewModel SheetViewModel::with_removed_sheet(const std::string& sheet_id) const {
    require(sheets_.size() > 1, "cannot remove the only drawing sheet");
    auto changed = sheets_;
    const auto found = std::find_if(changed.begin(), changed.end(),
        [&](const auto& sheet) { return sheet.id == sheet_id; });
    require(found != changed.end(), "cannot remove unknown drawing sheet");
    changed.erase(found);
    // create() rejects any surviving callout that still targets the removed
    // sheet. Callouts owned by the removed page disappear with that page.
    return create(views_, std::move(changed), schedule_ids_);
}

SheetViewModel SheetViewModel::with_viewport(const std::string& sheet_id,
                                             SheetViewport replacement) const {
    auto changed = sheets_;
    const auto sheet = std::find_if(changed.begin(), changed.end(),
        [&](const auto& candidate) { return candidate.id == sheet_id; });
    require(sheet != changed.end(), "cannot edit viewport on unknown drawing sheet");
    const auto viewport = std::find_if(sheet->viewports.begin(), sheet->viewports.end(),
        [&](const auto& candidate) { return candidate.id == replacement.id; });
    require(viewport != sheet->viewports.end(), "cannot replace unknown sheet viewport");
    *viewport = std::move(replacement);
    return create(views_, std::move(changed), schedule_ids_);
}

SheetViewModel SheetViewModel::with_schedule_placement(
    const std::string& sheet_id, SheetSchedulePlacement replacement) const {
    auto changed = sheets_;
    const auto sheet = std::find_if(changed.begin(), changed.end(),
        [&](const auto& candidate) { return candidate.id == sheet_id; });
    require(sheet != changed.end(), "cannot edit schedule placement on unknown drawing sheet");
    const auto placement = std::find_if(sheet->schedules.begin(), sheet->schedules.end(),
        [&](const auto& candidate) { return candidate.id == replacement.id; });
    require(placement != sheet->schedules.end(), "cannot replace unknown schedule placement");
    *placement = std::move(replacement);
    return create(views_, std::move(changed), schedule_ids_);
}

nlohmann::json SheetViewModel::to_json() const {
    return {{"schema", "sketch.sheet_view_model"}, {"version", 1}, {"views", views_},
        {"sheets", sheets_}, {"schedule_ids", schedule_ids_}};
}
SheetViewModel SheetViewModel::from_json(const nlohmann::json& value) {
    try {
        require(value.at("schema") == "sketch.sheet_view_model" &&
            value.at("version").is_number_integer() && value.at("version") == 1,
            "unsupported sheet/view schema");
        auto result = create(value.at("views").get<std::vector<CoordinatedView>>(),
            value.at("sheets").get<std::vector<DrawingSheet>>(),
            value.at("schedule_ids").get<std::vector<std::string>>());
        shape(value, result.to_json());
        return result;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid sheet/view JSON: ") + error.what());
    }
}

} // namespace sketch

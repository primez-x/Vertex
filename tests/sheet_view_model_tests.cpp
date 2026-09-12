#include "sketch/sheet_view_model.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid sheet/view input accepted");
}
sketch::SheetViewModel fixture() {
    sketch::CoordinatedView plan{"plan", "Ground floor"};
    sketch::CoordinatedView elevation{"elevation", "South elevation", sketch::CoordinatedViewKind::elevation};
    elevation.direction = {0, 1, 0}; elevation.up = {0, 0, 1};
    sketch::CoordinatedView section{"section", "Section A", sketch::CoordinatedViewKind::section};
    section.direction = {1, 0, 0}; section.up = {0, 0, 1};
    sketch::DrawingSheet first;
    first.id = "a"; first.number = "A101";
    first.title_block = {"House", "Plans", "Designer", "2026-09-11"};
    first.revisions = {{"02", "2026-09-11", "Issue"}, {"01", "2026-09-10", "Draft"}};
    first.viewports = {{"plan-small", "plan", {10, 10, 180, 120}, 100},
                       {"plan-large", "plan", {200, 10, 180, 120}, 50}};
    first.callouts = {{"section-marker", "A / A201", "b", "section-main", 50, 150}};
    first.schedules = {{"doors-table", "doors", {10, 160, 180, 100}}};
    sketch::DrawingSheet second;
    second.id = "b"; second.number = "A201";
    second.width_mm = 841; second.height_mm = 594;
    second.viewports = {{"section-main", "section", {20, 20, 300, 200}, 25},
                        {"elevation-main", "elevation", {400, 20, 300, 200}, 100}};
    return sketch::SheetViewModel::create({section, plan, elevation}, {second, first}, {"rooms", "doors"});
}
void coordination_and_isolation() {
    const auto original = fixture();
    const auto saved = original.to_json();
    auto plan = original.views().at(1);
    require(plan.id == "plan", "views canonicalized by identity");
    plan.presentation.cut_depth_m = 1.5;
    plan.presentation.cut_line_mm = 0.7;
    plan.presentation.hatch_pattern = "concrete";
    plan.presentation.detail = sketch::ViewDetail::fine;
    const auto changed = original.with_view(plan);
    require(changed.views()[1] == plan, "shared view settings updated");
    require(changed.sheets() == original.sheets(), "view edit preserves placements, callouts and scales");
    const auto& placements = changed.sheets()[0].viewports;
    require(placements[0].scale_denominator == 50 && placements[1].scale_denominator == 100 &&
        placements[0].view_id == placements[1].view_id, "same view has independent sheet scales");
    require(original.to_json() == saved, "original snapshot immutable");
    auto sheet = original.sheets().front();
    sheet.title_block.title = "Issued plans";
    sheet.title_block.author = "Architect";
    const auto changed_sheet = original.with_sheet(sheet);
    require(changed_sheet.sheets().front().title_block.title == "Issued plans" &&
                changed_sheet.sheets().front().viewports == original.sheets().front().viewports,
            "sheet metadata edits must preserve coordinated viewport placement");
    require(original.to_json() == saved, "original sheet snapshot must remain immutable");
    auto viewport = original.sheets().front().viewports.front();
    viewport.bounds = {15, 15, 390, 267};
    viewport.scale_denominator = 75;
    const auto changed_viewport = original.with_viewport("a", viewport);
    require(changed_viewport.sheets().front().viewports.front() == viewport,
            "viewport bounds and scale should update through the typed model");
    require(changed_viewport.sheets().front().callouts == original.sheets().front().callouts &&
                changed_viewport.sheets().front().schedules == original.sheets().front().schedules,
            "viewport edits must preserve sheet references and placements");
    require(original.to_json() == saved, "original viewport snapshot must remain immutable");
    rejects([&] { (void)original.with_viewport("unknown", viewport); });
    viewport.id = "unknown";
    rejects([&] { (void)original.with_viewport("a", viewport); });
    auto placement = original.sheets().front().schedules.front();
    placement.bounds = {20, 160, 180, 100};
    const auto changed_placement = original.with_schedule_placement("a", placement);
    require(changed_placement.sheets().front().schedules.front() == placement,
            "schedule placement bounds should update through the typed model");
    require(changed_placement.sheets().front().viewports == original.sheets().front().viewports,
            "schedule placement edits must preserve viewports");
    rejects([&] { (void)original.with_schedule_placement("unknown", placement); });
    placement.id = "unknown";
    rejects([&] { (void)original.with_schedule_placement("a", placement); });
    sheet.id = "unknown";
    rejects([&] { (void)original.with_sheet(sheet); });
    plan.id = "unknown";
    rejects([&] { (void)original.with_view(plan); });
    auto views = original.views(); auto sheets = original.sheets();
    const auto detached = sketch::SheetViewModel::create(views, sheets, {"doors", "rooms"});
    views[0].id = "changed"; sheets[0].number = "changed";
    require(detached.to_json() == saved, "snapshot owns input values");
}
void sheet_lifecycle() {
    const auto original = fixture();
    auto addition = original.sheets().front();
    addition.id = "c";
    addition.number = "A301";
    addition.title_block.title = "Details";
    addition.callouts.clear();
    addition.schedules.clear();
    const auto appended = original.with_added_sheet(addition);
    require(appended.sheets().size() == 3 && appended.sheets().back().id == "c",
            "added sheet should be validated and canonically appended");
    require(original.sheets().size() == 2, "adding a sheet must preserve the original snapshot");
    auto duplicate_id = addition;
    duplicate_id.id = "a";
    duplicate_id.number = "A302";
    rejects([&] { (void)original.with_added_sheet(duplicate_id); });
    auto duplicate_number = addition;
    duplicate_number.id = "d";
    duplicate_number.number = "A101";
    rejects([&] { (void)original.with_added_sheet(duplicate_number); });
    const auto removed = appended.with_removed_sheet("c");
    require(removed.to_json() == original.to_json(),
            "removing the appended sheet should restore the original graph");
    rejects([&] { (void)original.with_removed_sheet("b"); });
    const auto without_a = original.with_removed_sheet("a");
    require(without_a.sheets().size() == 1 && without_a.sheets().front().id == "b",
            "unreferenced sheet removal should preserve the remaining page");
    rejects([&] { (void)original.with_removed_sheet("missing"); });

    sketch::CoordinatedView plan{"plan", "Plan"};
    sketch::DrawingSheet only;
    only.id = "only"; only.number = "A001"; only.viewports = {{"vp", "plan", {0, 0, 100, 100}, 100}};
    const auto single = sketch::SheetViewModel::create({plan}, {only});
    rejects([&] { (void)single.with_removed_sheet("only"); });
}
void serialization() {
    const auto saved = fixture().to_json();
    require(sketch::SheetViewModel::from_json(saved).to_json().dump() == saved.dump(), "canonical roundtrip");
    auto shuffled = saved;
    std::reverse(shuffled["views"].begin(), shuffled["views"].end());
    std::reverse(shuffled["sheets"].begin(), shuffled["sheets"].end());
    std::reverse(shuffled["schedule_ids"].begin(), shuffled["schedule_ids"].end());
    std::reverse(shuffled["sheets"][1]["viewports"].begin(), shuffled["sheets"][1]["viewports"].end());
    require(sketch::SheetViewModel::from_json(shuffled).to_json().dump() == saved.dump(), "deterministic input ordering");
    const std::vector<nlohmann::json::json_pointer> unknown_locations{
        nlohmann::json::json_pointer(""), nlohmann::json::json_pointer("/views/0"),
        nlohmann::json::json_pointer("/views/0/presentation"), nlohmann::json::json_pointer("/sheets/0"),
        nlohmann::json::json_pointer("/sheets/0/viewports/0/bounds"),
        nlohmann::json::json_pointer("/sheets/0/title_block"),
        nlohmann::json::json_pointer("/sheets/0/revisions/0"),
        nlohmann::json::json_pointer("/sheets/0/callouts/0"),
        nlohmann::json::json_pointer("/sheets/0/schedules/0")};
    for (const auto& location : unknown_locations) {
        auto invalid = saved; invalid[location]["extra"] = true;
        rejects([&] { (void)sketch::SheetViewModel::from_json(invalid); });
    }
    auto invalid = saved; invalid["views"][0]["direction"].push_back(0);
    rejects([&] { (void)sketch::SheetViewModel::from_json(invalid); });
    for (const auto& version : {nlohmann::json(2), nlohmann::json(1.0), nlohmann::json("1")}) {
        invalid = saved; invalid["version"] = version;
        rejects([&] { (void)sketch::SheetViewModel::from_json(invalid); });
    }
}
void invalid_values() {
    const auto model = fixture();
    const auto saved = model.to_json();
    const auto bad = [&](const char* path, nlohmann::json replacement) {
        auto invalid = saved; invalid[nlohmann::json::json_pointer(path)] = std::move(replacement);
        rejects([&] { (void)sketch::SheetViewModel::from_json(invalid); });
    };
    bad("/views/0/id", " "); bad("/views/1/id", "elevation");
    bad("/views/0/kind", "perspective"); bad("/views/0/presentation/detail", "ultra");
    bad("/views/0/presentation/cut_depth_m", -1); bad("/views/0/presentation/cut_depth_m", 101);
    bad("/views/0/presentation/hatch_scale", 0); bad("/views/0/presentation/cut_line_mm", 0);
    bad("/views/0/direction", {0, 0, 0}); bad("/views/0/up", {0, 1, 0});
    bad("/sheets/1/number", "A101"); bad("/sheets/0/width_mm", 0);
    bad("/sheets/0/viewports/0/view_id", "missing");
    bad("/sheets/0/viewports/1/id", "plan-large");
    bad("/sheets/0/viewports/0/scale_denominator", 0);
    bad("/sheets/0/viewports/0/scale_denominator", true);
    bad("/sheets/0/viewports/0/bounds/x_mm", -1);
    bad("/sheets/0/viewports/0/bounds/width_mm", 1000);
    bad("/sheets/0/callouts/0/target_sheet_id", "missing");
    bad("/sheets/0/callouts/0/target_viewport_id", "plan-large");
    bad("/sheets/0/callouts/0/x_mm", 421);
    bad("/sheets/0/schedules/0/schedule_id", "missing");
    bad("/schedule_ids", {"doors", "doors"});
    for (const double number : {std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
        auto views = model.views(); views[0].origin_m[0] = number;
        rejects([&] { (void)sketch::SheetViewModel::create(views, model.sheets(), {"doors", "rooms"}); });
        auto sheets = model.sheets(); sheets[0].viewports[0].scale_denominator = number;
        rejects([&] { (void)sketch::SheetViewModel::create(model.views(), sheets, {"doors", "rooms"}); });
    }
    auto view = model.views()[0]; view.kind = static_cast<sketch::CoordinatedViewKind>(99);
    rejects([&] { (void)model.with_view(view); });
    require(model.to_json() == saved, "validation failures preserve snapshot");
}
} // namespace
int main() {
    sketch::testing::noninteractive_errors();
    try {
        coordination_and_isolation(); sheet_lifecycle(); serialization(); invalid_values();
        std::cout << "sheet/view model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

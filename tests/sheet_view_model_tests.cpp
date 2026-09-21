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
    plan.object_ids = {"wall-main", "room-main"};
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

    auto revision = original.sheets().front().revisions.front();
    revision.description = "Issued for permit";
    const auto changed_revision = original.with_revision("a", revision);
    require(changed_revision.sheets().front().revisions.front() == revision,
            "revision metadata should update through the typed model");
    require(changed_revision.sheets().front().callouts == original.sheets().front().callouts,
            "revision edits must preserve cross-sheet callouts");
    auto added_revision = revision;
    added_revision.id = "03";
    const auto with_revision = original.with_added_revision("a", added_revision);
    require(with_revision.sheets().front().revisions.size() == 3,
            "revision additions should be validated and persisted");
    require(with_revision.with_removed_revision("a", "03").to_json() == original.to_json(),
            "revision removal should restore the original graph");
    rejects([&] { (void)original.with_revision("unknown", revision); });
    revision.id = "missing";
    rejects([&] { (void)original.with_revision("a", revision); });
    rejects([&] { (void)original.with_removed_revision("a", "missing"); });

    auto callout = original.sheets().front().callouts.front();
    callout.label = "A / A202";
    callout.x_mm = 60;
    const auto changed_callout = original.with_callout("a", callout);
    require(changed_callout.sheets().front().callouts.front() == callout,
            "callout metadata should update through the typed model");
    auto added_callout = callout;
    added_callout.id = "elevation-marker";
    added_callout.target_viewport_id = "elevation-main";
    added_callout.x_mm = 100;
    const auto with_callout = original.with_added_callout("a", added_callout);
    require(with_callout.sheets().front().callouts.size() == 2,
            "callout additions should be validated and persisted");
    require(with_callout.with_removed_callout("a", "elevation-marker").to_json() == original.to_json(),
            "callout removal should restore the original graph");
    rejects([&] { (void)original.with_callout("unknown", callout); });
    callout.id = "missing";
    rejects([&] { (void)original.with_callout("a", callout); });
    added_callout = original.sheets().front().callouts.front();
    added_callout.id = "bad-target";
    added_callout.target_viewport_id = "missing";
    rejects([&] { (void)original.with_added_callout("a", added_callout); });
    rejects([&] { (void)original.with_removed_callout("a", "missing"); });
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
void placement_lifecycle() {
    const auto original = fixture();
    const auto saved = original.to_json();
    sketch::SheetViewport viewport{"added", "plan", {0, 0, 100, 100}, 50};
    const auto added = original.with_added_viewport("a", viewport);
    require(added.sheets()[0].viewports.size() == 3, "viewport addition missing");
    require(added.with_removed_viewport("a", "added").to_json() == saved, "viewport removal roundtrip");
    rejects([&] { (void)added.with_added_viewport("a", viewport); });
    rejects([&] { (void)original.with_added_viewport("missing", viewport); });
    rejects([&] { (void)original.with_removed_viewport("missing", "added"); });
    rejects([&] { (void)original.with_removed_viewport("a", "missing"); });
    rejects([&] { (void)original.with_removed_viewport("b", "section-main"); });
    auto same_sheet = original.sheets()[1];
    same_sheet.callouts = {{"self", "Self", "b", "elevation-main", 0, 0}};
    const auto self_linked = original.with_sheet(same_sheet);
    rejects([&] { (void)self_linked.with_removed_viewport("b", "elevation-main"); });
    const auto same_id = original.with_added_viewport("a", {"section-main", "plan", {0, 0, 100, 100}, 100});
    require(same_id.with_removed_viewport("a", "section-main").to_json() == saved,
            "callout protection must qualify viewport identity by sheet");
    const auto unlinked = original.with_removed_callout("a", "section-marker");
    require(unlinked.with_removed_viewport("b", "section-main").sheets()[1].viewports.size() == 1,
            "unreferenced viewport removal failed");
    viewport.view_id = "missing";
    rejects([&] { (void)original.with_added_viewport("a", viewport); });
    viewport.view_id = "plan"; viewport.bounds.width_mm = 1000;
    rejects([&] { (void)original.with_added_viewport("a", viewport); });
    viewport.bounds.width_mm = 100; viewport.scale_denominator = 0;
    rejects([&] { (void)original.with_added_viewport("a", viewport); });
    sketch::SheetSchedulePlacement schedule{"added", "rooms", {0, 0, 100, 50}};
    const auto scheduled = original.with_added_schedule_placement("b", schedule);
    require(scheduled.sheets()[1].schedules.size() == 1, "schedule addition missing");
    require(scheduled.with_removed_schedule_placement("b", "added").to_json() == saved,
            "schedule removal roundtrip");
    rejects([&] { (void)scheduled.with_added_schedule_placement("b", schedule); });
    rejects([&] { (void)original.with_added_schedule_placement("missing", schedule); });
    rejects([&] { (void)original.with_removed_schedule_placement("missing", "added"); });
    rejects([&] { (void)original.with_removed_schedule_placement("b", "missing"); });
    schedule.schedule_id = "missing";
    rejects([&] { (void)original.with_added_schedule_placement("b", schedule); });
    schedule.schedule_id = "rooms"; schedule.bounds.x_mm = -1;
    rejects([&] { (void)original.with_added_schedule_placement("b", schedule); });
    schedule.bounds.x_mm = 0; schedule.bounds.height_mm = std::numeric_limits<double>::infinity();
    rejects([&] { (void)original.with_added_schedule_placement("b", schedule); });
    require(original.to_json() == saved, "placement lifecycle mutated source");
}
void serialization() {
    const auto saved = fixture().to_json();
    require(sketch::SheetViewModel::from_json(saved).to_json().dump() == saved.dump(), "canonical roundtrip");
    auto legacy = saved;
    legacy["version"] = 1;
    for (auto& view : legacy["views"]) view.erase("object_ids");
    auto legacy_views = fixture().views();
    for (auto& view : legacy_views) view.object_ids.clear();
    const auto legacy_expected = sketch::SheetViewModel::create(
        std::move(legacy_views), fixture().sheets(), {"doors", "rooms"});
    require(sketch::SheetViewModel::from_json(legacy).to_json().dump() ==
                legacy_expected.to_json().dump(),
            "version 1 sheet/view data must upgrade with empty object references");
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
    for (const auto& version : {nlohmann::json(4), nlohmann::json(2.0), nlohmann::json("2")}) {
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
    auto duplicate_references = model.views();
    duplicate_references[0].object_ids = {"wall-main", "wall-main"};
    rejects([&] {
        (void)sketch::SheetViewModel::create(duplicate_references, model.sheets(), {"doors", "rooms"});
    });
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
void section_overlays() {
    auto model = fixture();
    auto view = model.views().back();
    sketch::SectionOverlay note; note.id = "note"; note.text = "Section note";
    sketch::SectionOverlay line; line.id = "line"; line.kind = sketch::SectionOverlayKind::detail_line;
    line.minimum_detail = sketch::ViewDetail::fine;
    auto dimension = line; dimension.id = "dimension"; dimension.kind = sketch::SectionOverlayKind::dimension;
    dimension.minimum_detail = sketch::ViewDetail::coarse;
    view.overlays = {note, line, dimension};
    model = model.with_view(view);
    require(sketch::SheetViewModel::from_json(model.to_json()).to_json() == model.to_json(), "overlay round trip");
    require(!sketch::section_overlay_visible(line, sketch::ViewDetail::medium) &&
        sketch::section_overlay_visible(line, sketch::ViewDetail::fine), "detail controls inclusion");
    auto legacy = model.to_json(); legacy["version"] = 2;
    for (auto& v : legacy["views"]) v.erase("overlays");
    require(sketch::SheetViewModel::from_json(legacy).views().back().overlays.empty(), "v2 upgrade");
    const auto invalid = [&](auto edit) {
        auto candidate = view; edit(candidate); rejects([&] { (void)model.with_view(candidate); });
    };
    invalid([](auto& v) { v.overlays.push_back(v.overlays[0]); });
    invalid([](auto& v) { v.kind = sketch::CoordinatedViewKind::plan; });
    invalid([](auto& v) { v.overlays[0].text = " "; });
    invalid([](auto& v) { v.overlays[1].end_m = v.overlays[1].start_m; });
    invalid([](auto& v) { v.overlays[0].start_m[0] = std::numeric_limits<double>::infinity(); });
    invalid([](auto& v) { v.overlays[0].object_id = "missing"; });
    invalid([](auto& v) { v.overlays[0].text_height_mm = 21; });
    invalid([](auto& v) { v.overlays.resize(1001); });
    auto malformed = model.to_json(); malformed["views"][2]["overlays"][0]["start_m"].push_back(0);
    rejects([&] { (void)sketch::SheetViewModel::from_json(malformed); });
}
} // namespace
int main() {
    sketch::testing::noninteractive_errors();
    try {
        coordination_and_isolation(); sheet_lifecycle(); placement_lifecycle(); serialization(); invalid_values(); section_overlays();
        std::cout << "sheet/view model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

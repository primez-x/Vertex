#include "sketch/sheet_output_scene.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid scene creation accepted");
}
sketch::OutputFingerprintInputs dependencies() {
    sketch::OutputFingerprintInputs inputs;
    for (auto* group : {&inputs.fonts, &inputs.profiles, &inputs.crs}) {
        group->state = sketch::FingerprintGroupState::not_applicable;
        group->reason = "semantic adapter test";
    }
    inputs.processing_components.state = sketch::FingerprintGroupState::resources;
    for (const auto* role : {"kernel", "solver", "renderer", "adapters"}) {
        auto& item = inputs.processing_components.roles[role];
        item.state = sketch::FingerprintGroupState::resources;
        item.resources.push_back({role, std::string(64, 'a')});
    }
    inputs.application_build.state = sketch::FingerprintGroupState::resources;
    inputs.application_build.resources.push_back({"test-build", std::string(64, 'b')});
    return inputs;
}
sketch::SheetViewModel model() {
    sketch::DrawingSheet a, b;
    a.id = "a"; a.number = "A1";
    b.id = "b"; b.number = "A2";
    a.viewports.push_back({"vp", "plan", {10, 10, 100, 100}, 50});
    b.viewports.push_back({"vp", "plan", {20, 20, 100, 100}, 100});
    a.callouts.push_back({"callout", "A2", "b", "vp", 5, 5});
    return sketch::SheetViewModel::create({{"plan", "Plan"}}, {b, a});
}
void sheet_set_contract() {
    auto definition = model().to_json();
    definition["version"] = 4;
    definition["sheet_order"] = {"b", "a"};
    auto document = sketch::Document::create({sketch::make_sheet_view_entity(
        "sheets", sketch::SheetViewModel::from_json(definition))});
    const auto snapshot = document.snapshot();
    const auto inputs = dependencies();
    const auto scene = sketch::make_sheet_set_output_scene(snapshot, "sheets", inputs);
    require(scene.at("schema") == "sketch.sheet-set-output-scene" &&
            scene.at("version") == 1 && scene.at("sheet_ids") == nlohmann::json({"b", "a"}) &&
            !scene.contains("sheet_id"), "set scene must retain the complete explicit sheet order");
    require(scene == sketch::make_sheet_set_output_scene(snapshot, "sheets", inputs),
            "one immutable snapshot and dependencies must yield a deterministic set scene");
    require(sketch::check_sheet_set_output_scene_current(scene, snapshot, inputs).current,
            "fresh drawing set must be current");
    const auto selected = sketch::make_sheet_output_scene(snapshot, "sheets", "a", inputs);
    require(selected.at("schema") == "sketch.sheet-output-scene" && selected.at("version") == 1 &&
            selected.contains("sheet_id") && !selected.contains("sheet_ids"),
            "selected-sheet envelope must remain unchanged");
    require(!sketch::check_sheet_output_scene_current(scene, snapshot, inputs).valid &&
            !sketch::check_sheet_set_output_scene_current(selected, snapshot, inputs).valid,
            "selected-sheet and whole-set schemas must not be interchangeable");
    for (const auto& ids : {nlohmann::json::array(), nlohmann::json({"b"}),
                           nlohmann::json({"b", "b"}), nlohmann::json({"b", "unknown"}),
                           nlohmann::json({"a", "b"}), nlohmann::json({"b", "a", "unknown"}),
                           nlohmann::json({"b", 1})}) {
        auto invalid = scene;
        invalid["sheet_ids"] = ids;
        // Rebind the altered envelope to a valid fingerprint so membership
        // validation, rather than only a stale scene hash, must reject it.
        auto descriptor = invalid;
        descriptor.erase("fingerprint");
        const auto text = descriptor.dump();
        auto rebound = inputs;
        rebound.views.state = sketch::FingerprintGroupState::resources;
        rebound.views.resources.push_back({"sketch.sheet-set-output-scene",
            sketch::sha256_hex(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(text.data()), text.size())),
            {{"schema", "sketch.sheet-set-output-scene"}, {"version", 1},
             {"entity_id", "sheets"}, {"sheet_ids", ids}}});
        invalid["fingerprint"] = sketch::serialize_output_fingerprint(
            sketch::make_output_fingerprint(snapshot, rebound));
        require(!sketch::check_sheet_set_output_scene_current(invalid, snapshot, inputs).valid,
                "incomplete, duplicate, unknown, reordered or mistyped scene IDs must reject");
    }
    for (const auto* field : {"sheet_ids", "fingerprint", "definition"}) {
        auto invalid = scene;
        invalid.erase(field);
        require(!sketch::check_sheet_set_output_scene_current(invalid, snapshot, inputs).valid,
                "missing set scene fields must reject");
    }
    auto invalid = scene;
    invalid["fingerprint"] = selected.at("fingerprint");
    require(!sketch::check_sheet_set_output_scene_current(invalid, snapshot, inputs).valid,
            "set scene must bind its own fingerprint resource");
    invalid = scene;
    invalid["definition"]["sheets"][0]["width_mm"] = 500;
    require(!sketch::check_sheet_set_output_scene_current(invalid, snapshot, inputs).valid,
            "set definition tampering must reject");
    auto canonical = scene;
    std::reverse(canonical["definition"]["sheets"].begin(), canonical["definition"]["sheets"].end());
    require(sketch::check_sheet_set_output_scene_current(canonical, snapshot, inputs).current,
            "storage array order must not override explicit sheet order");
    auto changed_inputs = inputs;
    changed_inputs.application_build.resources[0].sha256 = std::string(64, 'c');
    auto status = sketch::check_sheet_set_output_scene_current(scene, snapshot, changed_inputs);
    require(status.valid && !status.current && status.changed_groups == std::vector<std::string>{"application_build"},
            "set scene must detect dependency changes");
    changed_inputs = inputs;
    changed_inputs.views.state = sketch::FingerprintGroupState::resources;
    changed_inputs.views.resources.push_back({"visibility", std::string(64, 'c')});
    const auto filtered = sketch::make_sheet_set_output_scene(snapshot, "sheets", changed_inputs);
    changed_inputs.views.resources.front().sha256 = std::string(64, 'd');
    status = sketch::check_sheet_set_output_scene_current(filtered, snapshot, changed_inputs);
    require(status.valid && !status.current && status.changed_groups == std::vector<std::string>{"views"},
            "set scene must detect caller view dependency changes");
    definition["sheet_order"] = {"a", "b"};
    document.apply(sketch::ApplyEntityChanges{document.revision(),
        {sketch::EntityChange::upsert(sketch::make_sheet_view_entity(
            "sheets", sketch::SheetViewModel::from_json(definition)))}, {}, "reorder"});
    const auto reordered = sketch::make_sheet_set_output_scene(document.snapshot(), "sheets", inputs);
    require(scene.at("fingerprint").at("digest_sha256") != reordered.at("fingerprint").at("digest_sha256"),
            "changing the explicit order must change the digest");
    status = sketch::check_sheet_set_output_scene_current(scene, document.snapshot(), inputs);
    require(status.valid && !status.current && status.changed_groups == std::vector<std::string>{"document", "views"},
            "sheet reordering must stale the drawing set");
    require(scene == sketch::make_sheet_set_output_scene(snapshot, "sheets", inputs),
            "document edits must not affect an immutable source snapshot");
    auto changed_model = sketch::SheetViewModel::from_json(definition).with_viewport(
        "b", {"vp", "plan", {20, 20, 100, 100}, 75});
    document.apply(sketch::ApplyEntityChanges{document.revision(),
        {sketch::EntityChange::upsert(sketch::make_sheet_view_entity("sheets", changed_model))}, {}, "scale"});
    status = sketch::check_sheet_set_output_scene_current(reordered, document.snapshot(), inputs);
    require(status.valid && !status.current && status.changed_groups == std::vector<std::string>{"document", "views"},
            "content change in any sheet must stale the drawing set");
    rejects([&] { (void)sketch::make_sheet_set_output_scene(snapshot, "missing", inputs); });
    changed_inputs = inputs;
    changed_inputs.views.state = sketch::FingerprintGroupState::no_resource;
    changed_inputs.views.reason = "override";
    rejects([&] { (void)sketch::make_sheet_set_output_scene(snapshot, "sheets", changed_inputs); });
    require(!sketch::check_sheet_set_output_scene_current(scene, snapshot, changed_inputs).valid,
            "invalid current set dependencies must reject");
    document.apply(sketch::ApplyEntityChanges{document.revision(),
        {sketch::EntityChange::erase("sheets")}, {}, "remove"});
    require(!sketch::check_sheet_set_output_scene_current(scene, document.snapshot(), inputs).valid,
            "deleted drawing set must reject currentness");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    std::filesystem::path path;
    try {
        sheet_set_contract();
        auto document = sketch::Document::create({sketch::make_sheet_view_entity("sheets", model())});
        const auto inputs = dependencies();
        const auto scene = sketch::make_sheet_output_scene(document.snapshot(), "sheets", "a", inputs);
        require(scene.is_object(), "scene must be a versioned object");
        require(scene.dump() == sketch::make_sheet_output_scene(document.snapshot(), "sheets", "a", inputs).dump(),
                "scene must be deterministic");
        require(sketch::check_sheet_output_scene_current(scene, document.snapshot(), inputs).current,
                "fresh scene must be current");
        path = std::filesystem::temp_directory_path() / (sketch::make_stable_id() + ".bldproj");
        (void)sketch::ProjectStore::save(path, document.snapshot());
        auto reopened = sketch::ProjectStore::load(path).document;
        require(scene == sketch::make_sheet_output_scene(reopened.snapshot(), "sheets", "a", inputs),
                "save/reopen must preserve output scene and fingerprint");
        require(sketch::make_sheet_set_output_scene(document.snapshot(), "sheets", inputs) ==
                    sketch::make_sheet_set_output_scene(reopened.snapshot(), "sheets", inputs),
                "save/reopen must preserve the whole drawing-set scene and fingerprint");
        std::filesystem::remove(path); path.clear();
        const auto second = sketch::make_sheet_output_scene(document.snapshot(), "sheets", "b", inputs);
        require(scene["fingerprint"] != second["fingerprint"], "sheet selection must change fingerprint");
        auto changed_inputs = inputs;
        changed_inputs.application_build.resources[0].sha256 = std::string(64, 'c');
        auto status = sketch::check_sheet_output_scene_current(scene, document.snapshot(), changed_inputs);
        require(status.valid && !status.current && status.changed_groups == std::vector<std::string>{"application_build"},
                "changed dependency must report stale group");
        for (const auto* field : {"width_mm", "height_mm"}) {
            auto invalid = scene;
            invalid["definition"]["sheets"][0][field] = 0;
            require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid,
                    "invalid page size must fail");
        }
        for (const auto value : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
            auto invalid = scene;
            invalid["definition"]["sheets"][0]["viewports"][0]["scale_denominator"] = value;
            require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid,
                    "invalid scale must fail");
        }
        auto invalid = scene;
        invalid["definition"]["sheets"][0]["viewports"][0]["view_id"] = "missing";
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "dangling view accepted");
        invalid = scene;
        invalid["definition"]["sheets"][0]["viewports"][0]["bounds"]["x_mm"] = 1000;
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "off-page viewport accepted");
        invalid = scene;
        invalid["definition"]["sheets"][0]["width_mm"] = 500;
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "valid geometry tamper accepted");
        auto reordered = scene;
        std::reverse(reordered["definition"]["sheets"].begin(), reordered["definition"]["sheets"].end());
        require(sketch::check_sheet_output_scene_current(reordered, document.snapshot(), inputs).current,
                "equivalent reordered graph should normalize");
        invalid = scene; invalid["sheet_id"] = "b";
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "selection tamper accepted");
        invalid = scene; invalid["version"] = 2;
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "unknown version accepted");
        invalid = scene; invalid["version"] = 1.0;
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "float version accepted");
        invalid = scene; invalid["extra"] = true;
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "unknown field accepted");
        invalid = scene; invalid.erase("fingerprint");
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "missing fingerprint accepted");
        invalid = scene; invalid["fingerprint"]["digest_sha256"] = std::string(64, '0');
        require(!sketch::check_sheet_output_scene_current(invalid, document.snapshot(), inputs).valid, "corrupt digest accepted");
        rejects([&] { (void)sketch::make_sheet_output_scene(document.snapshot(), "missing", "a", inputs); });
        rejects([&] { (void)sketch::make_sheet_output_scene(document.snapshot(), "sheets", "missing", inputs); });
        changed_inputs = inputs;
        changed_inputs.views.state = sketch::FingerprintGroupState::no_resource;
        changed_inputs.views.reason = "caller view override";
        rejects([&] { (void)sketch::make_sheet_output_scene(document.snapshot(), "sheets", "a", changed_inputs); });
        changed_inputs = inputs;
        changed_inputs.views.state = sketch::FingerprintGroupState::resources;
        changed_inputs.views.resources.push_back({"effective-visibility", std::string(64, 'c'),
                                                  {{"hidden_floor_ids", {"floor-1"}}}});
        const auto filtered_scene = sketch::make_sheet_output_scene(
            document.snapshot(), "sheets", "a", changed_inputs);
        require(sketch::check_sheet_output_scene_current(
                    filtered_scene, document.snapshot(), changed_inputs).current,
                "caller-supplied effective visibility must bind into a sheet scene");
        auto changed_visibility = changed_inputs;
        changed_visibility.views.resources.front().sha256 = std::string(64, 'd');
        const auto visibility_status = sketch::check_sheet_output_scene_current(
            filtered_scene, document.snapshot(), changed_visibility);
        require(visibility_status.valid && !visibility_status.current &&
                    visibility_status.changed_groups == std::vector<std::string>{"views"},
                "changed effective visibility must stale a sheet scene");
        changed_inputs = inputs;
        changed_inputs.application_build.resources.clear();
        rejects([&] { (void)sketch::make_sheet_output_scene(document.snapshot(), "sheets", "a", changed_inputs); });
        require(!sketch::check_sheet_output_scene_current(scene, document.snapshot(), changed_inputs).valid,
                "invalid current dependencies accepted");
        auto changed_model = model().with_viewport("a", {"vp", "plan", {10, 10, 100, 100}, 75});
        document.apply(sketch::ApplyEntityChanges{document.revision(),
            {sketch::EntityChange::upsert(sketch::make_sheet_view_entity("sheets", changed_model))}, {}, "scale"});
        status = sketch::check_sheet_output_scene_current(scene, document.snapshot(), inputs);
        require(status.valid && !status.current && status.changed_groups == std::vector<std::string>{"document", "views"},
                "persisted scale change must stale document and scene view groups");
        document.apply(sketch::ApplyEntityChanges{document.revision(),
            {sketch::EntityChange::erase("sheets")}, {}, "remove sheet graph"});
        require(!sketch::check_sheet_output_scene_current(scene, document.snapshot(), inputs).valid,
                "deleted persisted selection accepted");
        std::cout << "sheet output scene tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); }
        std::cerr << error.what() << '\n'; return 1;
    }
}

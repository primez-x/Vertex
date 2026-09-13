#include "sketch/architectural_workflow_contract.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value) { if (!value) throw std::runtime_error("architectural contract check failed"); }
template<class F> void rejects(F fn) {
    try { fn(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid architectural contract accepted");
}
}
int main() {
    try {
        using namespace sketch;
        check(can_transform_architectural_entity_type("wall") &&
                  can_transform_architectural_entity_type("slab") &&
                  can_transform_architectural_entity_type("room") &&
                  can_transform_architectural_entity_type("column") &&
                  can_transform_architectural_entity_type("roof") &&
                  !can_transform_architectural_entity_type("annotation"));
        const ArchitecturalOperation create{ArchitecturalAction::create, "wall-b", "wall", {}, {{"height", "3m"}}};
        const ArchitecturalOperation select{ArchitecturalAction::select, "wall-b"};
        const ArchitecturalOperation edit{ArchitecturalAction::property_edit, "wall-b", {}, {}, {{"height", "4m"}}};
        ArchitecturalOperation move{ArchitecturalAction::transform, "wall-b"};
        move.transform = ArchitecturalTransform{1, 2, 0, 0.5, 1};
        const ArchitecturalOperation duplicate{ArchitecturalAction::duplicate, "wall-b", {}, "wall-c"};
        const ArchitecturalOperation remove{ArchitecturalAction::erase, "wall-b"};
        auto tx = ArchitecturalTransaction::create("edit-1", "model-r1", {"wall-a"},
            {create, select, edit, move, duplicate, remove}, "Edit walls");
        check(tx.resulting_ids() == std::vector<std::string>({"wall-a", "wall-c"}));
        check(tx.to_json().at("history").at("intent") == "single_undoable_transaction");
        auto detached_json = tx.to_json(); detached_json["operations"] = nullptr;
        check(tx.to_json().at("operations").size() == 6);
        check(ArchitecturalTransaction::create("e", "r", {"z", "a"},
            {{ArchitecturalAction::select, "a"}}, "Select").to_json() ==
            ArchitecturalTransaction::create("e", "r", {"a", "z"},
            {{ArchitecturalAction::select, "a"}}, "Select").to_json());
        rejects([&] { (void)ArchitecturalTransaction::create(" ", "r", {}, {create}, "Edit"); });
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {}, {}, "Edit"); });
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {}, {create}, " \t"); });
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {}, {select}, "Edit"); });
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {}, {create, create}, "Edit"); });
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {}, {create, remove, edit}, "Edit"); });
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {}, {create, remove, create}, "Edit"); });
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {"x", "x"}, {}, "Edit"); });
        auto bad = select; bad.action = static_cast<ArchitecturalAction>(999);
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {"wall-b"}, {bad}, "Edit"); });
        bad = select; bad.properties["hidden"] = "payload";
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {"wall-b"}, {bad}, "Edit"); });
        bad = move; bad.transform->scale = std::numeric_limits<double>::infinity();
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {"wall-b"}, {bad}, "Edit"); });
        bad = move; bad.transform.reset();
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {"wall-b"}, {bad}, "Edit"); });
        bad = edit; bad.properties.clear();
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {"wall-b"}, {bad}, "Edit"); });
        bad = duplicate; bad.duplicate_id = "wall-b";
        rejects([&] { (void)ArchitecturalTransaction::create("e", "r", {"wall-b"}, {bad}, "Edit"); });
        std::vector<ArchitecturalOperation> detached_ops{create};
        auto snapshot = ArchitecturalTransaction::create("e", "r", {}, detached_ops, "Create");
        detached_ops.front().properties["height"] = "999m";
        check(snapshot.to_json().at("operations").at(0).at("properties").at("height") == "3m");
        std::vector<ArchitecturalOutputRequirement> outputs;
        for (auto kind : {ArchitecturalOutputKind::plan, ArchitecturalOutputKind::elevation,
             ArchitecturalOutputKind::section, ArchitecturalOutputKind::view_3d,
             ArchitecturalOutputKind::schedule}) {
            outputs.push_back({"output-" + std::to_string(outputs.size()), kind, {"wall-a"}, "sheet-1"});
        }
        auto output = ArchitecturalOutputContract::create("issue-1", "model-r2", {"wall-a", "wall-c"},
            {"sheet-1"}, outputs);
        check(output.to_json().at("requirements").size() == 5);
        auto reversed = outputs; std::reverse(reversed.begin(), reversed.end());
        check(output.to_json() == ArchitecturalOutputContract::create("issue-1", "model-r2",
            {"wall-c", "wall-a"}, {"sheet-1"}, reversed).to_json());
        auto invalid = outputs; invalid.front().source_ids = {"absent"};
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"sheet-1"}, invalid); });
        invalid = outputs; invalid.front().sheet_id = "missing";
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"sheet-1"}, invalid); });
        invalid = outputs; invalid.front().kind = static_cast<ArchitecturalOutputKind>(999);
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"sheet-1"}, invalid); });
        invalid = outputs; invalid.front().source_ids.clear();
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"sheet-1"}, invalid); });
        invalid = outputs; invalid.back().id = invalid.front().id;
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"sheet-1"}, invalid); });
        invalid = outputs; invalid.front().source_ids.push_back("wall-a");
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"sheet-1"}, invalid); });
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"unused"}, outputs); });
        rejects([&] { (void)ArchitecturalOutputContract::create("i", "r", {"wall-a"}, {"sheet-1", "unused"}, outputs); });
        outputs.front().source_ids.clear();
        check(!output.to_json().at("requirements").at(0).at("source_ids").empty());
        std::cout << "Architectural workflow contract tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

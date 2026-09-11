#include "sketch/architectural_document_adapter.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/model_phases.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template <typename F>
void rejects(F&& function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid architectural adapter operation accepted");
}
}

int main() {
    try {
        using namespace sketch;
        auto wall = Entity::create("wall", {{"height_m", 3.0}});
        wall.id = "wall-a";
        Document document = Document::create({wall});
        const auto before = document.snapshot();
        ArchitecturalOperation edit{ArchitecturalAction::property_edit, "wall-a", {}, {},
                                    {{"height_m", "4.0"}, {"opaque_note", "4m"}}};
        ArchitecturalOperation transform{ArchitecturalAction::transform, "wall-a"};
        transform.transform = ArchitecturalTransform{1, 2, 0, 0.25, 1};
        const auto transaction = ArchitecturalTransaction::create(
            "tx-1", "model-r0", {"wall-a"}, {edit, transform}, "Update wall");
        const auto preview = preview_architectural_transaction(before, transaction);
        require(preview.entities().at("wall-a").properties.at("height_m") == 4.0,
                "preview numeric property edit lost its type");
        require(preview.entities().at("wall-a").properties.at("opaque_note") == "4m",
                "preview opaque property edit lost its string value");
        require(preview.entities().at("wall-a").properties.contains("transform"),
                "preview transform missing");
        require(document_snapshot_digest(document.snapshot()) == document_snapshot_digest(before),
                "preview mutated source document");
        const auto revision = apply_architectural_transaction(document, transaction, document.revision());
        require(revision == 1 && document.snapshot().entities().at("wall-a").properties.at("height_m") == 4.0,
                "architectural transaction did not apply");
        const auto path = std::filesystem::temp_directory_path() / "property-studio-architectural-adapter.bldproj";
        std::filesystem::remove(path);
        (void)ProjectStore::save(path, document.snapshot());
        auto reopened = ProjectStore::load(path).document;
        require(reopened.snapshot().entities().at("wall-a").properties.at("height_m") == 4.0 &&
                    reopened.snapshot().entities().at("wall-a").properties.contains("transform"),
                "architectural transaction did not survive save/reopen");
        std::filesystem::remove(path);
        document.undo(document.revision());
        require(document.snapshot().entities().at("wall-a").properties.at("height_m") == 3.0,
                "architectural transaction did not undo");
        rejects([&] { (void)apply_architectural_transaction(document, transaction, 99); });
        auto stale = ArchitecturalTransaction::create("tx-2", "r", {"missing"},
            {{ArchitecturalAction::select, "missing"}}, "stale");
        rejects([&] { (void)preview_architectural_transaction(document.snapshot(), stale); });
        const auto select = ArchitecturalTransaction::create("tx-3", "r", {"wall-a"},
            {{ArchitecturalAction::select, "wall-a"}}, "Select");
        require(preview_architectural_transaction(document.snapshot(), select).revision() == document.revision(),
                "select-only preview should not create a revision");
        rejects([&] { (void)apply_architectural_transaction(document, select, 99); });

        auto label = Entity::create("label");
        label.id = "label-a";
        auto invalid_phases = Entity::create("model_phases", {{"model",
            ModelPhases::create({label.id}, {label.id}, {}).to_json()}});
        rejects([&] { (void)Document::create({label, invalid_phases}); });

        AssemblyType type{"type-a", "Wall", {{"finish", "paint"}}, {}, {}};
        const auto assemblies = AssemblyModel::create({}, {type},
            {{"instance-a", "type-a", {}, {}, {}}});
        auto assembly_entity = Entity::create("assembly_model", {{"model", assemblies.to_json()}, {"note", "keep"}});
        assembly_entity.id = "assemblies";
        const auto phases = ModelPhases::create({wall.id}, {wall.id}, {{"option-a", "Remove wall", {wall.id}, {}}});
        auto phase_entity = Entity::create("model_phases", {{"model", phases.to_json()}});
        phase_entity.id = "phases";
        auto semantic = Document::create({wall, assembly_entity, phase_entity});
        type.properties["finish"] = "tile";
        semantic.apply(assembly_type_update_command(semantic.snapshot(), "assemblies", type, semantic.revision()));
        semantic.apply(model_phase_selection_command(semantic.snapshot(), "phases", "option-a", semantic.revision()));
        require(semantic.snapshot().entities().at("assemblies").properties.at("note") == "keep", "typed edit lost unrelated properties");
        const auto digest = document_snapshot_digest(semantic.snapshot());
        rejects([&] { semantic.apply(model_phase_selection_command(semantic.snapshot(), "phases", "missing", semantic.revision())); });
        rejects([&] { semantic.apply(assembly_type_update_command(semantic.snapshot(), "phases", type, semantic.revision())); });
        rejects([&] { semantic.apply(ApplyEntityChanges{semantic.revision(), {EntityChange::erase(wall.id)}, {}, "Remove referenced wall"}); });
        auto future = phase_entity;
        future.properties["model"]["version"] = 2;
        rejects([&] { semantic.apply(ApplyEntityChanges{semantic.revision(), {EntityChange::upsert(future)}, {}, "Unknown version"}); });
        require(document_snapshot_digest(semantic.snapshot()) == digest, "rejected semantic edits changed history");
        (void)ProjectStore::save(path, semantic.snapshot());
        auto semantic_reopened = ProjectStore::load(path).document;
        require(ModelPhases::from_json(semantic_reopened.snapshot().entities().at("phases").properties.at("model")).active_alternative() == "option-a", "phase selection did not reopen");
        semantic_reopened.undo(semantic_reopened.revision());
        require(!ModelPhases::from_json(semantic_reopened.snapshot().entities().at("phases").properties.at("model")).active_alternative(), "phase selection did not undo after reopening");
        semantic_reopened.undo(semantic_reopened.revision());
        require(AssemblyModel::from_json(semantic_reopened.snapshot().entities().at("assemblies").properties.at("model")).resolve("instance-a").properties.at("finish") == "paint", "assembly edit did not undo");
        semantic_reopened.redo(semantic_reopened.revision());
        semantic_reopened.redo(semantic_reopened.revision());
        require(semantic_reopened.snapshot().entities() == semantic.snapshot().entities(), "semantic redo did not restore edited models");
        std::filesystem::remove(path);
        std::cout << "architectural document adapter tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

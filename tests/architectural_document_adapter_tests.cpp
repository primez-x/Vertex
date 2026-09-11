#include "sketch/architectural_document_adapter.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"

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
        ArchitecturalOperation edit{ArchitecturalAction::property_edit, "wall-a", {}, {}, {{"height_m", "4.0"}}};
        ArchitecturalOperation transform{ArchitecturalAction::transform, "wall-a"};
        transform.transform = ArchitecturalTransform{1, 2, 0, 0.25, 1};
        const auto transaction = ArchitecturalTransaction::create(
            "tx-1", "model-r0", {"wall-a"}, {edit, transform}, "Update wall");
        const auto preview = preview_architectural_transaction(before, transaction);
        require(preview.entities().at("wall-a").properties.at("height_m") == "4.0",
                "preview property edit missing");
        require(preview.entities().at("wall-a").properties.contains("transform"),
                "preview transform missing");
        require(document_snapshot_digest(document.snapshot()) == document_snapshot_digest(before),
                "preview mutated source document");
        const auto revision = apply_architectural_transaction(document, transaction, document.revision());
        require(revision == 1 && document.snapshot().entities().at("wall-a").properties.at("height_m") == "4.0",
                "architectural transaction did not apply");
        const auto path = std::filesystem::temp_directory_path() / "property-studio-architectural-adapter.bldproj";
        std::filesystem::remove(path);
        (void)ProjectStore::save(path, document.snapshot());
        auto reopened = ProjectStore::load(path).document;
        require(reopened.snapshot().entities().at("wall-a").properties.at("height_m") == "4.0" &&
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
        std::cout << "architectural document adapter tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

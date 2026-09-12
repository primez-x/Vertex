#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/building_object_dialog.hpp"
#include "sketch/desktop/hosted_opening_dialog.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Drive the real command palette and nested native Qt prompts. The barrier
// deliberately changes the source during the first authoring prompt.
void invoke_with_intervention(sketch::desktop::MainWindow& window, QString command,
                              std::function<void()> intervention) {
    struct State {
        bool active{true}; bool invoked{}; bool intervened{}; bool timed_out{};
        std::function<void()> step;
    };
    auto state = std::make_shared<State>();
    const auto weak = std::weak_ptr<State>(state);
    state->step = [&, weak, command, intervention] {
        const auto current = weak.lock();
        if (!current || !current->active) return;
        auto* modal = QApplication::activeModalWidget();
        QTimer::singleShot(5, &window, current->step);
        if (auto* input = qobject_cast<QInputDialog*>(modal)) {
            if (!current->intervened) { current->intervened = true; intervention(); }
            if (input->textValue().isEmpty()) input->setTextValue("Fixture name");
            input->accept();
        } else if (auto* opening = dynamic_cast<sketch::desktop::HostedOpeningDialog*>(modal)) {
            if (!current->intervened) { current->intervened = true; intervention(); }
            require(opening->submit(), "opening form should submit after the barrier");
        } else if (auto* building = dynamic_cast<sketch::desktop::BuildingObjectDialog*>(modal)) {
            if (!current->intervened) { current->intervened = true; intervention(); }
            require(building->submit(), "building form should submit after the barrier");
        } else if (modal && !current->invoked) {
            auto* list = modal->findChild<QListWidget*>();
            auto* search = modal->findChild<QLineEdit*>();
            if (!list || !search) return;
            search->setText(command);
            require(list->count() == 1, "command palette query must resolve exactly once");
            current->invoked = true;
            QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
        }
    };
    QTimer::singleShot(0, &window, state->step);
    QTimer::singleShot(5000, &window, [weak] {
        const auto current = weak.lock();
        if (!current || !current->active) return;
        current->timed_out = true;
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->reject();
    });
    window.showCommandPalette();
    state->active = false;
    require(!state->timed_out && state->invoked && state->intervened,
            "modal intervention did not reach the intended live prompt");
}

void test_modal_sources_are_bound() {
    using namespace sketch;
    using namespace sketch::desktop;
    for (const auto& command : {QStringLiteral("Create door opening"),
                               QStringLiteral("Create slab from selected boundary"),
                               QStringLiteral("Create column, beam, stair or roof"),
                               QStringLiteral("Add building"),
                               QStringLiteral("Rename selected property, building, floor or layer")}) {
        for (const int intervention_kind : {0, 1, 2, 3, 4}) {
            MainWindow window;
            const auto wall = window.createStraightWall({0, 0}, {10, 0});
            require(!wall.isEmpty(), "modal fixture wall failed");
            const auto boundary = window.createBoundary({{{0, 0}, {10, 0}, 0}, {{10, 0}, {10, 6}, 0},
                                                         {{10, 6}, {0, 6}, 0}, {{0, 6}, {0, 0}, 0}});
            require(!boundary.isEmpty(), "modal fixture boundary failed");
            const auto initial_layer = window.activeLayerId();
            const auto second_layer = window.createLayer("floor-1", "Intervening layer");
            require(!second_layer.isEmpty(), "modal replacement fixture second layer failed");
            require(window.setActiveLayer(initial_layer), "modal initial drawing layer restore failed");
            QString selected = command.contains("slab") ? boundary : wall;
            if (command.startsWith("Add") || command.startsWith("Rename")) {
                const auto source = window.document().snapshot();
                for (const auto& [id, entity] : source.entities())
                    if (entity.type == "property") { selected = QString::fromStdString(id); break; }
            }
            require(window.selectEntity(selected), "modal fixture source selection failed");
            QTemporaryDir directory;
            require(directory.isValid(), "modal fixture directory failed");
            const auto path = directory.filePath("same-revision.bldproj");
            require(window.saveProjectAs(path), "modal replacement fixture save failed");
            auto after_intervention = window.document().snapshot();
            invoke_with_intervention(window, command, [&] {
                if (intervention_kind == 1) {
                    const auto revision = window.document().revision();
                    require(window.openProject(path), "same-revision replacement failed");
                    require(window.document().revision() == revision, "replacement fixture revision differs");
                    require(window.selectEntity(selected), "replacement should have identical source ID");
                } else if (intervention_kind == 2) {
                    require(window.selectEntity(selected == wall ? boundary : wall), "selection intervention failed");
                } else if (intervention_kind == 3) {
                    require(window.activeLayerId() != second_layer, "layer intervention must change context");
                    require(window.setActiveLayer(second_layer), "layer intervention failed");
                } else if (intervention_kind == 4) {
                    window.setMetricUnits(!window.metricUnits());
                } else {
                    auto changed = window.document().snapshot().entities().at(selected.toStdString());
                    changed.properties["name"] = "Changed while authoring";
                    window.document().apply(ApplyEntityChanges{window.document().revision(),
                        {EntityChange::upsert(changed)}, {}, "intervening edit"});
                }
                after_intervention = window.document().snapshot();
            });
            const auto after = window.document().snapshot();
            require(after.entities() == after_intervention.entities() &&
                    after.revision() == after_intervention.revision() &&
                    after.history().size() == after_intervention.history().size(),
                    "stale modal authoring changed the post-intervention project");
            require(window.lastError().contains("changed"), "stale modal intent needs an explanation");
        }
    }
}

void test_retained_revision_reaches_organization_apply() {
    sketch::desktop::MainWindow window;
    const auto captured = window.document().revision();
    require(!window.createLayer("floor-1", "Intervening layer").isEmpty(), "intervening organization fixture failed");
    const auto before = window.document().snapshot();
    require(window.createBuilding("property-1", "Stale building", captured).isEmpty(),
            "building helper recaptured a newer revision");
    require(window.createFloor("building-1", "Stale floor", captured).isEmpty(),
            "floor helper recaptured a newer revision");
    require(window.createLayer("floor-1", "Stale layer", captured).isEmpty(),
            "layer helper recaptured a newer revision");
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() && after.entities() == before.entities() &&
            after.history().size() == before.history().size(), "stale helper changed post-intervention state");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        test_modal_sources_are_bound();
        test_retained_revision_reaches_organization_apply();
        std::cout << "Modal authoring source barriers passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "modal_authoring_tests: " << error.what() << '\n';
        return 1;
    }
}

#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QEventLoop>
#include <QInputDialog>
#include <QMenu>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void events() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }

void unchanged(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    require(before.entities() == after.entities() && before.assets() == after.assets() &&
            before.revision() == after.revision() && before.history().size() == after.history().size() &&
            before.document_id() == after.document_id() &&
            before.saved_revision_optional() == after.saved_revision_optional() &&
            before.named_revisions() == after.named_revisions() &&
            before.is_editable() == after.is_editable() && before.read_only_reason() == after.read_only_reason(),
            "cancel or missing parent preserves entities, assets, revision and history");
    for (std::size_t index = 0; index < before.history().size(); ++index) {
        const auto& a = before.history()[index];
        const auto& b = after.history()[index];
        require(a.revision == b.revision && a.parent_revision == b.parent_revision &&
                a.source_revision == b.source_revision && a.action == b.action && a.name == b.name &&
                a.entities == b.entities && a.assets == b.assets &&
                a.undo_stack == b.undo_stack && a.redo_stack == b.redo_stack,
                "cancel preserves retained hierarchy history records and undo/redo stacks");
    }
}

QAction& propertyAction(MainWindow& window, const QString& property, const QString& kind) {
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    require(tree, "fixture uses the real project navigator");
    QTreeWidgetItemIterator iterator(tree);
    while (*iterator) {
        auto* item = *iterator;
        if (item->data(0, Qt::UserRole).toString() == property) {
            auto* button = qobject_cast<QToolButton*>(tree->itemWidget(item, 1));
            require(button && button->accessibleName() == QStringLiteral("Add to property") && button->menu(),
                    "clicked property has its actual inline Add to property menu");
            for (auto* action : button->menu()->actions())
                if (action->text().startsWith(QStringLiteral("Add ") + kind)) return *action;
            throw std::runtime_error("property menu lacks requested Add action");
        }
        ++iterator;
    }
    throw std::runtime_error("clicked property is absent from navigator");
}

// Respond through the actual nested modal event loop; never call authoring APIs
// in place of the inline menu's implementation.
struct Responses {
    QStringList parents;
    QString chosen;
    QString name;
    bool cancel_parent{};
    bool chooser_seen{};
    bool name_seen{};
    std::string failure;

    void trigger(MainWindow& window, QAction& action) {
        QTimer poll;
        poll.setInterval(1);
        QObject::connect(&poll, &QTimer::timeout, &window, [&] {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!modal) return;
            auto* input = qobject_cast<QInputDialog*>(modal);
            if (!input) {
                failure = "unexpected modal in navigator Add workflow";
                modal->reject();
                return;
            }
            if (input->objectName() == QStringLiteral("navigatorParentChooser")) {
                chooser_seen = true;
                auto* combo = input->findChild<QComboBox*>();
                if (parents.size() < 2 || !combo || combo->count() != parents.size()) {
                    failure = "parent chooser must contain exactly the clicked property's eligible parents";
                    input->reject();
                    return;
                }
                for (const auto& parent : parents) {
                    bool found = false;
                    for (int index = 0; index < combo->count(); ++index)
                        found = found || combo->itemText(index).contains(QStringLiteral("[") + parent + QStringLiteral("]"));
                    if (!found) failure = "parent chooser omits a clicked-property parent or admits another branch";
                }
                if (!failure.empty() || cancel_parent) {
                    input->reject();
                    return;
                }
                for (int index = 0; index < combo->count(); ++index)
                    if (combo->itemText(index).contains(QStringLiteral("[") + chosen + QStringLiteral("]")))
                        combo->setCurrentIndex(index);
                input->accept();
            } else {
                if (parents.size() > 1 && !chooser_seen) {
                    failure = "multiple parents require an explicit scoped chooser before name entry";
                    input->reject();
                    return;
                }
                name_seen = true;
                input->setTextValue(name);
                input->accept();
            }
        });
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &window, [&] {
            failure = "navigator Add workflow exceeded modal deadline";
            if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
        });
        poll.start();
        deadline.start(10000);
        action.trigger();
        poll.stop();
        deadline.stop();
        events();
        if (!failure.empty()) throw std::runtime_error(failure);
    }
};

struct Fixture {
    QTemporaryDir directory;
    MainWindow window;
    std::vector<QString> buildings;
    std::vector<QString> floors;

    Fixture(int parent_count, bool layer)
        : window({}, nullptr, directory.filePath(QStringLiteral("library.json"))) {
        require(directory.isValid(), "fixture uses temporary local storage");
        auto property = window.document().snapshot().entities().at("property-1");
        property.id = "property-2";
        property.properties["name"] = "Clicked property";
        property.properties["subject"]["name"] = "Clicked property";
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(property)}, {}, "Seed independent clicked property"});
        for (int index = 0; index < parent_count; ++index) {
            const auto building = window.createBuilding(QStringLiteral("property-2"),
                QStringLiteral("Clicked building %1").arg(index + 1));
            require(!building.isEmpty(), "clicked-property building is admitted");
            buildings.push_back(building);
            if (layer) {
                const auto floor = window.createFloor(building, QStringLiteral("Clicked floor %1").arg(index + 1));
                require(!floor.isEmpty(), "clicked-property floor is admitted");
                floors.push_back(floor);
            }
        }
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.resize(1400, 900);
        window.show();
        QApplication::setActiveWindow(&window);
        events();
        require(window.setActiveLayer(QStringLiteral("layer-1")), "different property's layer stays active");
        require(window.selectEntity(layer ? QStringLiteral("floor-1") : QStringLiteral("building-1")),
                "different property's eligible parent stays selected");
        events();
    }
};

void add_in_clicked_branch(bool layer, int parent_count, bool cancel) {
    Fixture fixture(parent_count, layer);
    const auto before = fixture.window.document().snapshot();
    const auto& parents = layer ? fixture.floors : fixture.buildings;
    Responses response;
    for (const auto& parent : parents) response.parents.append(parent);
    response.chosen = parents.back();
    response.name = layer ? QStringLiteral("New clicked layer") : QStringLiteral("New clicked floor");
    response.cancel_parent = cancel;
    response.trigger(fixture.window, propertyAction(fixture.window, QStringLiteral("property-2"),
                                                   layer ? QStringLiteral("layer") : QStringLiteral("floor")));
    const auto after = fixture.window.document().snapshot();
    if (cancel) {
        require(response.chooser_seen && !response.name_seen, "cancelled scoped parent choice never asks for a name");
        unchanged(before, after);
        return;
    }
    require(response.name_seen && response.chooser_seen == (parent_count > 1),
            "single parent is automatic and multiple parents require explicit choice");
    const Entity* added = nullptr;
    std::vector<const Entity*> default_layers;
    for (const auto& [id, entity] : after.entities()) {
        if (!before.entities().contains(id)) {
            if (!layer && entity.type == "layer") {
                default_layers.push_back(&entity);
                continue;
            }
            require(!added, "one accepted menu action creates exactly one object");
            added = &entity;
        }
    }
    require(added && added->type == (layer ? "layer" : "floor"), "menu adds the requested organization type");
    require(added->properties.at(layer ? "floor_id" : "building_id").get<std::string>() == response.chosen.toStdString(),
            "inline Add must use a parent under clicked property rather than active or selected other property");
    require(layer ? default_layers.empty() : default_layers.size() == 1 &&
            default_layers.front()->properties.at("floor_id").get<std::string>() == added->id,
            "new floor's ordinary default layer remains bound to that same clicked-property floor");
    require(after.revision() == before.revision() + 1 && after.history().size() == before.history().size() + 1,
            "accepted hierarchy creation has one revision and one history entry");
    require(fixture.window.undoCommand() && fixture.window.document().snapshot().entities() == before.entities(),
            "Undo removes only the clicked-property addition");
    require(fixture.window.redoCommand() && fixture.window.document().snapshot().entities() == after.entities(),
            "Redo restores the exact clicked-property parent binding");
}

void no_parent(bool layer) {
    Fixture fixture(0, layer);
    const auto before = fixture.window.document().snapshot();
    Responses response;
    response.name = QStringLiteral("Must not be created");
    response.trigger(fixture.window, propertyAction(fixture.window, QStringLiteral("property-2"),
                                                   layer ? QStringLiteral("layer") : QStringLiteral("floor")));
    unchanged(before, fixture.window.document().snapshot());
    require(!response.name_seen && !response.chooser_seen, "missing scoped parent opens no creation dialog");
    const auto error = fixture.window.lastError().toLower();
    require(error.contains(layer ? QStringLiteral("floor") : QStringLiteral("building")) &&
            (error.contains(QStringLiteral("add")) || error.contains(QStringLiteral("create"))),
            "missing parent explains what to add in the clicked property");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Vertex-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-navigator-branch-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        for (const bool layer : {false, true}) {
            std::cout << "navigator clicked-property Add " << (layer ? "layer" : "floor") << '\n';
            add_in_clicked_branch(layer, 1, false);
            add_in_clicked_branch(layer, 2, false);
            add_in_clicked_branch(layer, 2, true);
            no_parent(layer);
        }
        std::cout << "navigator_add_branch_desktop_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "navigator_add_branch_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}

#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"
#include "sketch/dxf_exchange.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/boundary_entity.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QComboBox>
#include <QDialog>
#include <QTableWidget>
#include <QTimer>
#include <QSettings>
#include <QDir>
#include <QLabel>
#include <QStandardPaths>
#include <QFontDatabase>
#include <QFont>
#include <QStandardItemModel>
#include <algorithm>
#include <exception>
#include <cmath>
#include <numbers>

#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void setReviewedDestination(QDialog& dialog, int row, const QString& destination, bool use_editor = false) {
    auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
    auto* model = dialog.findChild<QStandardItemModel*>("dxfImportDestinationModel");
    require(table && model && table->item(row, 2), "review destination cell and shared model must exist");
    int option = -1;
    for (int index = 0; index < model->rowCount(); ++index)
        if (model->item(index)->data(Qt::UserRole).toString() == destination) { option = index; break; }
    require(option >= 0, "existing destination must be offered by stable ID");
    if (use_editor) {
        table->setCurrentCell(row, 2);
        table->editItem(table->item(row, 2));
        QApplication::processEvents();
        auto* editor = table->findChild<QComboBox*>("dxfImportDestinationEditor");
        require(editor && editor->model() == model, "actual destination editor must use shared model");
        editor->setCurrentIndex(option);
        require(QMetaObject::invokeMethod(editor, "activated", Qt::DirectConnection, Q_ARG(int, option)),
                "actual editor choice must commit");
        require(table->item(row, 2)->data(Qt::UserRole).toString() == destination,
                "delegate must persist selected destination stable ID");
    } else {
        table->item(row, 2)->setData(Qt::UserRole, destination);
        table->item(row, 2)->setText(model->item(option)->text());
    }
}

template<class Review>
bool reviewedImport(sketch::desktop::MainWindow& window, const QString& path, Review review) {
    QTimer timer;
    timer.setInterval(10);
    bool visited = false;
    std::exception_ptr failure;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "dxfImportLayerDialog") return;
        timer.stop();
        visited = true;
        try { review(*dialog); }
        catch (...) { failure = std::current_exception(); dialog->reject(); }
    });
    timer.start();
    const auto result = window.importDxfWithLayerReview(path);
    timer.stop();
    if (failure) std::rethrow_exception(failure);
    if (!visited) throw std::runtime_error("reviewed DXF import must open the actual layer review modal: " +
                                          window.lastError().toStdString());
    return result;
}
}

void dxfLayerReviewDesktop(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    DxfDrawing drawing;
    drawing.insertion_units = 6;
    drawing.lines = {{{1, 2}, {5, 2}, "Exterior"}, {{7, 8}, {10, 8}, "Interior"}};
    drawing.labels = {{{2, 3}, .2, 30, "Room", "Notes"}};
    const auto bytes = export_dxf_ascii(drawing);
    const auto path = directory + "/layer-review.dxf";
    QFile input(path);
    require(input.open(QIODevice::WriteOnly) && input.write(bytes.data(), static_cast<qint64>(bytes.size())) ==
            static_cast<qint64>(bytes.size()), "layer fixture must be written");
    input.close();
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    const auto lower = window.createLayer("floor-1", "Lower import");
    const auto upper_floor = window.createFloor("building-1", "Upper import");
    const auto upper = window.createLayer(upper_floor, "Upper import");
    require(!lower.isEmpty() && !upper.isEmpty(), "review destinations must exist");
    const auto before = window.document().snapshot();
    require(!reviewedImport(window, path, [](QDialog& dialog) { dialog.reject(); }),
            "cancel must return false");
    require(window.lastError().isEmpty() && window.document().revision() == before.revision() &&
            window.document().snapshot().entities() == before.entities() &&
            window.document().snapshot().assets() == before.assets(), "cancel must preserve document and error state");
    require(reviewedImport(window, path, [&](QDialog& dialog) {
        auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
        require(table && table->rowCount() == 3, "review must list all three source layers including labels");
        require(table->findChildren<QComboBox*>().isEmpty(), "review must not create eager per-row destination editors");
        for (int row = 0; row < table->rowCount(); ++row) {
            require(table->item(row, 1)->text() == "1", "each row must show editable item count");
            const auto target = table->item(row, 0)->text() == "Exterior" ? lower : upper;
            setReviewedDestination(dialog, row, target, row == 0);
        }
        const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            require(dialog.grab().save(capture + "/dxf-layer-review.png"), "review modal must capture");
        }
        dialog.accept();
    }), "reviewed import must succeed");
    const auto imported = window.document().snapshot();
    require(imported.revision() == before.revision() + 1, "reviewed import must be a single revision");
    std::size_t geometry = 0, receipts = 0;
    for (const auto& [id, entity] : imported.entities()) {
        if (before.entities().contains(id)) {
            if (entity.type != kAnnotationEntityType) {
                require(entity == before.entities().at(id), "review must preserve unrelated entities");
                continue;
            }
        }
        if (entity.type == "boundary") {
            ++geometry;
            const auto cad_layer = entity.extensions.at("dxf_source").at("layer").get<std::string>();
            const auto target = cad_layer == "Exterior" ? lower : upper;
            require(entity.properties.at("layer_id") == target.toStdString(), "geometry must use chosen destination");
            require(entity.properties.at("floor_id") == (cad_layer == "Exterior" ? "floor-1" : upper_floor.toStdString()),
                    "geometry must use destination floor");
            const auto& first = entity.properties.at("boundary").at(0).at("start");
            require(first.at(0) == (cad_layer == "Exterior" ? 1 : 7) && first.at(1) == (cad_layer == "Exterior" ? 2 : 8),
                    "mapping between floors must preserve world coordinates");
        } else if (entity.type == kAnnotationEntityType) {
            const auto state = decode_annotation_entity(entity);
            std::size_t original_labels = 0;
            if (const auto old = before.entities().find(id); old != before.entities().end()) {
                const auto& original_state = old->second.properties.at("state");
                original_labels = original_state.at("labels").size();
                require(entity.required == old->second.required &&
                        entity.properties.at("state").at("symbols") == original_state.at("symbols"),
                        "import must preserve original annotation required flag and symbols");
                for (std::size_t index = 0; index < original_labels; ++index)
                    require(entity.properties.at("state").at("labels").at(index) == original_state.at("labels").at(index),
                            "original annotation labels must remain unchanged");
                for (const auto& [key, value] : old->second.extensions.items()) {
                    if (key == "dxf_annotation_layers" && value.is_object()) {
                        for (const auto& [child_id, name] : value.items())
                            require(entity.extensions.at(key).at(child_id) == name,
                                    "original annotation CAD layer lineage must remain unchanged");
                    } else require(entity.extensions.at(key) == value, "original annotation extensions must remain unchanged");
                }
            }
            require(state.labels.size() == original_labels + 1 && state.labels.back().placement.layer_id == upper.toStdString(),
                    "imported label must retain chosen destination");
        } else if (entity.type == "dxf_source") {
            ++receipts;
            require(entity.properties.at("layer_mapping").size() == 3, "receipt must retain reviewed mapping");
            const auto& asset = imported.assets().at(entity.properties.at("asset_id").get<std::string>());
            require(asset.bytes.size() == bytes.size(), "raw source size must survive review");
            for (std::size_t i = 0; i < bytes.size(); ++i)
                require(asset.bytes[i] == static_cast<std::byte>(bytes[i]), "raw source must remain byte exact");
        }
    }
    require(geometry == 2 && receipts == 1, "review must import geometry and retained source");
    const auto reviewed_export = directory + "/reviewed-layers.dxf";
    if (!window.exportDxf(reviewed_export))
        throw std::runtime_error("reviewed drawing must export again: " + window.lastError().toStdString());
    QFile reviewed_file(reviewed_export);
    require(reviewed_file.open(QIODevice::ReadOnly), "reviewed DXF output must open");
    const auto reviewed_bytes = reviewed_file.readAll();
    const auto reviewed_drawing = parse_dxf_ascii(std::string_view(reviewed_bytes.constData(),
                                                                 reviewed_bytes.size()));
    require(std::any_of(reviewed_drawing.drawing.labels.begin(), reviewed_drawing.drawing.labels.end(),
                       [](const auto& label) { return label.text == "Room" && label.layer == "Upper import"; }),
            "exported imported label must use its reviewed native destination layer");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
            window.document().snapshot().assets() == before.assets(), "one undo must restore exact preimport state");
    require(window.redoCommand() && window.document().snapshot().entities() == imported.entities() &&
            window.document().snapshot().assets() == imported.assets(), "redo must restore exact imported state");
    const auto project = directory + "/reviewed.sketch";
    require(window.saveProjectAs(project) && window.createNewProject(), "reviewed import must save and release project");
    MainWindow reopened;
    reopened.setAttribute(Qt::WA_DontShowOnScreen);
    require(reopened.openProject(project) && reopened.document().snapshot().entities() == imported.entities() &&
            reopened.document().snapshot().assets() == imported.assets(), "saved mapping must reopen intact");

    MainWindow defaults;
    defaults.setAttribute(Qt::WA_DontShowOnScreen);
    const auto reused = defaults.createLayer("floor-1", "Exterior");
    require(!reused.isEmpty(), "matching source-named layer must exist");
    const auto default_before = defaults.document().snapshot();
    require(reviewedImport(defaults, path, [](QDialog& dialog) { dialog.accept(); }), "default review must succeed");
    std::size_t created = 0;
    const auto default_imported = defaults.document().snapshot();
    for (const auto& [id, entity] : default_imported.entities())
        if (!default_before.entities().contains(id) && entity.type == "layer") ++created;
    require(created == 2, "matching source-named layer must be reused and unmatched layers created once");
    for (const auto& [id, entity] : default_imported.entities()) {
        (void)id;
        if (entity.type == "boundary" && entity.extensions.at("dxf_source").at("layer") == "Exterior")
            require(entity.properties.at("layer_id") == reused.toStdString(), "default review must reuse matching layer");
    }
    require(defaults.undoCommand() && defaults.document().snapshot().entities() == default_before.entities(),
            "created layers must undo with imported content");
    require(!reviewedImport(defaults, path, [&](QDialog& dialog) {
        require(!defaults.createLayer("floor-1", "Intervening change").isEmpty(), "stale fixture must change document");
        dialog.accept();
    }) && defaults.lastError().contains("changed"), "review must refuse stale document");
    const auto stale = defaults.document().snapshot();
    for (const auto& [id, entity] : stale.entities()) {
        (void)id;
        require(entity.type != "dxf_source" && entity.type != "boundary", "stale review must not add imported content");
    }
    MainWindow replacement;
    replacement.setAttribute(Qt::WA_DontShowOnScreen);
    const auto original_document = replacement.document().snapshot();
    require(!reviewedImport(replacement, path, [&](QDialog& dialog) {
        require(replacement.createNewProject(), "stale fixture must replace document");
        require(replacement.document().revision() == original_document.revision(), "replacement fixture must have same revision");
        dialog.accept();
    }) && replacement.lastError().contains("changed"), "review must refuse a replaced document with the same revision");
    const auto replaced = replacement.document().snapshot();
    require(replaced.entities() == original_document.entities() && replaced.assets() == original_document.assets(),
            "replaced-document refusal must preserve new document");
}

void dxfHostedLayerReviewDesktop(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    MainWindow source;
    source.setAttribute(Qt::WA_DontShowOnScreen);
    source.setMetricUnits(true);
    const auto wall = source.createStraightWall({1, 2}, {9, 2});
    require(!wall.isEmpty() && source.selectEntity(wall), "review fixture must create host");
    require(source.editSelectedElevation("3 m"), "review fixture must use a nonzero host elevation");
    const auto door = source.createHostedOpening("door", "2 m", "0.9 m", "0 m", "2 m");
    require(!door.isEmpty(), "review fixture must create opening");
    const auto expected = source.document().snapshot();
    const auto path = directory + "/host-layer-review.dxf";
    require(source.exportDxf(path), "native host fixture must export");
    QFile input(path);
    require(input.open(QIODevice::ReadOnly), "native host fixture must open");
    auto drawing = parse_dxf_ascii(input.readAll().toStdString()).drawing;
    input.close();
    std::size_t native_count = 0;
    for (auto& insert : drawing.inserts) {
        const auto block = std::find_if(drawing.blocks.begin(), drawing.blocks.end(),
            [&](const auto& item) { return item.name == insert.block_name; });
        require(block != drawing.blocks.end() && !block->vertex_entity_json.empty(), "native fixture must retain metadata");
        const auto payload = nlohmann::json::parse(block->vertex_entity_json);
        // INSERT layer is source placement evidence, independent of the saved native geometry.
        const auto kind = payload.at("type").get<std::string>();
        insert.layer = kind == "wall" ? "Hosts" : "Doors";
        ++native_count;
    }
    require(native_count == 2, "native fixture must have wall and opening inserts");
    const auto bytes = export_dxf_ascii(drawing);
    require(input.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        input.write(bytes.data(), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()),
        "split source-layer fixture must write");
    input.close();
    MainWindow target;
    target.setAttribute(Qt::WA_DontShowOnScreen);
    const auto host_layer = target.createLayer("floor-1", "Host destination");
    const auto door_layer = target.createLayer("floor-1", "Door destination");
    const auto other_floor = target.createFloor("building-1", "Other floor");
    const auto other_layer = target.createLayer(other_floor, "Other destination");
    require(!host_layer.isEmpty() && !door_layer.isEmpty() && !other_layer.isEmpty(), "host review destinations must exist");
    const auto before = target.document().snapshot();
    const auto choose = [&](QDialog& dialog, const QString& opening_destination) {
        auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
        require(table && table->rowCount() == 2, "native review must expose separate source layers");
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto destination = table->item(row, 0)->text() == "Hosts" ? host_layer : opening_destination;
            setReviewedDestination(dialog, row, destination);
        }
        dialog.accept();
    };
    require(!reviewedImport(target, path, [&](QDialog& dialog) { choose(dialog, other_layer); }) &&
            target.lastError().contains("same floor"), "host floor split must fail with actionable error");
    require(target.document().revision() == before.revision() && target.document().snapshot().entities() == before.entities() &&
            target.document().snapshot().assets() == before.assets(), "host floor refusal must leave all state unchanged");
    require(reviewedImport(target, path, [&](QDialog& dialog) { choose(dialog, door_layer); }),
            "same-floor host and opening may use different layers");
    const auto imported = target.document().snapshot();
    std::string imported_wall;
    for (const auto& [id, entity] : imported.entities()) {
        if (before.entities().contains(id)) continue;
        if (entity.type == "wall") {
            imported_wall = id;
            require(entity.properties.at("layer_id") == host_layer.toStdString() &&
                    entity.properties.at("floor_id") == "floor-1", "host must use chosen floor and layer");
            require(entity.properties.at("baseline") == expected.entities().at(wall.toStdString()).properties.at("baseline") &&
                    entity.properties.at("elevation_m") == expected.entities().at(wall.toStdString()).properties.at("elevation_m"),
                    "review must preserve host coordinates and elevation");
        }
    }
    require(!imported_wall.empty(), "native host must reconstruct");
    std::string imported_opening;
    const auto organization = organize_project(imported);
    for (const auto& [id, entity] : imported.entities()) {
        if (before.entities().contains(id) || entity.type != "opening") continue;
        imported_opening = id;
        require(entity.properties.at("layer_id") == door_layer.toStdString() && entity.properties.at("floor_id") == "floor-1" &&
                entity.properties.at("wall_id") == imported_wall, "opening must retain same-floor remapped host");
        const auto context = organization.drawing_context(id);
        require(context && context->complete() && context->layer_id == door_layer.toStdString() &&
                context->floor_id == "floor-1" && organization.nodes.at(id).issues.empty(),
                "same-floor opening layer must resolve a complete drawing context without issues");
    }
    require(!imported_opening.empty() && target.entityVisible(QString::fromStdString(imported_opening)),
            "opening must initially be visible in its destination context");
    require(target.setContainerVisible(door_layer, false) && !target.entityVisible(QString::fromStdString(imported_opening)),
            "opening must hide when its own destination layer is hidden");
    require(target.setContainerVisible(door_layer, true) && target.entityVisible(QString::fromStdString(imported_opening)),
            "opening must show when its own destination layer is shown");
    require(target.setContainerVisible("floor-1", false) && !target.entityVisible(QString::fromStdString(imported_opening)),
            "opening must hide when its host floor is hidden");
    require(target.setContainerVisible("floor-1", true) && target.entityVisible(QString::fromStdString(imported_opening)),
            "opening must show when its host floor is shown");
    require(target.undoCommand() && target.document().snapshot().entities() == before.entities() &&
            target.document().snapshot().assets() == before.assets(), "host and layer mapping must undo atomically");
}

void dxfAnnotationCollisionReviewDesktop(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    DxfDrawing drawing;
    drawing.insertion_units = 6;
    drawing.labels = {{{2, 3}, .2, 0, "Imported text", "Notes"}};
    drawing.dimensions = {{{0, 0}, {4, 0}, {0, 1}, {2, 1}, 0, "Imported dimension", "Dimensions"}};
    const auto bytes = export_dxf_ascii(drawing);
    const auto path = directory + "/annotation-collision.dxf";
    QFile input(path);
    require(input.open(QIODevice::WriteOnly) && input.write(bytes.data(), static_cast<qint64>(bytes.size())) ==
            static_cast<qint64>(bytes.size()), "dimension fixture must write");
    input.close();
    MainWindow target;
    target.setAttribute(Qt::WA_DontShowOnScreen);
    const auto destination = target.createLayer("floor-1", "Review annotations");
    require(!destination.isEmpty(), "annotation destination must exist");
    AnnotationState existing;
    LabelInstance label;
    label.id = "dxf-label-1";
    label.template_id = "dxf";
    label.content = "Existing text";
    label.placement.layer_id = destination.toStdString();
    existing.labels.push_back(label);
    label.id = "dxf-dimension-2";
    label.content = "Existing dimension";
    existing.labels.push_back(label);
    const auto initial = target.document().snapshot();
    const auto annotation_owner = std::find_if(initial.entities().begin(), initial.entities().end(),
        [](const auto& item) { return item.second.type == kAnnotationEntityType; });
    require(annotation_owner != initial.entities().end(), "new project must have its annotation owner");
    auto annotation = make_annotation_entity(annotation_owner->first, existing);
    annotation.required = true;
    annotation.extensions["unrelated_metadata"] = {{"kept", true}};
    (void)target.document().apply(ApplyEntityChanges{target.document().revision(),
        {EntityChange::upsert(annotation)}, {}, "collision fixture"});
    const auto before = target.document().snapshot();
    require(reviewedImport(target, path, [&](QDialog& dialog) {
        auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
        require(table && table->rowCount() == 2, "dimension and text CAD layers must appear");
        auto* notes = dialog.findChild<QLabel*>("dxfImportNotes");
        require(notes && notes->text().contains("original DXF"), "preflight must disclose retained source and import notes");
        for (int row = 0; row < table->rowCount(); ++row) {
            setReviewedDestination(dialog, row, destination);
        }
        dialog.accept();
    }), "dimension collision review must succeed");
    const auto after = target.document().snapshot();
    const auto& updated = after.entities().at(annotation.id);
    require(updated.required && updated.extensions.at("unrelated_metadata") == annotation.extensions.at("unrelated_metadata"),
            "annotation merge must preserve required flag and unrelated extensions");
    const auto merged = decode_annotation_entity(updated);
    require(merged.labels.size() == 4 && merged.labels[0].id == "dxf-label-1" &&
            merged.labels[0].content == "Existing text" && merged.labels[1].id == "dxf-dimension-2" &&
            merged.labels[1].content == "Existing dimension", "existing annotation children must survive collision unchanged");
    std::string imported_dimension;
    for (const auto& item : merged.labels) {
        if (item.content == "Imported dimension") imported_dimension = item.id;
        if (item.content == "Imported dimension" || item.content == "Imported text")
            require(item.placement.layer_id == destination.toStdString() && item.id != "dxf-label-1" && item.id != "dxf-dimension-2",
                    "colliding imported children must receive distinct identities on chosen layer");
    }
    require(!imported_dimension.empty(), "imported dimension child must exist");
    bool linked = false;
    for (const auto& [id, entity] : after.entities()) {
        if (before.entities().contains(id) || entity.type != "boundary") continue;
        linked = true;
        require(entity.extensions.at("dxf_dimension").at("annotation_id") == imported_dimension &&
                entity.extensions.at("dxf_source").at("layer") == "Dimensions" &&
                entity.properties.at("layer_id") == destination.toStdString(),
                "dimension must reference remapped imported child while retaining original CAD layer evidence");
    }
    require(linked && target.undoCommand() && target.document().snapshot().entities() == before.entities() &&
            target.document().snapshot().assets() == before.assets(), "dimension collision import must undo atomically");

    auto unsupported_bytes = export_dxf_ascii(DxfDrawing{});
    const std::string marker = "2\nENTITIES\n";
    const auto position = unsupported_bytes.find(marker);
    require(position != std::string::npos, "unsupported fixture must locate ENTITIES section");
    unsupported_bytes.insert(position + marker.size(), "0\nPOINT\n8\nUnsupported\n10\n1\n20\n2\n");
    const auto unsupported_path = directory + "/retained-only.dxf";
    QFile unsupported(unsupported_path);
    require(unsupported.open(QIODevice::WriteOnly) && unsupported.write(unsupported_bytes.data(),
            static_cast<qint64>(unsupported_bytes.size())) == static_cast<qint64>(unsupported_bytes.size()),
            "unsupported fixture must write");
    unsupported.close();
    require(reviewedImport(target, unsupported_path, [](QDialog& dialog) {
        auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
        auto* notes = dialog.findChild<QLabel*>("dxfImportNotes");
        require(table && table->rowCount() == 0 && notes && notes->text().contains("No editable items"),
                "source-only import must explicitly disclose absence of editable content");
        dialog.accept();
    }), "unsupported-only import must retain original source");
    const auto retained = target.document().snapshot();
    bool receipt = false;
    for (const auto& [id, entity] : retained.entities()) {
        if (before.entities().contains(id) || entity.type != "dxf_source") continue;
        receipt = true;
        require(entity.properties.at("layer_mapping").empty() && !entity.properties.at("diagnostics").empty(),
                "source-only receipt must retain diagnostics without inventing editable layers");
    }
    require(receipt, "source-only import must retain source receipt");
}

void dxfLargeLayerReviewUsesSharedDestinations(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    DxfDrawing drawing;
    drawing.insertion_units = 6;
    constexpr int source_layers = 240;
    constexpr int destination_layers = 120;
    for (int index = 0; index < source_layers; ++index)
        drawing.lines.push_back({{static_cast<double>(index), 0}, {static_cast<double>(index), 1},
            "CAD-" + std::to_string(index)});
    const auto bytes = export_dxf_ascii(drawing);
    const auto path = directory + "/many-cad-layers.dxf";
    QFile input(path);
    require(input.open(QIODevice::WriteOnly) && input.write(bytes.data(), static_cast<qint64>(bytes.size())) ==
            static_cast<qint64>(bytes.size()), "many-layer fixture must write");
    input.close();
    MainWindow target;
    target.setAttribute(Qt::WA_DontShowOnScreen);
    std::vector<EntityChange> layer_changes;
    std::string chosen;
    for (int index = 0; index < destination_layers; ++index) {
        auto entity = Entity::create("layer", {{"floor_id", "floor-1"}, {"name", "Destination " + std::to_string(index)}});
        chosen = entity.id;
        layer_changes.push_back(EntityChange::upsert(std::move(entity)));
    }
    (void)target.document().apply(ApplyEntityChanges{target.document().revision(), std::move(layer_changes), {}, "many-layer fixture"});
    const auto before = target.document().snapshot();
    const auto organization = organize_project(before);
    int valid_layers = 0;
    for (const auto& [id, entity] : before.entities()) {
        const auto context = organization.drawing_context(id);
        if (entity.type == "layer" && context && context->complete()) ++valid_layers;
    }
    require(!reviewedImport(target, path, [&](QDialog& dialog) {
        auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
        auto* model = dialog.findChild<QStandardItemModel*>("dxfImportDestinationModel");
        require(table && table->rowCount() == source_layers && model && model->rowCount() == valid_layers + 1,
                "large review must populate one destination model independently of source-layer count");
        require(dialog.findChildren<QStandardItemModel*>("dxfImportDestinationModel").size() == 1 &&
                table->findChildren<QComboBox*>().isEmpty(), "large review must have one model and no eager combo editors");
        for (int row = 0; row < table->rowCount(); ++row)
            require(!table->cellWidget(row, 2) && table->item(row, 2)->data(Qt::UserRole).isValid(),
                    "each destination row must store a stable choice without allocating widgets or copied models");
        setReviewedDestination(dialog, source_layers - 1, QString::fromStdString(chosen), true);
        require(model->rowCount() == valid_layers + 1, "editing a source row must not duplicate destination population");
        dialog.reject();
    }), "large review cancellation must return false");
    require(target.document().revision() == before.revision() && target.document().snapshot().entities() == before.entities() &&
            target.document().snapshot().assets() == before.assets(), "large review cancellation must remain pure");
}

void dxfOpeningProfileDesktopRoundtrip(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    MainWindow source;
    source.setMetricUnits(true);
    const auto wall = source.createCurvedWall({0, 0}, {8, 0}, "0.5 rad");
    require(!wall.isEmpty() && source.selectEntity(wall),
            "profile roundtrip curved host must be created");
    require(source.editSelectedThickness("0.3 m"), "profile host thickness must be configured");
    const auto door = source.createHostedOpening("door", "1 m", "0.9 m", "0 m", "2 m",
        std::nullopt, DoorOperation{true, false, 67.0});
    require(!door.isEmpty() && source.selectEntity(door) &&
            source.editSelectedOpeningAssembly("0.08 m", "0.25 m", "0.035 m", "0.012 m", "-0.015 m"),
            "profile roundtrip door assembly must be configured");
    const auto window_wall = source.createStraightWall({0, 5}, {12, 5});
    require(!window_wall.isEmpty() && source.selectEntity(window_wall) &&
            source.editSelectedThickness("0.3 m"), "second profile host must be created");
    const auto window = source.createHostedOpening("window", "9 m", "1 m", "0.8 m", "1.2 m");
    require(!window.isEmpty() && source.selectEntity(window) &&
            source.editSelectedOpeningAssembly("0.07 m", "0.24 m", "0.03 m", "0.014 m", "0.01 m"),
            "profile roundtrip window assembly must be configured");
    const auto original = source.document().snapshot();
    const auto path = directory + "/opening-profiles.dxf";
    require(source.exportDxf(path), "profile fixture must export");

    MainWindow destination;
    destination.setMetricUnits(true);
    const auto unrelated_wall = destination.createStraightWall({0, 10}, {3, 10});
    require(!unrelated_wall.isEmpty() && destination.selectEntity(unrelated_wall) &&
            !destination.createHostedOpening("door", "1 m", "0.8 m", "0 m", "2 m").isEmpty(),
            "unrelated target wall and opening must be created");
    const auto floor = destination.createFloor("building-1", "Imported profiles");
    const auto layer = destination.createLayer(floor, "Imported openings");
    require(!floor.isEmpty() && !layer.isEmpty() && destination.setActiveLayer(layer),
            "profile import destination floor and layer must be configured");
    const auto initial = destination.document().snapshot();
    const auto inspect = [&](const auto& snapshot) {
        std::size_t host_count = 0;
        std::map<std::string, std::string> openings;
        for (const auto& [id, entity] : snapshot.entities()) {
            if (initial.entities().contains(id)) {
                require(entity == initial.entities().at(id), "import must preserve unrelated target entities");
                continue;
            }
            if (entity.type != "wall" && entity.type != "opening") continue;
            require(entity.properties.at("floor_id") == floor.toStdString() &&
                    entity.properties.at("layer_id") == layer.toStdString(),
                    "imported profile objects must use the active destination floor and layer");
            require(!entity.required && !original.entities().contains(id),
                    "imported profile objects must be editable and receive fresh identities");
            if (entity.type == "wall") ++host_count;
            else openings.emplace(entity.properties.at("opening_kind").template get<std::string>(), id);
        }
        require(host_count == 2 && openings.size() == 2 && openings.contains("door") &&
                openings.contains("window"), "profile import must restore host, door, and window");
        for (const auto& [kind, id] : openings) {
            const auto& opening = snapshot.entities().at(id);
            const auto& expected = original.entities().at((kind == "door" ? door : window).toStdString());
            const auto host = opening.properties.at("wall_id").template get<std::string>();
            require(!initial.entities().contains(host) && snapshot.entities().at(host).type == "wall" &&
                    snapshot.entities().at(host).properties.at("baseline") ==
                        original.entities().at((kind == "door" ? wall : window_wall).toStdString()).properties.at("baseline"),
                    "each profile opening must reference its own remapped host with preserved baseline");
            require(opening.properties.at("offset_m") == expected.properties.at("offset_m"),
                    "each imported opening must retain its own host station");
            require(opening.properties.at("opening_assembly") == expected.properties.at("opening_assembly"),
                    "configured opening assembly must survive desktop exchange");
            if (kind == "door")
                require(opening.properties.at("door_operation") == expected.properties.at("door_operation"),
                        "configured door handing and angle must survive desktop exchange");
        }
        return openings;
    };
    if (!destination.importDxf(path))
        throw std::runtime_error("profile fixture must import: " + destination.lastError().toStdString());
    const auto first = inspect(destination.document().snapshot());
    require(destination.undoCommand(), "profile import must undo");
    require(destination.document().snapshot().entities() == initial.entities() &&
            destination.document().snapshot().assets() == initial.assets(),
            "profile import undo must restore destination objects and assets");
    if (!destination.importDxf(path))
        throw std::runtime_error("profile fixture must reimport after undo: " + destination.lastError().toStdString());
    const auto imported = inspect(destination.document().snapshot());
    for (const auto& [kind, id] : imported)
        require(id != first.at(kind), "reimport after undo must allocate fresh opening identities");
    const auto project = directory + "/opening-profiles-dxf.sketch";
    require(destination.saveProjectAs(project), "profile project must save");
    const auto expected_saved = destination.document().snapshot();
    require(destination.createNewProject(), "saved project ownership must be released before reopen");
    MainWindow reopened;
    require(reopened.openProject(project), "profile project must reopen");
    require(reopened.setActiveLayer(layer), "reopened imported profile layer must be activated");
    const auto saved = reopened.document().snapshot();
    inspect(saved);
    require(saved.entities() == expected_saved.entities() &&
            saved.assets() == expected_saved.assets(),
            "profile entities, hosts, assignments, and retained source must survive reopening");
    for (const auto& [kind, id] : imported) {
        (void)kind;
        if (!reopened.selectEntity(QString::fromStdString(id)) ||
                !reopened.editSelectedOpeningAssembly("0.06 m", "0.23 m", "0.025 m", "0.01 m", "0 m"))
            throw std::runtime_error("reopened " + kind + " profile must support semantic assembly editing: " +
                                     reopened.lastError().toStdString());
    }
}

void dxfCircleInsertDesktopWorkflow(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    struct Case {
        const char* xscale; const char* yscale; const char* radius; bool supported;
        const char* source_z{"0"}; const char* insert_z{"0"};
        const char* source_tags{""}; const char* insert_tags{""};
    };
    const Case cases[]{{"2", "2", "2", true}, {"-2", "2", "2", true},
                       {"0.00001", "0.00002", "100000", false}, {"1", "1.000000000001", "2", false},
                       {"-1", "1", "2", false, "0", "0", "210\n0\n220\n0\n230\n-1\n"},
                       {"1", "1", "2", false, "0", "0", "210\n0\n220\n1\n230\n0\n", "210\n0\n220\n1\n230\n0\n"},
                       {"1", "1", "-2", false}, {"1", "1", "2", false, "2", "-2"}};
    int index = 0;
    for (const auto& item : cases) {
        QByteArray bytes =
            "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1027\n9\n$INSUNITS\n70\n6\n0\nENDSEC\n"
            "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n100\nAcDbEntity\n8\n0\n100\nAcDbBlockBegin\n2\nROUND\n70\n0\n10\n0\n20\n0\n30\n0\n"
            "0\nCIRCLE\n100\nAcDbEntity\n8\n0\n100\nAcDbCircle\n10\n1\n20\n2\n30\n@sourcez@\n40\n@radius@\n@sourcetags@"
            "0\nENDBLK\n100\nAcDbEntity\n8\n0\n100\nAcDbBlockEnd\n0\nENDSEC\n"
            "0\nSECTION\n2\nENTITIES\n0\nINSERT\n100\nAcDbEntity\n8\nRound inserts\n100\nAcDbBlockReference\n2\nROUND\n10\n10\n20\n20\n30\n@insertz@\n41\n@xscale@\n42\n@yscale@\n43\n1\n50\n90\n@inserttags@0\nENDSEC\n0\nEOF\n";
        bytes.replace("@radius@", item.radius).replace("@xscale@", item.xscale).replace("@yscale@", item.yscale);
        bytes.replace("@sourcez@", item.source_z).replace("@insertz@", item.insert_z)
             .replace("@sourcetags@", item.source_tags).replace("@inserttags@", item.insert_tags);
        const auto path = directory + QStringLiteral("/circle-insert-%1.dxf").arg(index++);
        QFile input(path);
        require(input.open(QIODevice::WriteOnly) && input.write(bytes) == bytes.size(), "circle INSERT fixture must save");
        input.close();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.setMetricUnits(true);
        require(reviewedImport(window, path, [&](QDialog& dialog) {
            auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
            require(table && table->rowCount() == (item.supported ? 1 : 0),
                    "circle INSERT layer review must reject unequal scales and unsupported source geometry");
            if (item.supported) require(table->item(0, 0)->text() == "Round inserts" && table->item(0, 1)->text() == "1",
                                       "uniform/reflected circle INSERT must offer editable geometry");
            dialog.accept();
        }), "uniform/reflected circle INSERT must import through the sandboxed normalizer");
        std::size_t circles = 0;
        bool source_retained = false;
        const auto snapshot = window.document().snapshot();
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type == "dxf_source") {
                const auto& asset = snapshot.assets().at(entity.properties.at("asset_id").get<std::string>());
                require(asset.bytes.size() == static_cast<std::size_t>(bytes.size()) &&
                        std::equal(asset.bytes.begin(), asset.bytes.end(), reinterpret_cast<const std::byte*>(bytes.constData())),
                        "circle INSERT original source must remain byte exact");
                source_retained = true;
                if (!item.supported) require(!entity.properties.at("diagnostics").empty(),
                                             "unsupported circle must retain fidelity diagnostics");
            }
            if (entity.type != "boundary" || entity.properties.value("classification", "") != "dxf_circle") continue;
            ++circles;
            const auto geometry = boundary_geometry(decode_identified_boundary_entity(upgrade_legacy_boundary_entity(entity)));
            require(geometry.size() == 2 && validate_boundary(geometry).empty(), "normalized circle INSERT must remain analytical and closed");
            const Vec2 center{(geometry.front().start.x + geometry.front().end.x) * 0.5,
                              (geometry.front().start.y + geometry.front().end.y) * 0.5};
            require(std::abs(center.x - 6.0) < 1e-8 && std::abs(center.y - (item.xscale[0] == '-' ? 18.0 : 22.0)) < 1e-8 &&
                    std::abs(std::abs(signed_area(geometry)) - 16 * std::numbers::pi) < 1e-8 &&
                    std::abs(perimeter(geometry) - 8 * std::numbers::pi) < 1e-8,
                    "normalized uniform/reflected circle INSERT must retain exact world geometry");
        }
        require(circles == (item.supported ? 1U : 0U) && source_retained,
                "circle INSERT must retain source and admit only exact uniform geometry");
    }
}

void dxfCircleDesktopWorkflow(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    // Independent standard CIRCLE record, in millimetres; not the product writer.
    const QByteArray bytes =
        "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1027\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n0\nCIRCLE\n100\nAcDbEntity\n8\nRound pads\n"
        "100\nAcDbCircle\n10\n3000\n20\n-1000\n30\n0\n40\n2000\n0\nENDSEC\n0\nEOF\n";
    const auto path = directory + "/circle-mm.dxf";
    QFile input(path);
    require(input.open(QIODevice::WriteOnly) && input.write(bytes) == bytes.size(), "circle source must save");
    input.close();
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    const auto upper_floor = window.createFloor("building-1", "Round fixtures");
    const auto upper_layer = window.createLayer(upper_floor, "Round fixtures");
    require(!upper_floor.isEmpty() && !upper_layer.isEmpty(), "circle destination must exist");
    const auto before = window.document().snapshot();
    require(reviewedImport(window, path, [&](QDialog& dialog) {
        auto* table = dialog.findChild<QTableWidget*>("dxfImportLayerTable");
        require(table && table->rowCount() == 1 && table->item(0, 0)->text() == "Round pads" &&
                table->item(0, 1)->text() == "1", "circle must be offered as one editable layer item");
        setReviewedDestination(dialog, 0, upper_layer, true);
        dialog.accept();
    }), "native sandboxed circle import must succeed");
    const auto imported = window.document().snapshot();
    require(imported.revision() == before.revision() + 1, "circle import must be one transaction");
    QString circle_id;
    bool retained = false;
    for (const auto& [id, entity] : imported.entities()) {
        if (entity.type == "boundary" && entity.properties.value("classification", "") == "dxf_circle") {
            require(circle_id.isEmpty(), "one circle must create one boundary");
            circle_id = QString::fromStdString(id);
            require(entity.properties.at("floor_id") == upper_floor.toStdString() &&
                    entity.properties.at("layer_id") == upper_layer.toStdString(), "circle must follow reviewed destination");
            const auto geometry = boundary_geometry(decode_identified_boundary_entity(upgrade_legacy_boundary_entity(entity)));
            require(validate_boundary(geometry).empty() && std::abs(signed_area(geometry) - 4 * std::numbers::pi) < 1e-8 &&
                    std::abs(perimeter(geometry) - 4 * std::numbers::pi) < 1e-8,
                    "circle millimetres must normalize into exact two-metre radius area and perimeter");
        } else if (entity.type == "dxf_source") {
            const auto& asset = imported.assets().at(entity.properties.at("asset_id").get<std::string>());
            require(asset.bytes.size() == static_cast<std::size_t>(bytes.size()), "circle source size must survive");
            for (qsizetype i = 0; i < bytes.size(); ++i)
                require(asset.bytes[static_cast<std::size_t>(i)] == static_cast<std::byte>(bytes[i]),
                        "circle source must remain byte exact");
            require(entity.properties.value("isolated_import", false), "circle must pass actual isolated importer");
            retained = true;
        }
    }
    require(!circle_id.isEmpty() && retained, "editable circle and source receipt must both exist");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
            window.document().snapshot().assets() == before.assets(), "circle import undo must restore source state");
    require(window.redoCommand() && window.document().snapshot().entities() == imported.entities() &&
            window.document().snapshot().assets() == imported.assets(), "circle import redo must restore exact candidate");
    require(window.selectEntity(circle_id) && window.upgradeSelectedBoundaryIdentities(),
            "imported circle must expose existing typed editing upgrade");
    const auto editable = window.document().snapshot();
    const auto model = decode_identified_boundary_entity(editable.entities().at(circle_id.toStdString()));
    require(window.editSelectedBoundaryEdgeLength(QString::fromStdString(model.segments.front().segment_id),
                                                  "7 m", BoundaryFixedEndpoint::start, true),
            "imported curve edge must support exact physical-length editing");
    require(window.undoCommand() && window.document().snapshot().entities() == editable.entities(),
            "typed curve length undo must restore circular geometry");
    const auto exported = directory + "/circle-export.dxf";
    if (!window.exportDxf(exported))
        throw std::runtime_error("editable circle must export analytically: " + window.lastError().toStdString());
    QFile output(exported);
    require(output.open(QIODevice::ReadOnly), "circle DXF export must open");
    const auto exported_bytes = output.readAll();
    const auto parsed = parse_dxf_ascii(std::string_view(exported_bytes.constData(), exported_bytes.size()));
    require(parsed.diagnostics.empty() && parsed.drawing.polylines.size() == 1 &&
            parsed.drawing.polylines.front().closed && parsed.drawing.polylines.front().vertices.size() == 2,
            "circle export must retain closed exact two-bulge geometry");
    for (const auto& vertex : parsed.drawing.polylines.front().vertices)
        require(std::abs(vertex.bulge - 1.0) < 1e-12, "circle export must preserve each analytic semicircle");
    const auto project = directory + "/editable-circle.sketch";
    require(window.saveProjectAs(project), "editable circle must save");
    MainWindow reopened;
    reopened.setAttribute(Qt::WA_DontShowOnScreen);
    require(reopened.openProject(project) && reopened.document().snapshot().entities() == editable.entities() &&
            reopened.document().snapshot().assets() == editable.assets(), "circle geometry, identities and source must reopen exactly");
    require(reopened.selectEntity(circle_id), "reopened circle must remain selectable");
    const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        QDir().mkpath(capture);
        reopened.resize(1180, 780);
        reopened.show();
        QApplication::processEvents();
        require(reopened.selectEntity(circle_id), "rendered circle must be selected after window initialization");
        auto* canvas = dynamic_cast<PlanCanvas*>(reopened.findChild<QWidget*>("measurementPlanCanvas"));
        require(canvas, "circle drawing canvas must exist");
        canvas->fitView();
        QApplication::processEvents();
        require(reopened.grab().save(capture + "/dxf-circle-import.png"), "circle native UI must capture");
        reopened.hide();
    }
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    try {
        using namespace sketch;
        using namespace sketch::desktop;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory must be available");
        QCoreApplication::setOrganizationName(QStringLiteral("VertexTests"));
        QCoreApplication::setApplicationName(QStringLiteral("Vertex-dxf-desktop-") + QFileInfo(temporary.path()).fileName());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path() + "/settings");
        if (!qEnvironmentVariableIsEmpty("VERTEX_TEST_CAPTURE_DIR")) {
            require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                    "bundled font must load for DXF modal capture");
            application.setFont(QFont(QStringLiteral("Inter"), 10));
        }
        dxfLayerReviewDesktop(temporary.path());
        dxfHostedLayerReviewDesktop(temporary.path());
        dxfAnnotationCollisionReviewDesktop(temporary.path());
        dxfLargeLayerReviewUsesSharedDestinations(temporary.path());
        dxfOpeningProfileDesktopRoundtrip(temporary.path());
        dxfCircleDesktopWorkflow(temporary.path());
        dxfCircleInsertDesktopWorkflow(temporary.path());
        MainWindow source;
        const auto boundary_id = source.createBoundary({
            {{0.0, 0.0}, {4.0, 0.0}, 0.0},
            {{4.0, 0.0}, {4.0, 3.0}, 0.0},
            {{4.0, 3.0}, {0.0, 3.0}, 0.0},
            {{0.0, 3.0}, {0.0, 0.0}, 0.0}});
        require(!boundary_id.isEmpty(), "source boundary must be created");
        const auto path = temporary.filePath(QStringLiteral("mapped.dxf"));
        require(source.exportDxf(path), "native project must export DXF");
        require(QFileInfo::exists(path) && QFileInfo(path).size() > 0,
                "DXF export must write bytes");
        require(QFileInfo::exists(path + QStringLiteral(".fidelity.json")),
                "DXF export must write a fidelity report");

        MainWindow destination;
        const auto before = destination.document().revision();
        if (!destination.importDxf(path))
            throw std::runtime_error(
                "native project must import DXF: " + destination.lastError().toStdString());
        require(destination.document().revision() == before + 1,
                "DXF import must be one document revision");
        const auto snapshot = destination.document().snapshot();
        bool imported_boundary = false;
        bool retained_source = false;
        for (const auto& [id, entity] : snapshot.entities()) {
            (void)id;
            if (entity.type == "boundary" && entity.properties.value("classification", "") ==
                    "dxf_polyline_closed") imported_boundary = true;
            if (entity.type == "dxf_source") {
                retained_source = true;
                const auto asset_id = entity.properties.value("asset_id", "");
                require(snapshot.assets().contains(asset_id), "DXF source asset must be retained");
                require(entity.properties.value("isolated_import", false),
                        "DXF source receipt must attest isolated worker parsing");
            }
        }
        require(imported_boundary, "DXF import must create a mapped boundary");
        require(retained_source, "DXF import must retain source provenance");
        require(destination.undoCommand(), "DXF import must be undoable");
        const auto restored = destination.document().snapshot();
        const auto invalid_path = temporary.filePath(QStringLiteral("invalid.dxf"));
        {
            QFile invalid(invalid_path);
            require(invalid.open(QIODevice::WriteOnly) &&
                        invalid.write("0\nSECTION\n2\nENTITIES\n0\nENDSEC\n") > 0,
                    "invalid DXF fixture must be written");
        }
        require(!destination.importDxf(invalid_path), "malformed DXF must fail closed");
        require(destination.document().revision() == restored.revision() &&
                    destination.document().snapshot().entities() == restored.entities() &&
                    destination.document().snapshot().assets() == restored.assets(),
                "failed isolated DXF import must leave the document unchanged");
        std::cout << "DXF desktop workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

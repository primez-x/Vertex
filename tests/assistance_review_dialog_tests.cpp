#include "sketch/desktop/main_window.hpp"
#include "sketch/assistance_engine.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/project_organization.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/trusted_reference_fixture.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
template<class T> T* control(QDialog& dialog, const char* name) {
    auto* result = dynamic_cast<T*>(dialog.findChild<QWidget*>(QString::fromLatin1(name)));
    require(result != nullptr, "required assistance control is missing");
    return result;
}
void choose(QComboBox* combo, const QString& id) {
    const auto index = combo->findData(id);
    require(index >= 0, "expected choice must be available in the dialog");
    combo->setCurrentIndex(index);
}
void drive(MainWindow& window, const std::function<void(QDialog&)>& action) {
    std::exception_ptr failure;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("assistanceDialog");
        try {
            require(dialog && dialog->isVisible(), "actual modal assistance dialog must open");
            action(*dialog);
        } catch (...) { failure = std::current_exception(); }
        if (dialog) dialog->reject();
        else if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
    });
    window.showAssistance();
    if (failure) std::rethrow_exception(failure);
}
struct Fixture {
    QString first, second;
    Entity target;
};
Fixture seed(MainWindow& window, const QString& directory) {
    // Caller-owned pixels and embedded text are trusted UI fixtures. This is
    // neither production OCR nor AppContainer installation qualification.
    Fixture fixture;
    for (int number = 0; number != 2; ++number) {
        QImage image(100, 60, QImage::Format_ARGB32);
        image.fill(number == 0 ? Qt::white : Qt::lightGray);
        image.setPixelColor(15, 15, Qt::black);
        const auto path = QDir(directory).filePath(QString("review-source-%1.png").arg(number));
        require(image.save(path, "PNG"), "trusted source pixels must save");
        const auto id = testing::importOrSeedTrustedReferenceFixture(window, path, image);
        auto reference = window.document().snapshot().entities().at(id.toStdString());
        reference.properties["name"] = number == 0 ? "Review source A" : "Review source B";
        reference.properties["source_text_version"] = 1;
        reference.properties["source_text"] = "Wall: 31 ft 6 in";
        reference.properties["source_text_runs"] = nlohmann::json::array({
            {{"offset", std::size_t{0}}, {"length", std::size_t{16}},
             {"x", 0.1}, {"y", 0.2}, {"width", 0.5}, {"height", 0.1}}});
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(reference)}, {}, "Seed trusted review text"});
        require(window.calibrateReference(id, "0", "0", "100", "0", "1 m"),
                "review source must have validated calibration provenance");
        (number == 0 ? fixture.first : fixture.second) = id;
    }
    MeasurementLinework model;
    model.stroke_id = "dialog-measured-line";
    model.anchor = {0, 0};
    for (int edge = 0; edge != 2; ++edge) {
        ConstructionReceipt receipt;
        receipt.segment_id = edge == 0 ? "dialog-edge-a" : "dialog-edge-b";
        receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.start = edge == 0 ? Vec2{0, 0} : Vec2{10, 0};
        receipt.chord_end = edge == 0 ? Vec2{10, 0} : Vec2{10, 5};
        model.edges.push_back({receipt.segment_id, edge == 0 ? "dialog-start" : "dialog-join",
            edge == 0 ? "dialog-join" : "dialog-end", receipt});
    }
    fixture.target.id = model.stroke_id;
    fixture.target.type = "measurement_linework";
    fixture.target.required = true;
    fixture.target.properties = {{"model", encode_measurement_linework_model(model)}, {"name", "Reviewed measured line"}};
    const auto context = organize_project(window.document().snapshot()).drawing_context(
        window.activeLayerId().toStdString());
    require(context && context->complete(), "review fixture must use a complete active drawing context");
    fixture.target.properties["property_id"] = context->property_id;
    fixture.target.properties["building_id"] = context->building_id;
    fixture.target.properties["floor_id"] = context->floor_id;
    fixture.target.properties["layer_id"] = context->layer_id;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(fixture.target)}, {}, "Seed measured review target"});
    window.setAssistanceEnabled(true);
    return fixture;
}
void configure(QDialog& dialog, const Fixture& fixture) {
    auto* kind = control<QComboBox>(dialog, "assistanceKind");
    kind->setCurrentIndex(kind->findData(static_cast<int>(AssistanceKind::dimension_extraction)));
    choose(control<QComboBox>(dialog, "assistanceReference"), fixture.first);
    choose(control<QComboBox>(dialog, "assistanceDimensionTarget"), QString::fromStdString(fixture.target.id));
    choose(control<QComboBox>(dialog, "assistanceDimensionSegment"), "dialog-edge-a");
}
void generate(QDialog& dialog) {
    control<QPushButton>(dialog, "assistanceGenerate")->click();
    if (control<QListWidget>(dialog, "assistanceProposalList")->count() != 1)
        std::cerr << "Assistance review proposal count: "
                  << control<QListWidget>(dialog, "assistanceProposalList")->count()
                  << "; status: " << control<QLabel>(dialog, "assistanceStatus")->text().toStdString() << '\n';
    require(control<QListWidget>(dialog, "assistanceProposalList")->count() == 1,
            "Generate button must produce one reviewed suggestion");
    require(control<QPushButton>(dialog, "assistanceAccept")->isEnabled(), "generated suggestion must be selectable");
}
void empty(QDialog& dialog) {
    require(control<QListWidget>(dialog, "assistanceProposalList")->count() == 0 &&
        !control<QPushButton>(dialog, "assistanceAccept")->isEnabled(),
        "changed options must clear suggestions and disable acceptance");
}
void contour_review(const QString& directory, bool ring) {
    MainWindow window;
    QImage pixels(32, 32, QImage::Format_ARGB32);
    pixels.fill(Qt::white);
    for (int y = 3; y <= 22; ++y)
        for (int x = 3; x <= 24; ++x)
            if (ring ? (x == 3 || x == 24 || y == 3 || y == 22) : (x <= 8 || y >= 17))
                pixels.setPixelColor(x, y, Qt::black);
    const auto path = QDir(directory).filePath(ring ? "review-ring.png" : "review-concave.png");
    require(pixels.save(path, "PNG"), "contour source pixels must save");
    const auto id = testing::importOrSeedTrustedReferenceFixture(window, path, pixels);
    auto named_reference = window.document().snapshot().entities().at(id.toStdString());
    const std::string source_name = ring ? "Outlined plan" : "Concave plan";
    named_reference.properties["name"] = source_name;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(std::move(named_reference))}, {}, "Name the trace review source"});
    require(window.calibrateReference(id, "0", "0", "20", "0", "1 m"),
            "contour review source must calibrate through the real API");
    require(window.editReferenceTransform(id, "6", "-3", "0.05", "1.7", "37", "0.65",
                true, ring, true), "contour review must retain a nontrivial source transform");
    window.setAssistanceEnabled(true);
    const auto before = document_snapshot_digest(window.document().snapshot());
    const auto proposals = window.suggestReferenceAssistance(id, AssistanceKind::edge_tracing);
    require(proposals.size() == 1, "connected contour fixture must produce one proposal");
    const auto& arguments = proposals.front().preview.arguments;
    require(arguments.at("points").size() == (ring ? 4u : 6u) &&
                arguments.at("holes").size() == (ring ? 1u : 0u),
            "real proposal must preserve the known concavity or enclosed void");
    QTransform source_to_model;
    source_to_model.translate(6, -3);
    source_to_model.rotate(37);
    source_to_model.scale(-0.085, ring ? -0.085 : 0.085);
    source_to_model.translate(-16, -16);
    const std::vector<QPointF> corners = ring
        ? std::vector<QPointF>{{3,3}, {25,3}, {25,23}, {3,23}}
        : std::vector<QPointF>{{3,3}, {9,3}, {9,17}, {25,17}, {25,23}, {3,23}};
    for (const auto& corner : corners) {
        const auto expected = source_to_model.map(corner);
        bool found = false;
        for (const auto& point : arguments.at("points"))
            found |= std::abs(point.at(0).get<double>() - expected.x()) < 1e-10 &&
                     std::abs(point.at(1).get<double>() - expected.y()) < 1e-10;
        require(found, "real contour must use independently transformed source corners");
    }
    drive(window, [&](QDialog& dialog) {
        auto* kind = control<QComboBox>(dialog, "assistanceKind");
        kind->setCurrentIndex(kind->findData(static_cast<int>(AssistanceKind::edge_tracing)));
        choose(control<QComboBox>(dialog, "assistanceReference"), id);
        generate(dialog);
        dialog.layout()->activate();
        QApplication::processEvents();
        require(control<QListWidget>(dialog, "assistanceProposalList")->currentItem()->text()
                    .contains(QString::fromStdString(source_name)) &&
                control<QLabel>(dialog, "assistanceReviewDetails")->text()
                    .contains(QString::fromStdString(source_name)),
                "trace review must identify the selected plan by its readable source name");
        auto* canvas = control<PlanCanvas>(dialog, "assistanceReviewCanvas");
        require(canvas->isVisible() && canvas->references().size() == 1,
                "contour review must visibly retain its source reference");
        const auto& reference = canvas->references().front();
        require(same_point(reference.position, {6,-3}) && reference.metres_per_source_unit == 0.05 &&
                    reference.scale == 1.7 && reference.rotation_degrees == 37 &&
                    reference.flip_horizontal && reference.flip_vertical == ring &&
                    reference.intensity == 0.65 && reference.visible,
                "review reference must exactly retain calibrated source placement and presentation");
        require(reference.image.size() == pixels.size(), "review must retain source image dimensions");
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                require(reference.image.pixelColor(x,y) == pixels.pixelColor(x,y),
                        "review must retain every selected source pixel");
        require(canvas->entities().size() == 1 + arguments.at("holes").size(),
                "review must contain one actual outer contour and each distinct hole boundary");
        const auto check_contour = [&](const CanvasEntity& entity, const nlohmann::json& points) {
            require(entity.type == "boundary" && entity.stroke_color == QColor("#1671f5") &&
                        entity.segments.size() == points.size(),
                    "review must show blue actual contour boundaries without a bounding-box substitute");
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto point = [&](std::size_t index) {
                    return Vec2{points.at(index).at(0).get<double>(), points.at(index).at(1).get<double>()};
                };
                require(same_point(entity.segments[i].start, point(i)) &&
                            same_point(entity.segments[i].end, point((i+1) % points.size())) &&
                            entity.segments[i].sweep_radians == 0,
                        "review segments must exactly match the proposed model contour in order");
            }
        };
        check_contour(canvas->entities().front(), arguments.at("points"));
        for (std::size_t hole = 0; hole < arguments.at("holes").size(); ++hole)
            check_contour(canvas->entities()[hole+1], arguments.at("holes").at(hole));
        require(document_snapshot_digest(window.document().snapshot()) == before,
                "generating and previewing actual contours must not mutate the source");
        const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            require(QDir().mkpath(capture), "contour capture directory must exist");
            require(dialog.grab().save(QDir(capture).filePath(ring
                        ? "assistance-ring-contour-review.png" : "assistance-concave-contour-review.png")),
                    "actual transformed contour dialog capture must save");
        }
    });
    require(document_snapshot_digest(window.document().snapshot()) == before,
            "Cancel after contour review must preserve the complete source document");
}
void label_and_command_review(const QString& directory) {
    MainWindow window;
    auto fixture = seed(window, directory);
    const auto decoded = decode_measurement_linework_model(fixture.target.properties.at("model"));
    require(decoded.model.has_value(), "named measured label fixture must decode");
    PlanarTransform translation;
    translation.offset = {12, -7};
    const auto model = transformed_measurement_linework(*decoded.model, translation);
    fixture.target.properties["model"] = encode_measurement_linework_model(model);
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(fixture.target)}, {}, "Move named label anchor"});
    const auto before = window.document().snapshot();
    drive(window, [&](QDialog& dialog) {
        auto* kind = control<QComboBox>(dialog, "assistanceKind");
        kind->setCurrentIndex(kind->findData(static_cast<int>(AssistanceKind::label_placement)));
        control<QPushButton>(dialog, "assistanceGenerate")->click();
        auto* list = control<QListWidget>(dialog, "assistanceProposalList");
        require(list->count() >= 2, "named objects must produce individually reviewed label suggestions");
        int target_row = -1;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->text().contains("Reviewed measured line")) target_row = row;
        require(target_row >= 0, "named measured line must have a label proposal");
        list->setCurrentRow(target_row);
        auto* canvas = control<PlanCanvas>(dialog, "assistanceReviewCanvas");
        require(canvas->references().empty() && canvas->labels().size() == 1 &&
            canvas->labels().front().text == "Reviewed measured line" &&
            same_point(canvas->labels().front().position, {12, -7}) && !canvas->entities().empty(),
            "label review must show readable label content at its model anchor with target geometry");
        require(control<QLabel>(dialog, "assistanceReviewDetails")->text().contains("Reviewed measured line"),
            "label review must explain its content and owning object");
        auto* accept = control<QPushButton>(dialog, "assistanceAccept");
        require(accept->isEnabled(), "an object source ID must not disable label acceptance");
        const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            require(QDir().mkpath(capture), "label capture directory must exist");
            require(dialog.grab().save(QDir(capture).filePath("assistance-label-review.png")),
                    "actual label review capture must save");
        }
        accept->click();
        require(window.document().revision() == before.revision() + 1 && list->count() >= 1 && accept->isEnabled(),
            "one label acceptance must be one revision and restamp remaining owned proposals");
        kind->setCurrentIndex(kind->findData(static_cast<int>(AssistanceKind::natural_language)));
        empty(dialog);
    });
    const auto accepted = window.document().snapshot();
    bool found = false;
    for (const auto& [id, entity] : accepted.entities()) {
        if (entity.type != kAnnotationEntityType) continue;
        for (const auto& label : decode_annotation_entity(entity).labels) {
            if (label.content != "Reviewed measured line") continue;
            found = true;
            require(same_point(label.placement.position, {12, -7}) && label.model_plan &&
                entity.extensions.at("assistance_label_provenance").at(label.id).at("proposal")
                    .at("preview").at("arguments").at("anchor_entity_id") == fixture.target.id,
                "accepted label must retain model position and exact reviewed source provenance");
        }
    }
    require(found && accepted.entities().at(fixture.target.id) == fixture.target && accepted.assets() == before.assets(),
        "label acceptance must preserve the authoritative source geometry and assets");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(),
        "one Undo must remove the reviewed label and its provenance");
    require(window.redoCommand() && window.document().snapshot().entities() == accepted.entities(),
        "one Redo must restore the reviewed label and provenance exactly");
    for (const auto& text : {QString("label Command entry at 2.25, -3.5"),
                              QString("draw rectangle 4 m x 3 m"),
                              QString("set workspace architectural")}) {
        const auto command_before = window.document().snapshot();
        drive(window, [&](QDialog& dialog) {
            auto* kind = control<QComboBox>(dialog, "assistanceKind");
            kind->setCurrentIndex(kind->findData(static_cast<int>(AssistanceKind::natural_language)));
            control<QLineEdit>(dialog, "assistanceCommand")->setText(text);
            generate(dialog);
            auto* canvas = control<PlanCanvas>(dialog, "assistanceReviewCanvas");
            const auto details = control<QLabel>(dialog, "assistanceReviewDetails")->text();
            require(canvas->references().empty() && !details.contains("reference asset", Qt::CaseInsensitive),
                "natural-language commands must review their typed result without decoding a reference asset");
            if (text.startsWith("label"))
                require(canvas->labels().size() == 1 && canvas->labels().front().text == "Command entry" &&
                    same_point(canvas->labels().front().position, {2.25, -3.5}) && details.contains("Command entry"),
                    "command label must preview readable content at the requested coordinates");
            else if (text.startsWith("draw"))
                require(canvas->entities().size() == 1 && canvas->entities().front().segments.size() == 4 &&
                    details.contains("rectangle", Qt::CaseInsensitive), "rectangle command must preview proposed geometry");
            else require(details.contains("architectural", Qt::CaseInsensitive),
                         "workspace command must explain the proposed workspace");
            const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
            if (!capture.isEmpty()) {
                require(QDir().mkpath(capture), "command capture directory must exist");
                const auto name = text.startsWith("label") ? "assistance-command-label-review.png"
                    : text.startsWith("draw") ? "assistance-command-rectangle-review.png"
                                              : "assistance-command-workspace-review.png";
                require(dialog.grab().save(QDir(capture).filePath(QString::fromLatin1(name))),
                        "actual command review capture must save");
            }
            control<QPushButton>(dialog, "assistanceAccept")->click();
            empty(dialog);
        });
        if (text.startsWith("set"))
            require(window.workspace() == Workspace::architectural && window.document().revision() == command_before.revision(),
                "workspace command must apply exactly its presentation operation");
        else require(window.document().revision() == command_before.revision() + 1 && window.undoCommand() &&
                         window.document().snapshot().entities() == command_before.entities(),
                     "command geometry or label must apply and undo as one document operation");
    }
    const auto cancelled = document_snapshot_digest(window.document().snapshot());
    drive(window, [&](QDialog& dialog) {
        auto* kind = control<QComboBox>(dialog, "assistanceKind");
        kind->setCurrentIndex(kind->findData(static_cast<int>(AssistanceKind::natural_language)));
        control<QLineEdit>(dialog, "assistanceCommand")->setText("label Cancelled at 9, 9");
        generate(dialog);
        control<QLineEdit>(dialog, "assistanceCommand")->setText("label Changed at 8, 8"); empty(dialog);
        generate(dialog);
    });
    require(document_snapshot_digest(window.document().snapshot()) == cancelled,
            "Cancel after command review must preserve the complete document");
}
void workflow(const QString& directory) {
    MainWindow window;
    const auto fixture = seed(window, directory);
    const auto before = window.document().snapshot();
    drive(window, [&](QDialog& dialog) {
        configure(dialog, fixture);
        generate(dialog);
        auto* details = control<QLabel>(dialog, "assistanceReviewDetails");
        require(details->isVisible() && details->text().contains("31 ft 6 in") &&
            details->text().contains("%") && details->text().contains("Reviewed measured line"),
            "review must visibly compare recognized text, confidence and chosen target");
        const auto edge_length = control<QComboBox>(dialog, "assistanceDimensionSegment")
            ->currentText().section(QChar(0x00b7), 1).trimmed();
        require(!edge_length.isEmpty() && details->text().contains(edge_length),
            "visible review target length must match the chosen actual edge");
        auto* canvas = control<PlanCanvas>(dialog, "assistanceReviewCanvas");
        require(canvas->isVisible() && canvas->references().size() == 1 && canvas->entities().size() == 1,
            "review must show the actual source image and one raw ROI");
        const auto& image = canvas->references().front().image;
        require(image.size() == QSize(100, 60) && image.pixelColor(0, 0) == QColor(Qt::white) &&
            image.pixelColor(15, 15) == QColor(Qt::black), "review must use selected source pixels");
        const auto& roi = canvas->entities().front().segments;
        require(roi.size() == 4 && std::abs(roi.front().start.x + 40.0) < 1e-9 &&
            std::abs(roi.front().start.y + 18.0) < 1e-9 &&
            std::abs(roi.front().end.x - 10.0) < 1e-9 &&
            std::abs(roi[1].end.y + 12.0) < 1e-9,
            "review ROI must retain embedded text raw pixel bounds in centered source coordinates");
        const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            require(QDir().mkpath(capture), "capture directory must exist");
            require(dialog.grab().save(QDir(capture).filePath("assistance-review.png")), "actual dialog capture must save");
        }
        auto* accept = control<QPushButton>(dialog, "assistanceAccept");
        require(accept->text() == "Add linked dimension", "dimension acceptance must describe its linked result");
        accept->click();
        empty(dialog);
    });
    const auto accepted = window.document().snapshot();
    require(accepted.revision() == before.revision() + 1 && accepted.entities().size() == before.entities().size() + 1,
            "acceptance must add exactly one dimension in one history command");
    require(accepted.entities().at(fixture.target.id) == fixture.target && accepted.assets() == before.assets(),
            "recognized length must preserve drawing geometry and source assets");
    for (const auto& [id, entity] : accepted.entities()) {
        if (before.entities().contains(id)) continue;
        const auto linked = decode_boundary_dimension_entity(entity);
        require(linked.dimension && linked.dimension->boundary_id == fixture.target.id &&
            linked.dimension->segment_id == "dialog-edge-a" && linked.dimension->resolve(accepted).segment_length() == 10.0,
            "linked dimension must resolve the actual chosen ten-metre edge");
        const auto& provenance = entity.extensions.at("assistance_provenance");
        require(provenance.at("linked_length_metres") == 10.0 &&
            std::abs(provenance.at("recognized_length_metres").get<double>() - 9.6012) < 1e-9 &&
            provenance.at("proposal").dump().find(fixture.first.toStdString()) != std::string::npos,
            "linked dimension must retain recognized length and selected source provenance");
    }
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(),
            "one Undo must remove the accepted dimension");
    require(window.redoCommand() && window.document().snapshot().entities() == accepted.entities(),
            "one Redo must restore exact association and provenance");
    const auto cancel_digest = document_snapshot_digest(window.document().snapshot());
    drive(window, [&](QDialog& dialog) {
        configure(dialog, fixture);
        generate(dialog);
        choose(control<QComboBox>(dialog, "assistanceReference"), fixture.second); empty(dialog);
        generate(dialog);
        choose(control<QComboBox>(dialog, "assistanceDimensionSegment"), "dialog-edge-b"); empty(dialog);
        generate(dialog);
        control<QComboBox>(dialog, "assistanceDimensionTarget")->setCurrentIndex(0); empty(dialog);
        configure(dialog, fixture); generate(dialog);
        control<QCheckBox>(dialog, "assistanceRecognizeImage")->click(); empty(dialog);
        control<QCheckBox>(dialog, "assistanceRecognizeImage")->click(); generate(dialog);
        control<QComboBox>(dialog, "assistanceKind")->setCurrentIndex(0); empty(dialog);
    });
    require(document_snapshot_digest(window.document().snapshot()) == cancel_digest,
            "closing review after option changes must preserve complete document digest");
    drive(window, [&](QDialog& dialog) {
        configure(dialog, fixture); generate(dialog);
        auto changed = fixture.target;
        changed.extensions["review_stale_fixture"] = true;
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(changed)}, {}, "Change document during review"});
        const auto stale_digest = document_snapshot_digest(window.document().snapshot());
        control<QPushButton>(dialog, "assistanceAccept")->click();
        require(document_snapshot_digest(window.document().snapshot()) == stale_digest &&
            !control<QLabel>(dialog, "assistanceStatus")->text().isEmpty(),
            "stale acceptance must refuse without leaking a dimension");
    });
}
} // namespace
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        require(font_id >= 0, "bundled font must load for real assistance review captures");
        const auto families = QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "bundled review font must expose its family");
        application.setFont(QFont(families.front(), 9));
        QTemporaryDir directory;
        require(directory.isValid(), "fixture directory must initialize");
        workflow(directory.path());
        contour_review(directory.path(), false);
        contour_review(directory.path(), true);
        label_and_command_review(directory.path());
        std::cout << "Assistance review dialog trusted-fixture interactions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

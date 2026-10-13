// Inline dimension editing on the plan canvas (DimensionLabel "editing"):
// double-click (or Enter on a selected wall) replaces a wall's length label
// with an inline field. Enter applies through the wall length pipeline as one
// undoable revision; invalid text keeps the field open in its error state;
// Esc cancels without changes.

#include "sketch/desktop/main_window.hpp"
#include "sketch/geometry.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "../src/desktop/measurement_format.hpp"
#include "../src/desktop/vertex_design_tokens.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace sketch;
using namespace sketch::desktop;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Segment wall_baseline(const Entity& entity) {
    const auto& baseline = entity.properties.at("baseline");
    return {{baseline.at("start").at(0).get<double>(), baseline.at("start").at(1).get<double>()},
            {baseline.at("end").at(0).get<double>(), baseline.at("end").at(1).get<double>()},
            baseline.at("sweep_radians").get<double>()};
}

double wall_length(const MainWindow& window, const QString& wall) {
    return segment_length(wall_baseline(window.document().snapshot().entities().at(wall.toStdString())));
}

PlanCanvas& architectural_canvas(MainWindow& window) {
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    require(canvas != nullptr, "the architectural plan canvas exists");
    return *canvas;
}

void mouse(PlanCanvas& canvas, QEvent::Type type, QPointF point, Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()), Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void double_click(PlanCanvas& canvas, QPointF point) {
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::NoButton);
    mouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::NoButton);
    QApplication::processEvents();
}

void key(QWidget& target, int code) {
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(&target, &press);
    QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
    QApplication::sendEvent(&target, &release);
    QApplication::processEvents();
}

void type_and(QLineEdit& editor, const QString& text, int code) {
    editor.selectAll();
    editor.insert(text);
    key(editor, code);
}

struct Fixture {
    MainWindow window;
    QString wall;
    PlanCanvas* canvas{};
    CanvasLabelPresentationIdentity label;

    Fixture() {
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 900);
        window.setMetricUnits(false);
        window.show();
        QApplication::processEvents();
        wall = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
        require(!wall.isEmpty(), "a straight wall is created");
        window.setWorkspace(Workspace::architectural);
        QApplication::processEvents();
        canvas = &architectural_canvas(window);
        canvas->setSnapEnabled(false);
        canvas->setOverviewMapEnabled(false);
        canvas->setViewTransform({1.5, 0.0}, 120.0);
        QApplication::processEvents();
        label = CanvasLabelPresentationIdentity{wall, {}, {}};
        require(canvas->labelPresentation(label).has_value(), "the wall presents its derived length label");
        require(canvas->labelScreenBounds(label).has_value(), "the wall length label is painted on screen");
    }

    QPointF label_center() const { return canvas->labelScreenBounds(label)->center(); }
};

void test_double_click_wall_length_edits_in_place_and_undoes() {
    Fixture fixture;
    auto& canvas = *fixture.canvas;
    const auto before = fixture.window.document().snapshot();
    require(canvas.labelPresentationAt(fixture.label_center()) == fixture.label,
            "the label center hits the wall length label");

    double_click(canvas, fixture.label_center());
    auto* editor = canvas.inlineLengthEditor();
    require(canvas.inlineLengthEditActive() && editor && editor->isVisible(),
            "double-clicking a wall length label opens the inline field");
    require(canvas.inlineLengthEditTarget() == fixture.label, "the field edits the clicked label");
    require(editor->objectName() == QStringLiteral("canvasInlineLengthEditor"), "the field is the canvas child editor");
    require(editor->parentWidget() == &canvas, "the field is a PlanCanvas child widget");
    require(editor->text() == units::length_input_text(3.0, false),
            "the field is prefilled with the current length in drawing units");
    require(editor->geometry().contains(fixture.label_center().toPoint()), "the field sits over the label");
    require(editor->styleSheet().contains(QStringLiteral("border:1px solid")), "the field uses the design-system border");
    require(fixture.window.selectedEntityIds().contains(fixture.wall), "the edited wall is selected while editing");
    require(fixture.window.document().snapshot().entities() == before.entities(), "opening the field changes nothing");

    type_and(*editor, QStringLiteral("12' 6\""), Qt::Key_Return);
    require(!canvas.inlineLengthEditActive() && !editor->isVisible(), "Enter applies and closes the field");
    const auto expected = 12.5 * 0.3048;
    require(std::abs(wall_length(fixture.window, fixture.wall) - expected) < 1e-9,
            "Enter sets the wall length to exactly 12' 6\"");
    require(fixture.window.document().revision() == before.revision() + 1,
            "the inline edit is one undoable revision");

    require(fixture.window.undoCommand(), "the inline length edit undoes");
    require(fixture.window.document().snapshot().entities() == before.entities(),
            "one undo restores the original wall exactly");
    std::cout << "double-click wall length edit passed\n";
}

void test_invalid_text_keeps_editor_with_error_then_esc_cancels() {
    Fixture fixture;
    auto& canvas = *fixture.canvas;
    require(fixture.window.selectEntity(fixture.wall), "the wall is selected");
    QApplication::processEvents();
    const auto before = fixture.window.document().snapshot();

    // Keyboard path: Enter on the selected wall opens its length field.
    canvas.setFocus();
    key(canvas, Qt::Key_Return);
    auto* editor = canvas.inlineLengthEditor();
    require(canvas.inlineLengthEditActive() && editor && editor->isVisible(),
            "Enter with a wall selected opens its inline length field");

    type_and(*editor, QStringLiteral("twelve feet"), Qt::Key_Return);
    require(canvas.inlineLengthEditActive() && editor->isVisible(), "invalid text keeps the field open");
    require(editor->property("invalid").toBool(), "the field shows its error state");
    require(editor->styleSheet().contains(design::css(design::for_surface(false).danger)) ||
                editor->styleSheet().contains(design::css(design::for_surface(true).danger)),
            "the error state draws the border in danger");
    const auto message = canvas.inlineLengthEditError();
    require(!message.isEmpty() && message.contains(QStringLiteral("Type a value like")),
            "the error states the fact and the fix");
    auto* message_label = canvas.findChild<QLabel*>(QStringLiteral("canvasInlineLengthMessage"));
    require(message_label && message_label->isVisible(), "the error message is shown on the canvas");
    require(fixture.window.document().snapshot().entities() == before.entities() &&
                fixture.window.document().revision() == before.revision(),
            "invalid text changes nothing");

    type_and(*editor, QStringLiteral("0"), Qt::Key_Return);
    require(canvas.inlineLengthEditActive() && editor->property("invalid").toBool(),
            "a zero length is rejected and keeps the field open");

    key(*editor, Qt::Key_Escape);
    require(!canvas.inlineLengthEditActive() && !editor->isVisible() && !message_label->isVisible(),
            "Esc closes the field and its message");
    require(fixture.window.document().snapshot().entities() == before.entities() &&
                fixture.window.document().revision() == before.revision(),
            "Esc after an error changes nothing");
    std::cout << "invalid inline length passed\n";
}

void test_esc_cancels_typed_length() {
    Fixture fixture;
    auto& canvas = *fixture.canvas;
    const auto before = fixture.window.document().snapshot();
    double_click(canvas, fixture.label_center());
    auto* editor = canvas.inlineLengthEditor();
    require(canvas.inlineLengthEditActive() && editor, "the field opens");
    type_and(*editor, QStringLiteral("20'"), Qt::Key_Escape);
    require(!canvas.inlineLengthEditActive() && !editor->isVisible(), "Esc cancels the field");
    require(fixture.window.document().snapshot().entities() == before.entities() &&
                fixture.window.document().revision() == before.revision(),
            "Esc discards the typed length");

    // Accepting the unchanged (display-rounded) prefill is not an edit.
    double_click(canvas, fixture.label_center());
    require(canvas.inlineLengthEditActive(), "the field reopens");
    key(*canvas.inlineLengthEditor(), Qt::Key_Return);
    require(!canvas.inlineLengthEditActive() &&
                fixture.window.document().revision() == before.revision(),
            "Enter on the unchanged prefill closes without resizing to the rounded value");
    std::cout << "Esc cancel passed\n";
}

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-inline-dimension-test-") +
                                         QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    if (font_id >= 0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(), 10));
    try {
        test_double_click_wall_length_edits_in_place_and_undoes();
        test_invalid_text_keeps_editor_with_error_then_esc_cancels();
        test_esc_cancels_typed_length();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Inline dimension edit desktop tests passed\n";
    return 0;
}

#include "sketch/desktop/boundary_input_dialog.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace sketch;
using sketch::desktop::BoundaryInputDialog;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void install_capture_font() {
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    require(font_id >= 0, "bundled boundary input font must load");
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    require(!families.isEmpty(), "bundled boundary input font must expose a family");
    QApplication::setFont(QFont(families.front(), 10));

    const QFontMetrics metrics(QApplication::font());
    for (const auto character : QStringLiteral(
             "Add precise boundary segment Choose one analytical construction method. "
             "Length / heading Rise / run Relative turn Line to world coordinate "
             "Arc chord / angle Arc chord / height Arc chord / length "
             "Arc start tangent / length / sweep Construction method "
             "Heading (deg/rad/pi) Turn from previous edge End world X End world Y "
             "Sweep angle Signed chord height Arc length Start tangent Clockwise "
             "Endpoint (world metres): (2, 0) • length: 2 m Pending dimension "
             "place the dimension for this edge before finishing the boundary. "
             "Validated Add segment Finish closes the boundary.")) {
        require(metrics.inFont(character),
                "bundled boundary input font must contain each rendered character");
    }
}

BoundaryAuthoringSession source_session(BoundaryAuthoringMode mode,
                                        bool preceding_edge = false) {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = false;
    BoundaryAuthoringSession result(mode, options);
    result.set_classification("measurement");
    (void)result.anchor({0.0, 0.0});
    if (preceding_edge) {
        (void)result.add_line(parse_quantity("1 m"), parse_angle("0 deg"));
    }
    return result;
}

QComboBox& method(BoundaryInputDialog& dialog) {
    auto* result = dialog.findChild<QComboBox*>("boundaryInputMethod");
    require(result != nullptr, "missing boundary input method combo");
    return *result;
}

QLineEdit& field(BoundaryInputDialog& dialog, const char* object_name) {
    auto* result = dialog.findChild<QLineEdit*>(object_name);
    require(result != nullptr, std::string("missing boundary input field: ") + object_name);
    return *result;
}

QPushButton& add_button(BoundaryInputDialog& dialog) {
    auto* result = dialog.findChild<QPushButton*>("boundaryInputAdd");
    require(result != nullptr, "missing boundary input Add segment button");
    return *result;
}

QLabel& status(BoundaryInputDialog& dialog) {
    auto* result = dialog.findChild<QLabel*>("boundaryInputStatus");
    require(result != nullptr, "missing boundary input status");
    return *result;
}

void select_method(BoundaryInputDialog& dialog, int index) {
    auto& choice = method(dialog);
    require(index >= 0 && index < choice.count(), "boundary input method index is unavailable");
    choice.setCurrentIndex(index);
}

void set_field(BoundaryInputDialog& dialog, const char* object_name,
               const char* value) {
    field(dialog, object_name).setText(QString::fromUtf8(value));
}

BoundaryConstructionKind last_kind(const BoundaryAuthoringSession& session) {
    const auto chain = session.active_chain();
    require(chain.has_value() && !chain->segments.empty(),
            "candidate must contain the entered segment");
    require(!chain->receipts.empty(), "candidate must retain the construction receipt");
    return chain->receipts.back().kind;
}

void capture(BoundaryInputDialog& dialog, const QString& name, double scale) {
    const auto directory = qEnvironmentVariable("SKETCH_BOUNDARY_INPUT_CAPTURE_DIR");
    if (directory.isEmpty()) {
        return;
    }
    require(QDir().mkpath(directory), "boundary input capture directory could not be created");
    const auto previous_size = dialog.size();
    const auto was_visible = dialog.isVisible();
    dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
    dialog.resize(static_cast<int>(560.0 * scale), static_cast<int>(340.0 * scale));
    dialog.show();
    QCoreApplication::processEvents();
    const auto path = QDir(directory).filePath(name + QStringLiteral(".png"));
    require(dialog.grab().save(path), "boundary input dialog capture could not be written");
    if (!was_visible) {
        dialog.hide();
    }
    dialog.resize(previous_size);
    QCoreApplication::processEvents();
}

void test_precision_construction_forms() {
    struct FormCase {
        int method_index;
        BoundaryConstructionKind kind;
        void (*fill)(BoundaryInputDialog&);
    };

    const FormCase forms[] = {
        {0, BoundaryConstructionKind::line_heading,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputLength", "2 m");
             set_field(dialog, "boundaryInputHeading", "0 deg");
         }},
        {1, BoundaryConstructionKind::line_rise_run,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputRise", "1 m");
             set_field(dialog, "boundaryInputRun", "2 m");
         }},
        {2, BoundaryConstructionKind::line_relative_turn,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputLength", "1 m");
             set_field(dialog, "boundaryInputTurn", "90 deg");
         }},
        {3, BoundaryConstructionKind::line_to_point,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputEndX", "2 m");
             set_field(dialog, "boundaryInputEndY", "1 m");
         }},
        {4, BoundaryConstructionKind::arc_chord_angle,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputEndX", "2 m");
             set_field(dialog, "boundaryInputEndY", "0 m");
             set_field(dialog, "boundaryInputSweep", "90 deg");
         }},
        {5, BoundaryConstructionKind::arc_chord_height,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputEndX", "2 m");
             set_field(dialog, "boundaryInputEndY", "0 m");
             set_field(dialog, "boundaryInputHeight", "0.25 m");
         }},
        {6, BoundaryConstructionKind::arc_chord_length,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputEndX", "2 m");
             set_field(dialog, "boundaryInputEndY", "0 m");
             set_field(dialog, "boundaryInputArcLength", "3 m");
             auto* clockwise = dialog.findChild<QCheckBox*>("boundaryInputClockwise");
             require(clockwise != nullptr, "missing clockwise control");
             clockwise->setChecked(true);
         }},
        {7, BoundaryConstructionKind::arc_start_tangent,
         [](BoundaryInputDialog& dialog) {
             set_field(dialog, "boundaryInputTangent", "0 deg");
             set_field(dialog, "boundaryInputArcLength", "3 m");
             set_field(dialog, "boundaryInputSweep", "90 deg");
         }},
    };

    for (const auto& form : forms) {
        auto source = source_session(BoundaryAuthoringMode::draw_first,
                                     form.kind == BoundaryConstructionKind::line_relative_turn);
        const auto before = source.view();
        BoundaryInputDialog dialog(source, true);
        select_method(dialog, form.method_index);
        form.fill(dialog);
        require(add_button(dialog).isEnabled(), "valid boundary form should enable Add segment");
        if (form.method_index == 0) {
            capture(dialog, QStringLiteral("length-heading-normal"), 1.0);
            capture(dialog, QStringLiteral("length-heading-150"), 1.5);
        }
        require(dialog.submit(), std::string("precision form failed: ") +
                                  dialog.lastError().toStdString());
        const auto candidate = dialog.candidate();
        require(candidate.has_value(), "successful boundary form must expose a candidate");
        require(last_kind(*candidate) == form.kind, "candidate receipt kind does not match method");
        require(source.view() == before, "dialog preview or submit mutated the source session");
    }
}

void test_method_switch_preserves_inputs() {
    auto source = source_session(BoundaryAuthoringMode::draw_first);
    BoundaryInputDialog dialog(source, true);
    set_field(dialog, "boundaryInputLength", "3/2 m");
    set_field(dialog, "boundaryInputHeading", "pi/3");
    select_method(dialog, 1);
    select_method(dialog, 0);
    require(field(dialog, "boundaryInputLength").text() == QStringLiteral("3/2 m"),
            "method switch must preserve length input");
    require(field(dialog, "boundaryInputHeading").text() == QStringLiteral("pi/3"),
            "method switch must preserve heading input");
}

void test_imperial_default_and_candidate_copy() {
    auto source = source_session(BoundaryAuthoringMode::draw_first);
    const auto before = source.view();
    BoundaryInputDialog dialog(source, false);
    set_field(dialog, "boundaryInputLength", "2");
    set_field(dialog, "boundaryInputHeading", "90 deg");
    require(dialog.submit(), std::string("imperial default form failed: ") +
                                  dialog.lastError().toStdString());
    const auto candidate = dialog.candidate();
    require(candidate.has_value(), "imperial form must expose a candidate");
    const auto chain = candidate->active_chain();
    require(chain.has_value() && !chain->receipts.empty() &&
                chain->receipts.back().distance.has_value(),
            "imperial candidate must retain its distance receipt");
    require(chain->receipts.back().distance->entered_unit == Unit::foot &&
                chain->receipts.back().distance->original_expression == "2",
            "suffixless imperial quantity must use the foot default");
    auto modified_copy = *candidate;
    modified_copy.cancel();
    require(dialog.candidate().has_value() &&
                dialog.candidate()->phase() != BoundaryAuthoringPhase::cancelled,
            "mutating a returned candidate copy must not alter the private candidate");
    require(source.view() == before, "imperial dialog mutated the source session");
}

void test_invalid_input_stays_disabled_and_atomic() {
    struct InvalidCase {
        int method_index;
        const char* field_name;
        const char* value;
        const char* second_field_name;
        const char* second_value;
    };
    const InvalidCase invalid_cases[] = {
        {0, "boundaryInputLength", "not a length", "boundaryInputHeading", "0 deg"},
        {0, "boundaryInputLength", "-1 m", "boundaryInputHeading", "0 deg"},
        {0, "boundaryInputLength", "1 m", "boundaryInputHeading", "90"},
        {3, "boundaryInputEndX", "0 m", "boundaryInputEndY", "0 m"},
        {6, "boundaryInputArcLength", "1 m", "boundaryInputEndX", "2 m"},
    };

    for (const auto& invalid_case : invalid_cases) {
        auto source = source_session(BoundaryAuthoringMode::draw_first);
        const auto before = source.view();
        BoundaryInputDialog dialog(source, true);
        select_method(dialog, invalid_case.method_index);
        set_field(dialog, invalid_case.field_name, invalid_case.value);
        set_field(dialog, invalid_case.second_field_name, invalid_case.second_value);
        require(!add_button(dialog).isEnabled(), "invalid boundary form must disable Add segment");
        require(!dialog.submit(), "invalid boundary form must stay open");
        require(!dialog.candidate().has_value(), "invalid form must not expose a candidate");
        require(!dialog.lastError().isEmpty(), "invalid form must expose an inline diagnostic");
        auto* diagnostic = dialog.findChild<QLabel*>("boundaryInputError");
        require(diagnostic != nullptr && !diagnostic->text().isEmpty(),
                "invalid form diagnostic must have text");
        dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
        dialog.show();
        QCoreApplication::processEvents();
        require(diagnostic->isVisible(), "invalid form diagnostic must be visible");
        dialog.hide();
        require(source.view() == before, "invalid dialog input mutated the source session");
    }

    auto source = source_session(BoundaryAuthoringMode::draw_first);
    const auto before = source.view();
    BoundaryInputDialog negative_arc(source, true);
    select_method(negative_arc, 6);
    set_field(negative_arc, "boundaryInputEndX", "2 m");
    set_field(negative_arc, "boundaryInputEndY", "0 m");
    set_field(negative_arc, "boundaryInputArcLength", "-3 m");
    require(!negative_arc.submit(), "negative arc length must be rejected");
    require(source.view() == before, "negative arc length mutated the source session");
}

void test_define_first_reports_pending_dimension() {
    auto source = source_session(BoundaryAuthoringMode::define_first);
    const auto before = source.view();
    BoundaryInputDialog dialog(source, true);
    set_field(dialog, "boundaryInputLength", "2 m");
    set_field(dialog, "boundaryInputHeading", "0 deg");
    require(dialog.submit(), std::string("Define First form failed: ") +
                                  dialog.lastError().toStdString());
    const auto candidate = dialog.candidate();
    require(candidate.has_value(), "Define First should return its copied candidate");
    require(candidate->phase() == BoundaryAuthoringPhase::awaiting_dimension,
            "Define First edge must await manual dimension placement");
    require(status(dialog).text().contains(QStringLiteral("Pending dimension")),
            "Define First status must explain pending dimension placement");
    require(source.view() == before, "Define First dialog mutated the source session");
}

void test_escape_cancels_without_candidate() {
    auto source = source_session(BoundaryAuthoringMode::draw_first);
    const auto before = source.view();
    BoundaryInputDialog dialog(source, true);
    dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
    dialog.show();
    QCoreApplication::processEvents();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &escape);
    QCoreApplication::processEvents();
    require(dialog.result() != QDialog::Accepted, "Escape must cancel the boundary input dialog");
    require(!dialog.candidate().has_value(), "cancelled dialog must not expose a candidate");
    require(source.view() == before, "cancelled dialog mutated the source session");
}

void test_keyboard_tab_reaches_next_control() {
    auto source = source_session(BoundaryAuthoringMode::draw_first);
    BoundaryInputDialog dialog(source, true);
    dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
    dialog.show();
    QApplication::setActiveWindow(&dialog);
    auto* length = dialog.findChild<QLineEdit*>("boundaryInputLength");
    auto* heading = dialog.findChild<QLineEdit*>("boundaryInputHeading");
    require(length != nullptr && heading != nullptr, "missing tab-flow controls");
    length->setFocus();
    QCoreApplication::processEvents();
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(length, &tab);
    require(heading->hasFocus(), "Tab should advance from length to heading");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        install_capture_font();
        for (int index = 1; index < argc; ++index) {
            const auto argument = QString::fromLocal8Bit(argv[index]);
            if (argument == QStringLiteral("--capture-directory")) {
                if (index + 1 >= argc) {
                    throw std::runtime_error("--capture-directory requires a destination path");
                }
                const auto directory = QString::fromLocal8Bit(argv[++index]);
                if (directory.trimmed().isEmpty()) {
                    throw std::runtime_error("--capture-directory requires a destination path");
                }
                qputenv("SKETCH_BOUNDARY_INPUT_CAPTURE_DIR", directory.toUtf8());
            } else {
                throw std::runtime_error("unknown test argument: " + argument.toStdString());
            }
        }
        test_precision_construction_forms();
        test_method_switch_preserves_inputs();
        test_imperial_default_and_candidate_copy();
        test_invalid_input_stays_disabled_and_atomic();
        test_define_first_reports_pending_dimension();
        test_escape_cancels_without_candidate();
        test_keyboard_tab_reaches_next_control();
        std::cout << "Boundary input dialog workflows passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "boundary_input_dialog_tests: " << exception.what() << '\n';
        return 1;
    }
}

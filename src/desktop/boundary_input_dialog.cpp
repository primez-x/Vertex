#include "sketch/desktop/boundary_input_dialog.hpp"

#include "sketch/boundary_receipt.hpp"
#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sketch::desktop {
namespace {

enum class InputMethod {
    length_heading,
    rise_run,
    relative_turn,
    line_to_coordinate,
    arc_chord_angle,
    arc_chord_height,
    arc_chord_length,
    arc_start_tangent,
};

QString error_text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString number_text(double value) {
    if (!std::isfinite(value)) {
        return {};
    }
    return QString::number(value, 'g', 12);
}

QString method_name(InputMethod method) {
    switch (method) {
        case InputMethod::length_heading:
            return QStringLiteral("length and heading");
        case InputMethod::rise_run:
            return QStringLiteral("rise and run");
        case InputMethod::relative_turn:
            return QStringLiteral("relative turn");
        case InputMethod::line_to_coordinate:
            return QStringLiteral("world-coordinate line");
        case InputMethod::arc_chord_angle:
            return QStringLiteral("chord and sweep angle arc");
        case InputMethod::arc_chord_height:
            return QStringLiteral("chord and height arc");
        case InputMethod::arc_chord_length:
            return QStringLiteral("chord and arc-length arc");
        case InputMethod::arc_start_tangent:
            return QStringLiteral("start-tangent arc");
    }
    return QStringLiteral("boundary segment");
}

InputMethod method_from(const QComboBox& combo) {
    const auto value = combo.currentData();
    if (value.isValid()) {
        const auto index = value.toInt();
        if (index >= static_cast<int>(InputMethod::length_heading) &&
            index <= static_cast<int>(InputMethod::arc_start_tangent)) {
            return static_cast<InputMethod>(index);
        }
    }
    const auto index = combo.currentIndex();
    if (index >= 0 && index <= static_cast<int>(InputMethod::arc_start_tangent)) {
        return static_cast<InputMethod>(index);
    }
    return InputMethod::length_heading;
}

QString quantity_placeholder(bool metric) {
    return metric ? QStringLiteral("e.g. 2.5 m") : QStringLiteral("e.g. 8 ft");
}

QString coordinate_placeholder(bool metric) {
    return metric ? QStringLiteral("e.g. 1.25 m") : QStringLiteral("e.g. 4 ft");
}

QString angle_placeholder() {
    return QStringLiteral("e.g. 90 deg, 1.5708 rad, or pi/2");
}

bool has_explicit_angle_unit(const QString& value) {
    const auto lower = value.trimmed().toLower();
    return lower.endsWith(QStringLiteral("deg")) ||
           lower.endsWith(QStringLiteral("rad")) || lower.contains(QStringLiteral("pi"));
}

}  // namespace

class BoundaryInputDialog::Impl {
public:
    Impl(BoundaryInputDialog* owner, const BoundaryAuthoringSession& source,
         bool metric_units)
        : owner(owner), source(source), metric(metric_units) {
        owner->setObjectName(QStringLiteral("boundaryInputDialog"));
        owner->setWindowTitle(QStringLiteral("Add precise boundary segment"));
        owner->setMinimumWidth(500);
        owner->resize(560, 340);

        auto* root = new QVBoxLayout(owner);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* heading = new QLabel(
            QStringLiteral("Choose how to draw the next line or curve. Check the measurements, "
                           "then choose Add segment."),
            owner);
        heading->setWordWrap(true);
        root->addWidget(heading);

        form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);

        method = new QComboBox(owner);
        method->setObjectName(QStringLiteral("boundaryInputMethod"));
        add_method(QStringLiteral("Length / heading"), InputMethod::length_heading);
        add_method(QStringLiteral("Rise / run"), InputMethod::rise_run);
        add_method(QStringLiteral("Relative turn"), InputMethod::relative_turn);
        add_method(QStringLiteral("Line to world coordinate"), InputMethod::line_to_coordinate);
        add_method(QStringLiteral("Arc chord / angle"), InputMethod::arc_chord_angle);
        add_method(QStringLiteral("Arc chord / height"), InputMethod::arc_chord_height);
        add_method(QStringLiteral("Arc chord / length"), InputMethod::arc_chord_length);
        add_method(QStringLiteral("Arc start tangent / length / sweep"),
                   InputMethod::arc_start_tangent);
        form->addRow(QStringLiteral("Construction method"), method);

        length = quantity_field(QStringLiteral("boundaryInputLength"));
        heading_angle = angle_field(QStringLiteral("boundaryInputHeading"));
        rise = quantity_field(QStringLiteral("boundaryInputRise"));
        run = quantity_field(QStringLiteral("boundaryInputRun"));
        turn = angle_field(QStringLiteral("boundaryInputTurn"));
        end_x = coordinate_field(QStringLiteral("boundaryInputEndX"));
        end_y = coordinate_field(QStringLiteral("boundaryInputEndY"));
        sweep = angle_field(QStringLiteral("boundaryInputSweep"));
        height = quantity_field(QStringLiteral("boundaryInputHeight"));
        arc_length = quantity_field(QStringLiteral("boundaryInputArcLength"));
        tangent = angle_field(QStringLiteral("boundaryInputTangent"));
        clockwise = new QCheckBox(QStringLiteral("Clockwise"), owner);
        clockwise->setObjectName(QStringLiteral("boundaryInputClockwise"));

        form->addRow(QStringLiteral("Length"), length);
        form->addRow(QStringLiteral("Heading (deg/rad/pi)"), heading_angle);
        form->addRow(QStringLiteral("Rise"), rise);
        form->addRow(QStringLiteral("Run"), run);
        form->addRow(QStringLiteral("Turn from previous edge (deg/rad/pi)"), turn);
        form->addRow(QStringLiteral("End world X"), end_x);
        form->addRow(QStringLiteral("End world Y"), end_y);
        form->addRow(QStringLiteral("Sweep angle (deg/rad/pi)"), sweep);
        form->addRow(QStringLiteral("Signed chord height"), height);
        form->addRow(QStringLiteral("Arc length"), arc_length);
        form->addRow(QStringLiteral("Start tangent (deg/rad/pi)"), tangent);
        form->addRow(clockwise);
        root->addLayout(form);

        error = new QLabel(owner);
        error->setObjectName(QStringLiteral("boundaryInputError"));
        error->setAccessibleName(QStringLiteral("Boundary input diagnostic"));
        error->setWordWrap(true);
        error->setStyleSheet(QStringLiteral("color:#b44b4b;"));
        error->setVisible(false);
        root->addWidget(error);

        preview = new QLabel(owner);
        preview->setObjectName(QStringLiteral("boundaryInputPreview"));
        preview->setAccessibleName(QStringLiteral("Boundary segment preview"));
        preview->setWordWrap(true);
        root->addWidget(preview);

        status = new QLabel(owner);
        status->setObjectName(QStringLiteral("boundaryInputStatus"));
        status->setAccessibleName(QStringLiteral("Boundary input status"));
        status->setWordWrap(true);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        root->addWidget(status);

        buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, owner);
        buttons->setObjectName(QStringLiteral("boundaryInputButtons"));
        add_button = buttons->addButton(QStringLiteral("Add segment"),
                                         QDialogButtonBox::AcceptRole);
        add_button->setObjectName(QStringLiteral("boundaryInputAdd"));
        add_button->setDefault(true);
        add_button->setEnabled(false);
        if (auto* cancel = buttons->button(QDialogButtonBox::Cancel)) {
            cancel->setObjectName(QStringLiteral("boundaryInputCancel"));
        }
        root->addWidget(buttons);

        set_defaults();

        QObject::connect(method, qOverload<int>(&QComboBox::currentIndexChanged), owner,
                         [this](int) {
                             if (!loading) {
                                 configure();
                             }
                         });
        for (auto* field : {length, heading_angle, rise, run, turn, end_x, end_y, sweep,
                            height, arc_length, tangent}) {
            QObject::connect(field, &QLineEdit::textChanged, owner,
                             [this] {
                                 if (!loading) {
                                     refresh_validation();
                                 }
                             });
        }
        QObject::connect(clockwise, &QCheckBox::toggled, owner,
                         [this] {
                             if (!loading) {
                                 refresh_validation();
                             }
                         });
        QObject::connect(add_button, &QPushButton::clicked, owner,
                         [this] { (void)submit(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner, &QDialog::reject);
        QObject::connect(owner, &QDialog::rejected, owner,
                         [this] { accepted_candidate.reset(); });

        QWidget::setTabOrder(method, length);
        QWidget::setTabOrder(length, heading_angle);
        QWidget::setTabOrder(heading_angle, rise);
        QWidget::setTabOrder(rise, run);
        QWidget::setTabOrder(run, turn);
        QWidget::setTabOrder(turn, end_x);
        QWidget::setTabOrder(end_x, end_y);
        QWidget::setTabOrder(end_y, sweep);
        QWidget::setTabOrder(sweep, height);
        QWidget::setTabOrder(height, arc_length);
        QWidget::setTabOrder(arc_length, tangent);
        QWidget::setTabOrder(tangent, clockwise);
        QWidget::setTabOrder(clockwise, add_button);

        loading = false;
        configure();
    }

    std::optional<BoundaryAuthoringSession> candidate() const {
        return accepted_candidate;
    }

    QString last_error() const { return error_message; }

    bool submit() {
        accepted_candidate.reset();
        try {
            auto value = make_candidate();
            accepted_candidate = value;
            error_message.clear();
            error->clear();
            error->setVisible(false);
            add_button->setEnabled(true);
            update_preview(*accepted_candidate);
            owner->accept();
            return true;
        } catch (const std::exception& exception) {
            show_error(exception.what());
            return false;
        }
    }

private:
    void add_method(const QString& label, InputMethod value) {
        method->addItem(label, static_cast<int>(value));
    }

    QLineEdit* quantity_field(const QString& object_name) {
        auto* field = new QLineEdit(owner);
        field->setObjectName(object_name);
        field->setPlaceholderText(quantity_placeholder(metric));
        field->setClearButtonEnabled(true);
        return field;
    }

    QLineEdit* coordinate_field(const QString& object_name) {
        auto* field = quantity_field(object_name);
        field->setPlaceholderText(coordinate_placeholder(metric));
        return field;
    }

    QLineEdit* angle_field(const QString& object_name) {
        auto* field = new QLineEdit(owner);
        field->setObjectName(object_name);
        field->setPlaceholderText(angle_placeholder());
        field->setToolTip(QStringLiteral("Enter an explicit deg, rad, or pi expression."));
        field->setClearButtonEnabled(true);
        return field;
    }

    void set_defaults() {
        const auto unit = metric ? QStringLiteral(" m") : QStringLiteral(" ft");
        length->setText(QStringLiteral("1") + unit);
        heading_angle->setText(QStringLiteral("0 deg"));
        rise->setText(QStringLiteral("0") + unit);
        run->setText(QStringLiteral("1") + unit);
        turn->setText(QStringLiteral("0 deg"));
        end_x->setText(QStringLiteral("1") + unit);
        end_y->setText(QStringLiteral("0") + unit);
        sweep->setText(QStringLiteral("90 deg"));
        height->setText(QStringLiteral("0.25") + unit);
        arc_length->setText(QStringLiteral("2") + unit);
        tangent->setText(QStringLiteral("0 deg"));
        clockwise->setChecked(false);
    }

    void configure() {
        const auto selected = method_from(*method);
        form->setRowVisible(length, selected == InputMethod::length_heading ||
                                      selected == InputMethod::relative_turn);
        form->setRowVisible(heading_angle, selected == InputMethod::length_heading);
        form->setRowVisible(rise, selected == InputMethod::rise_run);
        form->setRowVisible(run, selected == InputMethod::rise_run);
        form->setRowVisible(turn, selected == InputMethod::relative_turn);
        const auto has_chord = selected == InputMethod::arc_chord_angle ||
                               selected == InputMethod::arc_chord_height ||
                               selected == InputMethod::arc_chord_length;
        form->setRowVisible(end_x, selected == InputMethod::line_to_coordinate || has_chord);
        form->setRowVisible(end_y, selected == InputMethod::line_to_coordinate || has_chord);
        form->setRowVisible(sweep, selected == InputMethod::arc_chord_angle ||
                                      selected == InputMethod::arc_start_tangent);
        form->setRowVisible(height, selected == InputMethod::arc_chord_height);
        form->setRowVisible(arc_length, selected == InputMethod::arc_chord_length ||
                                      selected == InputMethod::arc_start_tangent);
        form->setRowVisible(tangent, selected == InputMethod::arc_start_tangent);
        form->setRowVisible(clockwise, selected == InputMethod::arc_chord_length);
        if (!loading) {
            refresh_validation();
        }
    }

    Quantity read_quantity(const QLineEdit* field, QString label) const {
        const auto raw = field->text().trimmed();
        if (raw.isEmpty()) {
            throw std::invalid_argument(label.toStdString() + " is required");
        }
        try {
            const auto value = parse_quantity(raw.toUtf8().toStdString(),
                                              metric ? Unit::metre : Unit::foot);
            if (!std::isfinite(value.metres)) {
                throw std::invalid_argument("must be finite");
            }
            return value;
        } catch (const std::exception& exception) {
            throw std::invalid_argument(label.toStdString() + ": " + exception.what());
        }
    }

    AngleInput read_angle(const QLineEdit* field, QString label) const {
        const auto raw = field->text().trimmed();
        if (raw.isEmpty()) {
            throw std::invalid_argument(label.toStdString() + " is required");
        }
        if (!has_explicit_angle_unit(raw)) {
            throw std::invalid_argument(
                label.toStdString() +
                " requires an explicit deg, rad, or pi expression (for example 90 deg or pi/2)");
        }
        try {
            auto value = parse_angle(raw.toUtf8().toStdString());
            if (!std::isfinite(value.radians)) {
                throw std::invalid_argument("must be finite");
            }
            return value;
        } catch (const std::exception& exception) {
            throw std::invalid_argument(label.toStdString() + ": " + exception.what());
        }
    }

    Vec2 read_coordinate() const {
        return {read_quantity(end_x, QStringLiteral("world X")).metres,
                read_quantity(end_y, QStringLiteral("world Y")).metres};
    }

    BoundaryAuthoringSession make_candidate() const {
        auto value = source;
        const auto selected = method_from(*method);
        switch (selected) {
            case InputMethod::length_heading:
                (void)value.add_line(read_quantity(length, QStringLiteral("length")),
                                      read_angle(heading_angle, QStringLiteral("heading")));
                break;
            case InputMethod::rise_run:
                (void)value.add_line_rise_run(read_quantity(rise, QStringLiteral("rise")),
                                              read_quantity(run, QStringLiteral("run")));
                break;
            case InputMethod::relative_turn:
                (void)value.add_line_relative_turn(
                    read_quantity(length, QStringLiteral("length")),
                    read_angle(turn, QStringLiteral("turn")));
                break;
            case InputMethod::line_to_coordinate:
                (void)value.add_line_to(read_coordinate());
                break;
            case InputMethod::arc_chord_angle:
                (void)value.add_arc_chord_angle(
                    read_coordinate(), read_angle(sweep, QStringLiteral("sweep angle")));
                break;
            case InputMethod::arc_chord_height:
                (void)value.add_arc_chord_height(
                    read_coordinate(), read_quantity(height, QStringLiteral("chord height")));
                break;
            case InputMethod::arc_chord_length:
                (void)value.add_arc_chord_arc_length(
                    read_coordinate(), read_quantity(arc_length, QStringLiteral("arc length")),
                    clockwise->isChecked());
                break;
            case InputMethod::arc_start_tangent:
                (void)value.add_arc_start_tangent(
                    read_angle(tangent, QStringLiteral("start tangent")),
                    read_quantity(arc_length, QStringLiteral("arc length")),
                    read_angle(sweep, QStringLiteral("sweep angle")));
                break;
        }
        return value;
    }

    QString display_length(double metres) const {
        const auto value = metric ? metres : metres / 0.3048;
        return number_text(value) + (metric ? QStringLiteral(" m") : QStringLiteral(" ft"));
    }

    void update_preview(const BoundaryAuthoringSession& value) {
        const auto chain = value.active_chain();
        if (!chain.has_value() || chain->segments.empty()) {
            preview->setText(QStringLiteral("No edge has been entered yet."));
            status->setText(QStringLiteral("A validated segment will appear here."));
            return;
        }
        const auto& segment = chain->segments.back().segment;
        const auto length_value = segment_length(segment);
        preview->setText(
            QStringLiteral("Endpoint: X %1, Y %2 • length: %3")
                .arg(display_length(segment.end.x), display_length(segment.end.y),
                     display_length(length_value)));
        if (value.phase() == BoundaryAuthoringPhase::awaiting_dimension ||
            !chain->pending_dimensions.empty()) {
            status->setText(QStringLiteral(
                "Pending dimension: place the dimension for this edge before finishing the "
                "boundary."));
        } else {
            status->setText(QStringLiteral(
                "Ready to add %1. Press Enter on the drawing to close the area.")
                                .arg(method_name(method_from(*method))));
        }
    }

    void show_error(const std::string& message) {
        error_message = error_text(message);
        error->setText(error_message);
        error->setVisible(true);
        add_button->setEnabled(false);
        status->setText(QStringLiteral("Enter a valid %1 to continue.")
                            .arg(method_name(method_from(*method))));
    }

    void refresh_validation() {
        if (loading) {
            return;
        }
        accepted_candidate.reset();
        validation_candidate.reset();
        error_message.clear();
        error->clear();
        error->setVisible(false);
        try {
            auto value = make_candidate();
            validation_candidate = value;
            add_button->setEnabled(true);
            update_preview(*validation_candidate);
        } catch (const std::exception& exception) {
            show_error(exception.what());
        }
    }

    BoundaryInputDialog* owner{};
    BoundaryAuthoringSession source;
    bool metric{};
    bool loading{true};
    std::optional<BoundaryAuthoringSession> validation_candidate;
    std::optional<BoundaryAuthoringSession> accepted_candidate;
    QString error_message;
    QFormLayout* form{};
    QComboBox* method{};
    QLineEdit *length{}, *heading_angle{}, *rise{}, *run{}, *turn{};
    QLineEdit *end_x{}, *end_y{}, *sweep{}, *height{}, *arc_length{}, *tangent{};
    QCheckBox* clockwise{};
    QLabel *error{}, *preview{}, *status{};
    QDialogButtonBox* buttons{};
    QPushButton* add_button{};
};

BoundaryInputDialog::BoundaryInputDialog(const BoundaryAuthoringSession& source,
                                         bool metricUnits, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, source, metricUnits)) {}

BoundaryInputDialog::~BoundaryInputDialog() = default;

std::optional<BoundaryAuthoringSession> BoundaryInputDialog::candidate() const {
    return m_impl->candidate();
}

bool BoundaryInputDialog::submit() { return m_impl->submit(); }

QString BoundaryInputDialog::lastError() const { return m_impl->last_error(); }

}  // namespace sketch::desktop

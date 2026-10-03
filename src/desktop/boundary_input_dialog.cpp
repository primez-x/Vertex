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
    Impl(BoundaryInputDialog* owner, std::optional<BoundaryAuthoringSession> source,
         bool metric_units,const BoundaryInputPreferences& preferences,
         BoundaryInputPresentation presentation,
         std::optional<MeasurementLineworkInputContext> measured = std::nullopt)
        : owner(owner), source(std::move(source)), measured(std::move(measured)), metric(metric_units),
          preferences(preferences.metric_units && *preferences.metric_units!=metric_units ? BoundaryInputPreferences{} : preferences),
          wall(presentation == BoundaryInputPresentation::wall) {
        phase=this->measured ? (this->measured->start ? BoundaryAuthoringPhase::drawing :
            BoundaryAuthoringPhase::awaiting_anchor) : this->source->phase();
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
        if (phase==BoundaryAuthoringPhase::awaiting_anchor) {
            owner->setWindowTitle(QStringLiteral("Boundary start point"));
            heading->setText(QStringLiteral("Enter the starting point, then choose Place start point."));
        } else if (phase==BoundaryAuthoringPhase::awaiting_dimension) {
            owner->setWindowTitle(QStringLiteral("Place precise edge dimension"));
            heading->setText(QStringLiteral("Enter the pending dimension's text position, then choose Place dimension."));
        }
        if (wall) {
            owner->setWindowTitle(phase == BoundaryAuthoringPhase::awaiting_anchor
                ? QStringLiteral("Wall start point") : QStringLiteral("Add precise wall segment"));
            if (phase == BoundaryAuthoringPhase::drawing)
                heading->setText(QStringLiteral("Choose how to draw the next physical wall. Its thickness and height come from Wall settings."));
        }
        if (this->measured) {
            owner->setWindowTitle(phase == BoundaryAuthoringPhase::awaiting_anchor
                ? QStringLiteral("Measured stroke start point") : QStringLiteral("Add precise measured edge"));
            heading->setText(phase == BoundaryAuthoringPhase::awaiting_anchor
                ? QStringLiteral("Enter starting coordinates.") : QString{});
            heading->setVisible(phase == BoundaryAuthoringPhase::awaiting_anchor);
        }

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
        error->setAccessibleName(this->measured ? QStringLiteral("Measured edge input diagnostic") : wall ? QStringLiteral("Wall input diagnostic") : QStringLiteral("Boundary input diagnostic"));
        error->setWordWrap(true);
        error->setStyleSheet(QStringLiteral("color:#b44b4b;"));
        error->setVisible(false);
        root->addWidget(error);

        preview = new QLabel(owner);
        preview->setObjectName(QStringLiteral("boundaryInputPreview"));
        preview->setAccessibleName(this->measured ? QStringLiteral("Measured edge preview") : wall ? QStringLiteral("Wall segment preview") : QStringLiteral("Boundary segment preview"));
        preview->setWordWrap(true);
        root->addWidget(preview);

        status = new QLabel(owner);
        status->setObjectName(QStringLiteral("boundaryInputStatus"));
        status->setAccessibleName(this->measured ? QStringLiteral("Measured edge input status") : wall ? QStringLiteral("Wall input status") : QStringLiteral("Boundary input status"));
        status->setWordWrap(true);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        root->addWidget(status);

        buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, owner);
        buttons->setObjectName(QStringLiteral("boundaryInputButtons"));
        const auto action_text=phase==BoundaryAuthoringPhase::awaiting_anchor ? QStringLiteral("Place start point") :
            phase==BoundaryAuthoringPhase::awaiting_dimension ? QStringLiteral("Place dimension") :
            this->measured ? QStringLiteral("Add measured edge") : wall ? QStringLiteral("Add wall") : QStringLiteral("Add segment");
        add_button = buttons->addButton(action_text,
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
                         [this] { accepted_candidate.reset(); accepted_receipt.reset(); accepted_anchor.reset(); accepted_preferences.reset(); });

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
        if (phase!=BoundaryAuthoringPhase::drawing) owner->adjustSize();
        if (phase!=BoundaryAuthoringPhase::drawing) end_x->setFocus();
        else if (method_from(*method)==InputMethod::rise_run) rise->setFocus();
        else if (method_from(*method)==InputMethod::arc_start_tangent) tangent->setFocus();
        else if (length->isHidden()) end_x->setFocus();
        else length->setFocus();
    }

    std::optional<BoundaryAuthoringSession> candidate() const {
        return accepted_candidate;
    }
    std::optional<BoundaryInputPreferences> acceptedPreferences() const { return accepted_preferences; }
    std::optional<ConstructionReceipt> receipt() const { return accepted_receipt; }
    std::optional<Vec2> anchor() const { return accepted_anchor; }

    QString last_error() const { return error_message; }

    bool submit() {
        accepted_candidate.reset();
        accepted_receipt.reset();
        accepted_anchor.reset();
        accepted_preferences.reset();
        try {
            if (measured) {
                if (measured->start) accepted_receipt = replay_measured().receipt;
                else accepted_anchor = read_coordinate();
            } else accepted_candidate = make_candidate();
            accepted_preferences=preferences;
            if (phase==BoundaryAuthoringPhase::drawing) {
                accepted_preferences->metric_units=metric;
                accepted_preferences->method_index=method->currentIndex();
                accepted_preferences->clockwise=clockwise->isChecked();
                for (auto* field : {length,heading_angle,rise,run,turn,sweep,height,arc_length,tangent})
                    accepted_preferences->expressions[field->objectName().toStdString()]=field->text();
            }
            error_message.clear();
            error->clear();
            error->setVisible(false);
            add_button->setEnabled(true);
            if (measured) update_measured_preview();
            else update_preview(*accepted_candidate);
            owner->accept();
            return true;
        } catch (const std::exception& exception) {
            accepted_candidate.reset();
            accepted_receipt.reset();
            accepted_anchor.reset();
            accepted_preferences.reset();
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
        if (preferences.method_index>=0 && preferences.method_index<method->count())
            method->setCurrentIndex(preferences.method_index);
        for (auto* field : {length,heading_angle,rise,run,turn,sweep,height,arc_length,tangent}) {
            const auto value=preferences.expressions.find(field->objectName().toStdString());
            if (value!=preferences.expressions.end()) field->setText(value->second);
        }
        clockwise->setChecked(preferences.clockwise);
        if (measured) {
            auto position = measured->pointer.value_or(measured->start.value_or(Vec2{}));
            if (measured->start && (!measured->pointer ||
                (position.x == measured->start->x && position.y == measured->start->y))) position.x += 1.0;
            end_x->setText(QString::number(position.x,'g',17)+QStringLiteral(" m"));
            end_y->setText(QString::number(position.y,'g',17)+QStringLiteral(" m"));
            return;
        }
        const auto state=source->view();
        auto position=state.pointer.value_or(state.anchor.value_or(Vec2{}));
        if (state.active_chain && !state.active_chain->segments.empty()) {
            const auto& segment=state.active_chain->segments.back().segment;
            if (phase==BoundaryAuthoringPhase::awaiting_dimension && !state.pointer)
                position={(segment.start.x+segment.end.x)*0.5,(segment.start.y+segment.end.y)*0.5};
            else if (phase==BoundaryAuthoringPhase::drawing &&
                (!state.pointer || (position.x==segment.end.x && position.y==segment.end.y)))
                position={segment.end.x+1.0,segment.end.y};
        } else if (phase==BoundaryAuthoringPhase::drawing &&
            (!state.pointer || !state.anchor || (position.x==state.anchor->x && position.y==state.anchor->y)))
            position.x+=1.0;
        // Explicit model metres round-trip the contextual coordinates without
        // interpreting a converted decimal in the current default input unit.
        end_x->setText(QString::number(position.x,'g',17)+QStringLiteral(" m"));
        end_y->setText(QString::number(position.y,'g',17)+QStringLiteral(" m"));
    }

    void configure() {
        const auto selected = method_from(*method);
        if (phase!=BoundaryAuthoringPhase::drawing) {
            form->setRowVisible(method,false);
            for (auto* field : {length,heading_angle,rise,run,turn,sweep,height,arc_length,tangent})
                form->setRowVisible(field,false);
            form->setRowVisible(clockwise,false);
            form->setRowVisible(end_x,true); form->setRowVisible(end_y,true);
            const auto prefix=phase==BoundaryAuthoringPhase::awaiting_anchor ? QStringLiteral("Anchor world ") : QStringLiteral("Dimension world ");
            if (auto* label=qobject_cast<QLabel*>(form->labelForField(end_x))) label->setText(prefix+QStringLiteral("X"));
            if (auto* label=qobject_cast<QLabel*>(form->labelForField(end_y))) label->setText(prefix+QStringLiteral("Y"));
            if (!loading) refresh_validation();
            return;
        }
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

    ConstructionReceipt make_receipt(Vec2 start) const {
        ConstructionReceipt value;
        value.segment_id = "precision-input";
        value.start = start;
        switch (method_from(*method)) {
            case InputMethod::length_heading:
                value.kind = BoundaryConstructionKind::line_heading;
                value.distance = read_quantity(length, QStringLiteral("length"));
                value.heading = read_angle(heading_angle, QStringLiteral("heading"));
                break;
            case InputMethod::rise_run:
                value.kind = BoundaryConstructionKind::line_rise_run;
                value.rise = read_quantity(rise, QStringLiteral("rise"));
                value.run = read_quantity(run, QStringLiteral("run"));
                break;
            case InputMethod::relative_turn:
                value.kind = BoundaryConstructionKind::line_relative_turn;
                value.distance = read_quantity(length, QStringLiteral("length"));
                value.turn = read_angle(turn, QStringLiteral("turn"));
                break;
            case InputMethod::line_to_coordinate:
                value.kind = BoundaryConstructionKind::line_to_point;
                value.chord_end = read_coordinate();
                break;
            case InputMethod::arc_chord_angle:
                value.kind = BoundaryConstructionKind::arc_chord_angle;
                value.chord_end = read_coordinate();
                value.angle = read_angle(sweep, QStringLiteral("sweep angle"));
                break;
            case InputMethod::arc_chord_height:
                value.kind = BoundaryConstructionKind::arc_chord_height;
                value.chord_end = read_coordinate();
                value.height = read_quantity(height, QStringLiteral("chord height"));
                break;
            case InputMethod::arc_chord_length:
                value.kind = BoundaryConstructionKind::arc_chord_length;
                value.chord_end = read_coordinate();
                value.arc_length = read_quantity(arc_length, QStringLiteral("arc length"));
                value.clockwise = clockwise->isChecked();
                break;
            case InputMethod::arc_start_tangent:
                value.kind = BoundaryConstructionKind::arc_start_tangent;
                value.tangent = read_angle(tangent, QStringLiteral("start tangent"));
                value.arc_length = read_quantity(arc_length, QStringLiteral("arc length"));
                value.sweep = read_angle(sweep, QStringLiteral("sweep angle"));
                break;
        }
        return value;
    }

    BoundaryAuthoringSession make_candidate() const {
        auto value = *source;
        if (phase==BoundaryAuthoringPhase::awaiting_anchor) { (void)value.anchor(read_coordinate()); return value; }
        if (phase==BoundaryAuthoringPhase::awaiting_dimension) { (void)value.place_manual_dimension(read_coordinate()); return value; }
        if (phase!=BoundaryAuthoringPhase::drawing)
            throw std::invalid_argument("This drawing is not awaiting an anchor, edge or dimension position");
        const auto receipt = make_receipt({});
        switch (receipt.kind) {
            case BoundaryConstructionKind::line_heading: (void)value.add_line(*receipt.distance,*receipt.heading); break;
            case BoundaryConstructionKind::line_rise_run: (void)value.add_line_rise_run(*receipt.rise,*receipt.run); break;
            case BoundaryConstructionKind::line_relative_turn: (void)value.add_line_relative_turn(*receipt.distance,*receipt.turn); break;
            case BoundaryConstructionKind::line_to_point: (void)value.add_line_to(*receipt.chord_end); break;
            case BoundaryConstructionKind::arc_chord_angle: (void)value.add_arc_chord_angle(*receipt.chord_end,*receipt.angle); break;
            case BoundaryConstructionKind::arc_chord_height: (void)value.add_arc_chord_height(*receipt.chord_end,*receipt.height); break;
            case BoundaryConstructionKind::arc_chord_length: (void)value.add_arc_chord_arc_length(*receipt.chord_end,*receipt.arc_length,receipt.clockwise); break;
            case BoundaryConstructionKind::arc_start_tangent: (void)value.add_arc_start_tangent(*receipt.tangent,*receipt.arc_length,*receipt.sweep); break;
            default: throw std::invalid_argument("Unsupported precision input method");
        }
        return value;
    }

    ReplayedConstructionReceipt replay_measured() const {
        ConstructionReplayContext context;
        context.expected_start = measured->start.value();
        if (method_from(*method) == InputMethod::relative_turn) context.previous_segment = measured->previous_segment;
        return replay_construction_receipt(make_receipt(context.expected_start), context);
    }

    void update_measured_preview() {
        if (!measured->start) {
            const auto position = read_coordinate();
            preview->setText(QStringLiteral("Start point preview: X %1, Y %2")
                .arg(display_coordinate(position.x),display_coordinate(position.y)));
            preview->setToolTip(QStringLiteral("Coordinate preview is rounded to three decimals. Entered positions are preserved."));
            status->setText(QStringLiteral("Ready to place the measured start point. Press D for the first edge."));
            return;
        }
        const auto segment = replay_measured().segment;
        preview->setText(QStringLiteral("Endpoint: X %1, Y %2 • length: %3")
            .arg(display_length(segment.end.x),display_length(segment.end.y),display_length(segment_length(segment))));
        status->setText(QStringLiteral("D: next edge • Enter: finish stroke"));
    }

    QString display_length(double metres) const {
        const auto value = metric ? metres : metres / 0.3048;
        return number_text(value) + (metric ? QStringLiteral(" m") : QStringLiteral(" ft"));
    }

    QString display_coordinate(double metres) const {
        auto value=metric ? metres : metres/0.3048;
        if (std::abs(value)<0.0005) value=0.0;
        auto text=QString::number(value,'f',3);
        while (text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
        return text+(metric ? QStringLiteral(" m") : QStringLiteral(" ft"));
    }

    void update_preview(const BoundaryAuthoringSession& value) {
        if (phase==BoundaryAuthoringPhase::awaiting_anchor) {
            const auto anchor=value.view().anchor.value();
            preview->setText(QStringLiteral("Start point preview: X %1, Y %2").arg(display_coordinate(anchor.x),display_coordinate(anchor.y)));
            preview->setToolTip(QStringLiteral("Coordinate preview is rounded to three decimals. Entered positions are preserved."));
            status->setText(QStringLiteral("Ready to place the anchor. Press D on the drawing to enter the first edge."));
            return;
        }
        if (phase==BoundaryAuthoringPhase::awaiting_dimension) {
            const auto position=read_coordinate();
            preview->setText(QStringLiteral("Dimension position preview: X %1, Y %2").arg(display_coordinate(position.x),display_coordinate(position.y)));
            preview->setToolTip(QStringLiteral("Coordinate preview is rounded to three decimals. Entered positions are preserved."));
            status->setText(QStringLiteral("Ready to place the dimension. Press D on the drawing to continue."));
            return;
        }
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
            status->setText(wall ? QStringLiteral("Ready to add %1. Press D for the next wall or Esc to finish the chain.")
                                .arg(method_name(method_from(*method))) : QStringLiteral(
                "Ready to add %1. Press Enter on the drawing to close the area.")
                                .arg(method_name(method_from(*method))));
        }
    }

    void show_error(const std::string& message) {
        error_message = error_text(message);
        error->setText(error_message);
        error->setVisible(true);
        add_button->setEnabled(false);
        status->setText(phase==BoundaryAuthoringPhase::drawing ? QStringLiteral("Enter a valid %1 to continue.")
                            .arg(method_name(method_from(*method))) : QStringLiteral("Enter valid world X and Y coordinates to continue."));
    }

    void refresh_validation() {
        if (loading) {
            return;
        }
        accepted_candidate.reset();
        accepted_receipt.reset();
        accepted_anchor.reset();
        accepted_preferences.reset();
        validation_candidate.reset();
        error_message.clear();
        error->clear();
        error->setVisible(false);
        try {
            if (measured) update_measured_preview();
            else {
                validation_candidate = make_candidate();
                update_preview(*validation_candidate);
            }
            add_button->setEnabled(true);
        } catch (const std::exception& exception) {
            show_error(exception.what());
        }
    }

    BoundaryInputDialog* owner{};
    std::optional<BoundaryAuthoringSession> source;
    std::optional<MeasurementLineworkInputContext> measured;
    BoundaryAuthoringPhase phase{};
    bool metric{};
    BoundaryInputPreferences preferences;
    bool wall{};
    std::optional<BoundaryInputPreferences> accepted_preferences;
    bool loading{true};
    std::optional<BoundaryAuthoringSession> validation_candidate;
    std::optional<BoundaryAuthoringSession> accepted_candidate;
    std::optional<ConstructionReceipt> accepted_receipt;
    std::optional<Vec2> accepted_anchor;
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
                                         bool metricUnits, QWidget* parent,
                                         const BoundaryInputPreferences& preferences,
                                         BoundaryInputPresentation presentation)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, source, metricUnits,preferences,presentation)) {}

BoundaryInputDialog::BoundaryInputDialog(const MeasurementLineworkInputContext& source,
                                         bool metricUnits, QWidget* parent,
                                         const BoundaryInputPreferences& preferences)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, std::nullopt, metricUnits,
        preferences, BoundaryInputPresentation::boundary, source)) {}

BoundaryInputDialog::~BoundaryInputDialog() = default;

std::optional<BoundaryAuthoringSession> BoundaryInputDialog::candidate() const {
    return m_impl->candidate();
}
std::optional<BoundaryInputPreferences> BoundaryInputDialog::acceptedPreferences() const {
    return m_impl->acceptedPreferences();
}
std::optional<ConstructionReceipt> BoundaryInputDialog::receipt() const { return m_impl->receipt(); }
std::optional<Vec2> BoundaryInputDialog::anchor() const { return m_impl->anchor(); }

bool BoundaryInputDialog::submit() { return m_impl->submit(); }

QString BoundaryInputDialog::lastError() const { return m_impl->last_error(); }

}  // namespace sketch::desktop

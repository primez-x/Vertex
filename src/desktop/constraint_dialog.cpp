#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/desktop/constraint_preview_canvas.hpp"
#include "sketch/architecture.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScreen>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {
using json = nlohmann::json;

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

Vec2 point(const json& value) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
        throw std::invalid_argument("Wall endpoint must contain two coordinates");
    const Vec2 result{value[0].get<double>(), value[1].get<double>()};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
        throw std::invalid_argument("Wall endpoint must be finite");
    return result;
}

Segment baseline(const Entity& entity) {
    const auto& value = entity.properties.at("baseline");
    return {point(value.at("start")), point(value.at("end")), value.at("sweep_radians").get<double>()};
}

QString dimension(double metres, bool metric) {
    return QString::number(metric ? metres : metres / 0.3048, 'g', 9) +
        (metric ? QStringLiteral(" m") : QStringLiteral(" ft"));
}

QString exact_number(double value) {
    std::array<char, 768> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
    if (result.ec != std::errc{}) throw std::invalid_argument("Dimension cannot be displayed exactly");
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}

QString editable_dimension(double metres, bool metric) {
    if (!metric) {
        const auto feet = exact_number(metres / 0.3048) + QStringLiteral(" ft");
        try {
            if (feet.size() <= 14 && parse_quantity(feet.toStdString()).metres == metres) return feet;
        } catch (const std::exception&) { /* Fall back to the original SI value. */ }
    }
    return exact_number(metres) + QStringLiteral(" m");
}

// The service validates pure semantics; the desktop additionally checks actual
// architectural solids before presenting an applicable candidate.
void validate_solids(const ConstraintAuthoringPreview& preview) {
    for (const auto& change : preview.changed_walls()) {
        const auto& entity = preview.candidate_entities().at(change.wall_id);
        const auto& p = entity.properties;
        Wall wall{entity.id, baseline(entity), p.at("thickness_m").get<double>(),
                  p.at("height_m").get<double>(), p.at("elevation_m").get<double>(), {}};
        if (const auto layers = p.find("layers"); layers != p.end()) {
            wall.layers = parse_wall_layers(layers.value(), wall.thickness);
        }
        if (const auto slope = p.find("slope_rise_m"); slope != p.end()) {
            if (!slope->is_number()) throw std::invalid_argument("Wall slope_rise_m must be a finite number");
            wall.slope_rise = slope->get<double>();
        }
        for (const auto& [id, opening] : preview.candidate_entities()) {
            if (opening.type != "opening" || opening.properties.value("wall_id", std::string{}) != entity.id)
                continue;
            const auto& o = opening.properties;
            wall.openings.push_back({id, o.at("offset_m").get<double>(), o.at("width_m").get<double>(),
                                      o.at("sill_m").get<double>(), o.at("height_m").get<double>()});
        }
        (void)make_wall(wall);
    }
}
} // namespace

class ConstraintDialog::Impl {
public:
    Impl(ConstraintDialog* owner, DocumentSnapshot source, QString wall_id, bool metric_units)
        : owner(owner), snapshot(std::move(source)), selected_id(wall_id.toStdString()), metric(metric_units) {
        owner->setObjectName(QStringLiteral("constraintDialog"));
        owner->setWindowTitle(QStringLiteral("Wall dimensions and constraints"));
        const auto available = owner->screen()->availableGeometry();
        owner->resize(std::min(710, std::max(360, available.width() - 48)),
                      std::min(730, std::max(300, available.height() - 48)));
        auto* layout = new QVBoxLayout(owner);
        auto* heading = new QLabel(QStringLiteral("Preview the change, then Apply to update the project."), owner);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        auto* scroll = new QScrollArea(owner);
        scroll->setWidgetResizable(true);
        auto* body = new QWidget(scroll);
        auto* body_layout = new QVBoxLayout(body);
        form = new QFormLayout;
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        mode = new QComboBox(body);
        mode->setObjectName(QStringLiteral("constraintOperation"));
        mode->addItems({QStringLiteral("Change wall length"), QStringLiteral("Add constraint"),
                         QStringLiteral("Edit constraint"), QStringLiteral("Remove constraint")});
        form->addRow(QStringLiteral("Operation"), mode);
        existing = new QComboBox(body);
        existing->setObjectName(QStringLiteral("existingConstraint"));
        existing->setMinimumWidth(0);
        existing->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        existing->setMinimumContentsLength(12);
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.type == "wall") {
                try {
                    if (baseline(entity).sweep_radians != 0.0) continue;
                    const auto name = entity.properties.value("name", id);
                    for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                        endpoints.push_back({id, role});
                        endpoint_labels.push_back(text(name) + QStringLiteral(" · ") + text(wall_endpoint_role_name(role)));
                    }
                } catch (const std::exception&) { /* Invalid walls are not available as endpoints. */ }
            } else if (entity.type == "constraint") {
                const auto decoded = decode_constraint_entity(entity);
                if (!decoded.constraint) continue;
                const auto& owners = decoded.constraint->bindings;
                if (std::any_of(owners.begin(), owners.end(), [&](const auto& b) { return b.owner_id == selected_id; }))
                    existing->addItem(text(constraint_relation_name(decoded.constraint->relation)) +
                        QStringLiteral(" · ") + text(id), text(id));
            }
        }
        form->addRow(QStringLiteral("Existing constraint"), existing);
        relation = new QComboBox(body);
        relation->setObjectName(QStringLiteral("constraintRelation"));
        for (const auto kind : {ConstraintRelationKind::horizontal, ConstraintRelationKind::vertical,
            ConstraintRelationKind::coincident, ConstraintRelationKind::fixed_length,
            ConstraintRelationKind::parallel, ConstraintRelationKind::perpendicular, ConstraintRelationKind::fixed_anchor})
            relation->addItem(text(constraint_relation_name(kind)).replace('_', ' '), static_cast<int>(kind));
        form->addRow(QStringLiteral("Relationship"), relation);
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            bindings[index] = new QComboBox(body);
            bindings[index]->setObjectName(QStringLiteral("constraintBinding%1").arg(index));
            bindings[index]->addItems(endpoint_labels);
            bindings[index]->setMinimumWidth(0);
            bindings[index]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            bindings[index]->setMinimumContentsLength(12);
            form->addRow(QStringLiteral("Endpoint %1").arg(index + 1), bindings[index]);
        }
        length = new QLineEdit(body);
        length->setObjectName(QStringLiteral("constraintLength"));
        length->setPlaceholderText(metric ? QStringLiteral("e.g. 4.2 m") : QStringLiteral("e.g. 14 ft"));
        form->addRow(QStringLiteral("Length"), length);
        anchor = new QComboBox(body);
        anchor->setObjectName(QStringLiteral("constraintAnchor"));
        anchor->addItems({QStringLiteral("Keep selected wall start fixed"), QStringLiteral("Keep selected wall end fixed")});
        form->addRow(QStringLiteral("Anchor"), anchor);
        connected = new QCheckBox(QStringLiteral("Allow connected walls to move"), body);
        connected->setObjectName(QStringLiteral("constraintMoveConnected"));
        connected->setChecked(true);
        connected->setToolTip(QStringLiteral("Moves walls linked by explicit constraints. When disabled, other walls stay fixed and incompatible edits are rejected."));
        form->addRow(connected);
        anchor_x = new QLineEdit(body); anchor_y = new QLineEdit(body);
        anchor_x->setObjectName(QStringLiteral("constraintAnchorX"));
        anchor_y->setObjectName(QStringLiteral("constraintAnchorY"));
        form->addRow(QStringLiteral("Fixed point X"), anchor_x);
        form->addRow(QStringLiteral("Fixed point Y"), anchor_y);
        body_layout->addLayout(form);
        canvas = new ConstraintPreviewCanvas(body);
        body_layout->addWidget(canvas, 1);
        changes = new QTableWidget(0, 4, body);
        changes->setObjectName(QStringLiteral("constraintChanges"));
        changes->setHorizontalHeaderLabels({QStringLiteral("Wall"), QStringLiteral("Current length"),
            QStringLiteral("Proposed length"), QStringLiteral("Max endpoint move")});
        changes->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        changes->horizontalHeader()->setStretchLastSection(true);
        changes->setEditTriggers(QAbstractItemView::NoEditTriggers);
        changes->setMaximumHeight(150);
        changes->setMinimumHeight(85);
        body_layout->addWidget(changes);
        scroll->setWidget(body);
        layout->addWidget(scroll, 1);
        status = new QPlainTextEdit(owner);
        status->setObjectName(QStringLiteral("constraintStatus"));
        status->setAccessibleName(QStringLiteral("Constraint preview result"));
        status->setReadOnly(true);
        status->setTabChangesFocus(true);
        status->setFrameShape(QFrame::NoFrame);
        status->setMinimumHeight(55);
        status->setMaximumHeight(100);
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, owner);
        preview_button = buttons->addButton(QStringLiteral("Preview"), QDialogButtonBox::ActionRole);
        preview_button->setObjectName(QStringLiteral("constraintPreviewButton"));
        apply_button = buttons->button(QDialogButtonBox::Apply);
        apply_button->setObjectName(QStringLiteral("constraintApplyButton"));
        apply_button->setEnabled(false);
        apply_button->setAutoDefault(false);
        preview_button->setDefault(true);
        layout->addWidget(buttons);
        QObject::connect(preview_button, &QPushButton::clicked, owner, [this] { (void)previewEdit(); });
        QObject::connect(apply_button, &QPushButton::clicked, owner, [this] { (void)submit(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner, &QDialog::reject);
        QObject::connect(mode, &QComboBox::currentIndexChanged, owner, [this] { configure(true); });
        QObject::connect(existing, &QComboBox::currentIndexChanged, owner, [this] { configure(true); });
        QObject::connect(relation, &QComboBox::currentIndexChanged, owner, [this] { if (!loading) configure(false); });
        for (auto* field : {length, anchor_x, anchor_y})
            QObject::connect(field, &QLineEdit::textChanged, owner, [this] { invalidate(); });
        for (auto* field : bindings)
            QObject::connect(field, &QComboBox::currentIndexChanged, owner, [this] { invalidate(); });
        QObject::connect(anchor, &QComboBox::currentIndexChanged, owner, [this] { invalidate(); });
        QObject::connect(connected, &QCheckBox::toggled, owner, [this] { invalidate(); });
        configure(true);
    }

    void invalidate() {
        if (loading) return;
        preview.reset(); accepted.reset(); apply_button->setEnabled(false);
        canvas->setWalls({}); changes->setRowCount(0);
        error.clear(); status->setStyleSheet({});
        status->setPlainText(QStringLiteral("Preview required before Apply."));
    }

    void selectBinding(std::size_t index, const WallEndpointBinding& value) {
        for (std::size_t i = 0; i < endpoints.size(); ++i)
            if (endpoints[i] == value) { bindings[index]->setCurrentIndex(static_cast<int>(i)); return; }
        bindings[index]->setCurrentIndex(-1);
    }

    void configure(bool load_values) {
        loading = true;
        const auto operation = mode->currentIndex();
        if (load_values) {
            selected_constraint.reset();
            if (operation >= 2 && existing->currentIndex() >= 0) {
                selected_constraint = *decode_constraint_entity(snapshot.entities().at(existing->currentData().toString().toStdString())).constraint;
                const auto& c = *selected_constraint;
                relation->setCurrentIndex(relation->findData(static_cast<int>(c.relation)));
                for (std::size_t i = 0; i < c.bindings.size(); ++i) selectBinding(i, c.bindings[i]);
                if (c.length) length->setText(text(c.length->original_expression));
                if (c.anchor) {
                    anchor_x->setText(editable_dimension(c.anchor->x, true));
                    anchor_y->setText(editable_dimension(c.anchor->y, true));
                }
            } else {
                selectBinding(0, {selected_id, WallEndpointRole::start});
                selectBinding(1, {selected_id, WallEndpointRole::end});
                for (const auto& endpoint : endpoints)
                    if (endpoint.owner_id != selected_id) {
                        selectBinding(2, {endpoint.owner_id, WallEndpointRole::start});
                        selectBinding(3, {endpoint.owner_id, WallEndpointRole::end});
                        break;
                    }
                try {
                    const auto segment = baseline(snapshot.entities().at(selected_id));
                    length->setText(editable_dimension(segment_length(segment), metric));
                    anchor_x->setText(editable_dimension(segment.start.x, true));
                    anchor_y->setText(editable_dimension(segment.start.y, true));
                } catch (const std::exception&) { length->clear(); }
            }
        }
        const auto kind = static_cast<ConstraintRelationKind>(relation->currentData().toInt());
        const bool editing_relation = operation == 1 || operation == 2;
        const auto count = kind == ConstraintRelationKind::fixed_anchor ? 1U :
            (kind == ConstraintRelationKind::parallel || kind == ConstraintRelationKind::perpendicular ? 4U : 2U);
        form->setRowVisible(existing, operation >= 2);
        form->setRowVisible(relation, editing_relation);
        for (std::size_t i = 0; i < bindings.size(); ++i) form->setRowVisible(bindings[i], editing_relation && i < count);
        form->setRowVisible(length, operation == 0 || (editing_relation && kind == ConstraintRelationKind::fixed_length));
        form->setRowVisible(anchor, operation != 3);
        form->setRowVisible(connected, operation != 3);
        form->setRowVisible(anchor_x, editing_relation && kind == ConstraintRelationKind::fixed_anchor);
        form->setRowVisible(anchor_y, editing_relation && kind == ConstraintRelationKind::fixed_anchor);
        loading = false;
        invalidate();
    }

    ConstraintAuthoringIntent intent() const {
        ConstraintAuthoringIntent result;
        const auto unit = metric ? Unit::metre : Unit::foot;
        const auto role = anchor->currentIndex() == 0 ? WallEndpointRole::start : WallEndpointRole::end;
        const auto operation = mode->currentIndex();
        if (operation == 0) {
            result.wall_resize = WallResizeIntent{selected_id, parse_quantity(length->text().toStdString(), unit),
                role == WallEndpointRole::start ? WallResizeAnchor::start : WallResizeAnchor::end, connected->isChecked()};
            result.message = "change wall length with preview";
        } else {
            result.relation_anchor = WallEndpointBinding{selected_id, role};
            result.relation_move_connected_walls = connected->isChecked();
            if (operation == 3) {
                if (!selected_constraint) throw std::invalid_argument("Select an existing constraint to remove");
                result.relation_mutations.push_back(ConstraintRelationMutation::remove(selected_constraint->id));
                result.message = "remove wall constraint";
                return result;
            }
            if (operation == 2 && !selected_constraint) throw std::invalid_argument("Select an existing constraint to edit");
            PersistentConstraint value;
            value.id = operation == 2 ? selected_constraint->id : new_constraint_id;
            value.relation = static_cast<ConstraintRelationKind>(relation->currentData().toInt());
            const auto count = value.relation == ConstraintRelationKind::fixed_anchor ? 1U :
                (value.relation == ConstraintRelationKind::parallel || value.relation == ConstraintRelationKind::perpendicular ? 4U : 2U);
            for (std::size_t i = 0; i < count; ++i) {
                const auto index = bindings[i]->currentIndex();
                if (index < 0 || static_cast<std::size_t>(index) >= endpoints.size())
                    throw std::invalid_argument("Select every endpoint required by the relationship");
                value.bindings.push_back(endpoints[static_cast<std::size_t>(index)]);
            }
            if (value.relation == ConstraintRelationKind::fixed_length)
                value.length = parse_quantity(length->text().toStdString(), unit);
            if (value.relation == ConstraintRelationKind::fixed_anchor)
                value.anchor = Vec2{parse_quantity(anchor_x->text().toStdString(), unit).metres,
                                    parse_quantity(anchor_y->text().toStdString(), unit).metres};
            result.relation_mutations.push_back(ConstraintRelationMutation::upsert(std::move(value)));
            result.message = operation == 1 ? "add wall constraint" : "edit wall constraint";
        }
        return result;
    }

    bool previewEdit() {
        invalidate();
        try {
            auto candidate = preview_constraint_authoring(snapshot, intent());
            if (!candidate.accepted()) {
                QStringList reasons;
                for (const auto& diagnostic : candidate.diagnostics()) reasons.push_back(text(diagnostic));
                throw std::invalid_argument(reasons.join(QStringLiteral("\n")).toStdString());
            }
            validate_solids(candidate);
            std::vector<WallPreviewDrawing> drawing;
            QStringList summary;
            changes->setRowCount(static_cast<int>(candidate.changed_walls().size()));
            int row = 0;
            for (const auto& wall : candidate.changed_walls()) {
                drawing.push_back({text(wall.wall_id), wall.old_baseline, wall.proposed_baseline});
                const auto displacement = std::max(
                    std::hypot(wall.old_baseline.start.x - wall.proposed_baseline.start.x, wall.old_baseline.start.y - wall.proposed_baseline.start.y),
                    std::hypot(wall.old_baseline.end.x - wall.proposed_baseline.end.x, wall.old_baseline.end.y - wall.proposed_baseline.end.y));
                const auto& properties = candidate.candidate_entities().at(wall.wall_id).properties;
                const auto name = properties.find("name");
                const auto display_name = name != properties.end() && name->is_string() && !name->get_ref<const std::string&>().empty()
                    ? text(name->get_ref<const std::string&>())
                    : wall.wall_id == selected_id ? QStringLiteral("Selected wall") : text(wall.wall_id);
                const QStringList values{display_name, dimension(segment_length(wall.old_baseline), metric),
                    dimension(segment_length(wall.proposed_baseline), metric), dimension(displacement, metric)};
                if (summary.size() < 3)
                    summary.push_back(QStringLiteral("%1: %2 → %3; endpoint movement up to %4")
                        .arg(values[0], values[1], values[2], values[3]));
                for (int column = 0; column < values.size(); ++column) {
                    auto* item = new QTableWidgetItem(values[column]);
                    item->setToolTip(text(wall.wall_id));
                    changes->setItem(row, column, item);
                }
                ++row;
            }
            if (drawing.empty()) {
                const auto line = baseline(snapshot.entities().at(selected_id));
                drawing.push_back({text(selected_id), line, line});
            }
            canvas->setWalls(std::move(drawing));
            preview = std::move(candidate);
            apply_button->setEnabled(true);
            if (summary.isEmpty()) summary.push_back(QStringLiteral("Constraint change only; wall geometry stays in place."));
            if (preview->changed_walls().size() > 3)
                summary.push_back(QStringLiteral("%1 more walls are listed in the preview details.").arg(preview->changed_walls().size() - 3));
            for (const auto& diagnostic : preview->diagnostics()) summary.push_back(text(diagnostic));
            summary.push_back(QStringLiteral("Apply records one undoable command."));
            status->setPlainText(summary.join('\n'));
            return true;
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
            status->setStyleSheet(status->palette().color(QPalette::Window).lightness() < 128
                ? QStringLiteral("color:#ffb4a2;") : QStringLiteral("color:#9f1b11;"));
            status->setPlainText(error.isEmpty() ? QStringLiteral("The requested constraints could not be satisfied.") : error);
            return false;
        }
    }

    bool submit() {
        if (!preview || !preview->accepted()) {
            if (error.isEmpty()) error = QStringLiteral("Preview the current edit before applying it.");
            status->setPlainText(error);
            return false;
        }
        accepted = preview;
        owner->accept();
        return true;
    }

    ConstraintDialog* owner;
    DocumentSnapshot snapshot;
    std::string selected_id;
    bool metric{};
    bool loading{};
    std::string new_constraint_id = make_stable_id();
    std::vector<WallEndpointBinding> endpoints;
    QStringList endpoint_labels;
    std::optional<PersistentConstraint> selected_constraint;
    std::optional<ConstraintAuthoringPreview> preview, accepted;
    QString error;
    QFormLayout* form{};
    QComboBox *mode{}, *existing{}, *relation{}, *anchor{};
    std::array<QComboBox*, 4> bindings{};
    QLineEdit *length{}, *anchor_x{}, *anchor_y{};
    QCheckBox* connected{};
    ConstraintPreviewCanvas* canvas{};
    QTableWidget* changes{};
    QPlainTextEdit* status{};
    QPushButton *apply_button{}, *preview_button{};
};

ConstraintDialog::ConstraintDialog(DocumentSnapshot snapshot, QString selected_wall_id,
                                   bool metric_units, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, std::move(snapshot), std::move(selected_wall_id), metric_units)) {}
ConstraintDialog::~ConstraintDialog() = default;
void ConstraintDialog::setLengthExpression(const QString& expression) { m_impl->length->setText(expression); }
bool ConstraintDialog::previewEdit() { return m_impl->previewEdit(); }
bool ConstraintDialog::submit() { return m_impl->submit(); }
std::optional<ConstraintAuthoringPreview> ConstraintDialog::acceptedPreview() const { return m_impl->accepted; }
QString ConstraintDialog::lastError() const { return m_impl->error; }

} // namespace sketch::desktop

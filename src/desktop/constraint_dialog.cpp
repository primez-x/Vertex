#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/desktop/constraint_preview_canvas.hpp"
#include "sketch/architecture.hpp"
#include "sketch/boundary_entity.hpp"

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
#include <set>
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

PersistentConstraintComponentAnalysis stored_component_analysis(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::vector<std::string>& owners, std::optional<Revision> revision = std::nullopt) {
    try {
        return analyze_persistent_constraint_component(entities, owners, revision);
    } catch (const std::exception& exception) {
        PersistentConstraintComponentAnalysis unavailable;
        unavailable.diagnostics.push_back(exception.what());
        return unavailable;
    }
}

QString stored_freedom_value(const PersistentConstraintComponentAnalysis& analysis) {
    return analysis.supported && analysis.degrees_of_freedom >= 0
        ? QString::number(analysis.degrees_of_freedom) : QStringLiteral("unavailable");
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
        const auto& selected = snapshot.entities().at(selected_id);
        if (!ConstraintDialog::supportsEntity(selected))
            throw std::invalid_argument("Select a straight wall or an identified straight boundary");
        boundary_mode = selected.type != "wall";
        owner->setWindowTitle(boundary_mode ? QStringLiteral("Boundary dimensions and constraints") : QStringLiteral("Wall dimensions and constraints"));
        const auto available = owner->screen()->availableGeometry();
        owner->resize(std::min(710, std::max(360, available.width() - 48)),
                      std::min(730, std::max(300, available.height() - 48)));
        auto* layout = new QVBoxLayout(owner);
        auto* heading = new QLabel(QStringLiteral("Preview the change, then Apply to update the project."), owner);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        persistent_freedom = new QLabel(owner);
        persistent_freedom->setObjectName(QStringLiteral("constraintPersistentFreedom"));
        persistent_freedom->setAccessibleName(QStringLiteral("Stored component coordinate freedom"));
        persistent_freedom->setWordWrap(true);
        layout->addWidget(persistent_freedom);
        auto* scroll = new QScrollArea(owner);
        scroll->setWidgetResizable(true);
        auto* body = new QWidget(scroll);
        auto* body_layout = new QVBoxLayout(body);
        form = new QFormLayout;
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        mode = new QComboBox(body);
        mode->setObjectName(QStringLiteral("constraintOperation"));
        if (!boundary_mode) mode->addItem(QStringLiteral("Change wall length"), 0);
        mode->addItem(QStringLiteral("Add constraint"), 1);
        mode->addItem(QStringLiteral("Edit constraint"), 2);
        mode->addItem(QStringLiteral("Remove constraint"), 3);
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
                        endpoint_labels.push_back(QStringLiteral("Wall · ") + text(name) + QStringLiteral(" · ") + text(wall_endpoint_role_name(role)));
                    }
                } catch (const std::exception&) { /* Invalid walls are not available as endpoints. */ }
            } else if (can_recognize_boundary_entity_type(entity.type) && ConstraintDialog::supportsEntity(entity)) {
                const auto boundary = decode_identified_boundary_entity(entity);
                const auto name = text(entity.properties.value("name", id));
                for (std::size_t i = 0; i < boundary.segments.size(); ++i) {
                    const auto& edge = boundary.segments[i];
                    for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                        const auto& vertex = role == WallEndpointRole::start ? edge.start_vertex_id : edge.end_vertex_id;
                        endpoints.push_back({id, role, edge.segment_id, vertex});
                        endpoint_labels.push_back((entity.type == "room_boundary" ? QStringLiteral("Room boundary · ") : QStringLiteral("Boundary · ")) +
                            name + QStringLiteral(" · Edge %1 · ").arg(i + 1) + text(wall_endpoint_role_name(role)));
                    }
                }
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
        form->addRow(QStringLiteral("Anchor"), anchor);
        connected = new QCheckBox(QStringLiteral("Allow connected objects to move"), body);
        connected->setObjectName(QStringLiteral("constraintMoveConnected"));
        connected->setChecked(true);
        connected->setToolTip(QStringLiteral("Moves objects linked by explicit constraints. When disabled, other objects stay fixed and incompatible edits are rejected."));
        form->addRow(connected);
        anchor_x = new QLineEdit(body); anchor_y = new QLineEdit(body);
        anchor_x->setObjectName(QStringLiteral("constraintAnchorX"));
        anchor_y->setObjectName(QStringLiteral("constraintAnchorY"));
        form->addRow(QStringLiteral("Fixed point X"), anchor_x);
        form->addRow(QStringLiteral("Fixed point Y"), anchor_y);
        body_layout->addLayout(form);
        canvas = new ConstraintPreviewCanvas(body);
        canvas->setAccessibleName(QStringLiteral("Object movement preview"));
        canvas->setAccessibleDescription(QStringLiteral("Dashed gray lines show current walls and boundary edges. Blue lines show proposed geometry. Unchanged relationship partners provide context."));
        body_layout->addWidget(canvas, 1);
        changes = new QTableWidget(0, 4, body);
        changes->setObjectName(QStringLiteral("constraintChanges"));
        changes->setHorizontalHeaderLabels({QStringLiteral("Wall / boundary edge"), QStringLiteral("Current length"),
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
        source_freedom = stored_component_analysis(snapshot.entities(), {selected_id}, snapshot.revision());
        configure(true);
    }

    QString stored_constraint_names(const std::vector<std::string>& ids,
        const std::map<std::string, Entity, std::less<>>& entities) const {
        QStringList names;
        for (const auto& id : ids) {
            const auto found = entities.find(id);
            if (found == entities.end()) continue;
            const auto name = found->second.properties.find("name");
            if (name != found->second.properties.end() && name->is_string() && !name->get_ref<const std::string&>().empty()) {
                names.push_back(text(name->get_ref<const std::string&>()));
                continue;
            }
            try {
                const auto decoded = decode_constraint_entity(found->second);
                if (decoded.constraint)
                    names.push_back(text(constraint_relation_name(decoded.constraint->relation)).replace('_', ' ') + QStringLiteral(" constraint"));
            } catch (const std::exception&) { /* Unsupported semantics have their own diagnostic. */ }
        }
        return names.join(QStringLiteral(", "));
    }

    void showPersistentFreedom(const ConstraintAuthoringPreview* candidate = nullptr) {
        auto before = source_freedom;
        std::optional<PersistentConstraintComponentAnalysis> after;
        std::set<std::string> owners{selected_id};
        owners.insert(before.owner_ids.begin(), before.owner_ids.end());
        if (candidate) {
            // A removed or redirected relation can disconnect a component.
            // Expand through both states until they share one owner universe.
            for (;;) {
                const std::vector<std::string> seeds(owners.begin(), owners.end());
                before = stored_component_analysis(snapshot.entities(), seeds, snapshot.revision());
                after = stored_component_analysis(candidate->candidate_entities(), seeds);
                auto expanded = owners;
                expanded.insert(before.owner_ids.begin(), before.owner_ids.end());
                expanded.insert(after->owner_ids.begin(), after->owner_ids.end());
                if (expanded == owners) break;
                owners = std::move(expanded);
            }
        }
        QString value = QStringLiteral("Stored coordinate freedom: %1").arg(stored_freedom_value(before));
        if (after) {
            value += QStringLiteral(" → %1").arg(stored_freedom_value(*after));
            if (before.supported && after->supported && before.degrees_of_freedom >= 0 && after->degrees_of_freedom >= 0) {
                const auto delta = after->degrees_of_freedom - before.degrees_of_freedom;
                value += QStringLiteral(" (%1%2)").arg(delta >= 0 ? QStringLiteral("+") : QString{}).arg(delta);
            }
        }
        if (before.supported && (!after || after->supported))
            value += owners.size() == 1 ? QStringLiteral(" · 1 object")
                : QStringLiteral(" · %1 objects").arg(static_cast<qulonglong>(owners.size()));
        if (!before.redundant_constraint_ids.empty() || (after && !after->redundant_constraint_ids.empty())) {
            value += QStringLiteral(" · redundant constraints: %1").arg(static_cast<qulonglong>(before.redundant_constraint_ids.size()));
            if (after) value += QStringLiteral(" → %1").arg(static_cast<qulonglong>(after->redundant_constraint_ids.size()));
        }
        QStringList details{QStringLiteral("Independent X/Y endpoint-coordinate freedoms under stored constraints. Includes translation and rotation when stored relations permit them. Does not count wall thickness, height, or other architectural parameters, and excludes temporary edit anchors and pins. Before and after use the same owner set.")};
        const auto append_details = [&](const PersistentConstraintComponentAnalysis& analysis,
            const std::map<std::string, Entity, std::less<>>& entities, const QString& phase) {
            if (!analysis.redundant_constraint_ids.empty())
                details.push_back(phase + QStringLiteral(" redundant stored relations: ") + stored_constraint_names(analysis.redundant_constraint_ids, entities));
            if (!analysis.conflicting_constraint_ids.empty())
                details.push_back(phase + QStringLiteral(" conflicting stored relations: ") + stored_constraint_names(analysis.conflicting_constraint_ids, entities));
            for (const auto& diagnostic : analysis.diagnostics)
                details.push_back(phase + QStringLiteral(" stored component: ") + text(diagnostic));
        };
        append_details(before, snapshot.entities(), QStringLiteral("Current"));
        if (after) append_details(*after, candidate->candidate_entities(), QStringLiteral("Proposed"));
        persistent_freedom->setText(value);
        persistent_freedom->setToolTip(details.join('\n'));
    }

    QString owner_label(const std::string& id) const {
        const auto& entity = snapshot.entities().at(id);
        return (entity.type == "wall" ? QStringLiteral("Wall · ") :
            entity.type == "room_boundary" ? QStringLiteral("Room boundary · ") : QStringLiteral("Boundary · ")) +
            text(entity.properties.value("name", id));
    }

    void appendCurrentGeometry(std::vector<WallPreviewDrawing>& drawing, const std::string& id) const {
        const auto& entity = snapshot.entities().at(id);
        if (!ConstraintDialog::supportsEntity(entity)) return;
        if (entity.type == "wall") {
            const auto line = baseline(entity);
            drawing.push_back({owner_label(id), line, line});
        } else {
            const auto boundary = decode_identified_boundary_entity(entity);
            for (std::size_t i = 0; i < boundary.segments.size(); ++i) {
                const auto& edge = boundary.segments[i].segment;
                drawing.push_back({owner_label(id) + QStringLiteral(" · Edge %1").arg(i + 1), edge, edge});
            }
        }
    }

    void invalidate() {
        if (loading) return;
        preview.reset(); accepted.reset(); apply_button->setEnabled(false);
        showPersistentFreedom();
        std::vector<WallPreviewDrawing> current;
        appendCurrentGeometry(current, selected_id);
        canvas->setWalls(std::move(current)); changes->setRowCount(0);
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
        const auto operation = mode->currentData().toInt();
        const bool wall_resize = operation == 0;
        if (anchor->count() == 0 || anchor_for_wall_resize != wall_resize) {
            anchor->clear();
            if (wall_resize) {
                anchor->addItems({QStringLiteral("Keep selected wall start fixed"), QStringLiteral("Keep selected wall end fixed")});
            } else {
                for (std::size_t i = 0; i < endpoints.size(); ++i)
                    anchor->addItem(QStringLiteral("Keep ") + endpoint_labels[static_cast<int>(i)] + QStringLiteral(" fixed"), static_cast<int>(i));
                for (std::size_t i = 0; i < endpoints.size(); ++i)
                    if (endpoints[i].owner_id == selected_id) { anchor->setCurrentIndex(static_cast<int>(i)); break; }
            }
            anchor_for_wall_resize = wall_resize;
        }
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
                for (std::size_t i = 0; i + 1 < endpoints.size(); i += 2) {
                    if (endpoints[i].owner_id == selected_id) {
                        selectBinding(0, endpoints[i]); selectBinding(1, endpoints[i + 1]); break;
                    }
                }
                bindings[2]->setCurrentIndex(-1); bindings[3]->setCurrentIndex(-1);
                for (std::size_t i = 0; i + 1 < endpoints.size(); i += 2)
                    if (endpoints[i].owner_id != selected_id) {
                        selectBinding(2, endpoints[i]); selectBinding(3, endpoints[i + 1]); break;
                    }
                try {
                    const auto segment = boundary_mode ? decode_identified_boundary_entity(snapshot.entities().at(selected_id)).segments.front().segment : baseline(snapshot.entities().at(selected_id));
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
        const auto operation = mode->currentData().toInt();
        if (operation == 0) {
            result.wall_resize = WallResizeIntent{selected_id, parse_quantity(length->text().toStdString(), unit),
                role == WallEndpointRole::start ? WallResizeAnchor::start : WallResizeAnchor::end, connected->isChecked()};
            result.message = "change wall length with preview";
        } else {
            const auto anchor_index = anchor->currentData().toInt();
            if (anchor->currentIndex() < 0 || anchor_index < 0 || static_cast<std::size_t>(anchor_index) >= endpoints.size())
                throw std::invalid_argument("Select an endpoint to keep fixed");
            result.relation_anchor = endpoints[static_cast<std::size_t>(anchor_index)];
            result.relation_move_connected_walls = connected->isChecked();
            if (operation == 3) {
                if (!selected_constraint) throw std::invalid_argument("Select an existing constraint to remove");
                result.relation_mutations.push_back(ConstraintRelationMutation::remove(selected_constraint->id));
                result.message = "remove geometry constraint";
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
            result.message = operation == 1 ? "add geometry constraint" : "edit geometry constraint";
        }
        return result;
    }

    bool previewEdit() {
        invalidate();
        try {
            const auto command = intent();
            auto candidate = preview_constraint_authoring(snapshot, command);
            if (!candidate.accepted()) {
                QStringList reasons;
                for (const auto& diagnostic : candidate.diagnostics()) reasons.push_back(text(diagnostic));
                throw std::invalid_argument(reasons.join(QStringLiteral("\n")).toStdString());
            }
            validate_solids(candidate);
            std::vector<WallPreviewDrawing> drawing;
            std::set<std::string> drawn_owners;
            QStringList summary;
            changes->setRowCount(static_cast<int>(candidate.changed_walls().size()));
            int row = 0;
            for (const auto& wall : candidate.changed_walls()) {
                drawn_owners.insert(wall.wall_id);
                drawing.push_back({owner_label(wall.wall_id), wall.old_baseline, wall.proposed_baseline});
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
            for (const auto& boundary : candidate.changed_boundaries()) {
                drawn_owners.insert(boundary.before.id);
                for (std::size_t i = 0; i < boundary.before.segments.size(); ++i) {
                    const auto& before = boundary.before.segments[i].segment;
                    const auto& after = boundary.after.segments[i].segment;
                    const auto label = owner_label(boundary.before.id) + QStringLiteral(" · Edge %1").arg(i + 1);
                    drawing.push_back({label, before, after});
                    const auto displacement = std::max(std::hypot(before.start.x - after.start.x, before.start.y - after.start.y), std::hypot(before.end.x - after.end.x, before.end.y - after.end.y));
                    changes->insertRow(row);
                    const QStringList values{label, dimension(segment_length(before), metric), dimension(segment_length(after), metric), dimension(displacement, metric)};
                    for (int column = 0; column < values.size(); ++column) changes->setItem(row, column, new QTableWidgetItem(values[column]));
                    ++row;
                }
                summary.push_back(QStringLiteral("%1: boundary movement shown in preview.").arg(text(boundary.before.id)));
            }
            std::set<std::string> context_owners{selected_id};
            if (command.relation_anchor) context_owners.insert(command.relation_anchor->owner_id);
            if (selected_constraint)
                for (const auto& binding : selected_constraint->bindings) context_owners.insert(binding.owner_id);
            // Include partners of the relation being previewed, without adding
            // every unchanged object in the connected component to the view.
            for (const auto& mutation : command.relation_mutations)
                if (mutation.kind == ConstraintRelationMutationKind::upsert)
                    for (const auto& binding : mutation.constraint.bindings) context_owners.insert(binding.owner_id);
            for (const auto& id : context_owners)
                if (!drawn_owners.contains(id)) appendCurrentGeometry(drawing, id);
            canvas->setWalls(std::move(drawing));
            preview = std::move(candidate);
            showPersistentFreedom(&*preview);
            apply_button->setEnabled(true);
            if (summary.isEmpty()) summary.push_back(QStringLiteral("Constraint change only; geometry stays in place."));
            if (preview->changed_walls().size() > 3)
                summary.push_back(QStringLiteral("%1 more walls are listed in the preview details.").arg(preview->changed_walls().size() - 3));
            for (const auto& diagnostic : preview->diagnostics())
                summary.push_back(QStringLiteral("Edit preview: ") + text(diagnostic));
            summary.push_back(QStringLiteral("Apply records one undoable command."));
            status->setPlainText(summary.join('\n'));
            return true;
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
            status->setStyleSheet(status->palette().color(QPalette::Window).lightness() < 128
                ? QStringLiteral("color:#ffb4a2;") : QStringLiteral("color:#9f1b11;"));
            status->setPlainText(QStringLiteral("Edit preview: ") +
                (error.isEmpty() ? QStringLiteral("The requested constraints could not be satisfied.") : error));
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
    bool boundary_mode{};
    bool loading{};
    bool anchor_for_wall_resize{};
    std::string new_constraint_id = make_stable_id();
    std::vector<WallEndpointBinding> endpoints;
    QStringList endpoint_labels;
    std::optional<PersistentConstraint> selected_constraint;
    std::optional<ConstraintAuthoringPreview> preview, accepted;
    PersistentConstraintComponentAnalysis source_freedom;
    QString error;
    QFormLayout* form{};
    QComboBox *mode{}, *existing{}, *relation{}, *anchor{};
    std::array<QComboBox*, 4> bindings{};
    QLineEdit *length{}, *anchor_x{}, *anchor_y{};
    QCheckBox* connected{};
    ConstraintPreviewCanvas* canvas{};
    QTableWidget* changes{};
    QPlainTextEdit* status{};
    QLabel* persistent_freedom{};
    QPushButton *apply_button{}, *preview_button{};
};

bool ConstraintDialog::supportsEntity(const Entity& entity) noexcept {
    try {
        if (entity.type == "wall") return baseline(entity).sweep_radians == 0.0;
        if (!can_recognize_boundary_entity_type(entity.type)) return false;
        const auto boundary = decode_identified_boundary_entity(entity);
        return !boundary.segments.empty() && std::all_of(boundary.segments.begin(), boundary.segments.end(),
            [](const auto& edge) { return edge.segment.sweep_radians == 0.0; });
    } catch (...) { return false; }
}

ConstraintDialog::ConstraintDialog(DocumentSnapshot snapshot, QString selected_entity_id,
                                   bool metric_units, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, std::move(snapshot), std::move(selected_entity_id), metric_units)) {}
ConstraintDialog::~ConstraintDialog() = default;
void ConstraintDialog::setLengthExpression(const QString& expression) { m_impl->length->setText(expression); }
bool ConstraintDialog::previewEdit() { return m_impl->previewEdit(); }
bool ConstraintDialog::submit() { return m_impl->submit(); }
std::optional<ConstraintAuthoringPreview> ConstraintDialog::acceptedPreview() const { return m_impl->accepted; }
QString ConstraintDialog::lastError() const { return m_impl->error; }

} // namespace sketch::desktop

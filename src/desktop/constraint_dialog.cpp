#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/desktop/constraint_preview_canvas.hpp"
#include "sketch/architecture.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_wall_replacement_request.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/desktop/physical_wall_phase_room_review_dialog.hpp"
#include "plan_canvas.hpp"

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

struct EndpointEdge { std::string segment_id, start_vertex_id, end_vertex_id; Segment segment; };
std::vector<EndpointEdge> endpoint_edges(const Entity& entity) {
    std::vector<EndpointEdge> result;
    if(entity.type=="measurement_linework") {
        const auto decoded=decode_measurement_linework_model(entity.properties.at("model"));
        if(!decoded.supported())throw std::invalid_argument(decoded.diagnostic);
        for(const auto& edge:replay_measurement_linework(*decoded.model).edges)
            result.push_back({edge.segment_id,edge.start_vertex_id,edge.end_vertex_id,edge.segment});
    } else {
        for(const auto& edge:decode_identified_boundary_entity(entity).segments)
            result.push_back({edge.segment_id,edge.start_vertex_id,edge.end_vertex_id,edge.segment});
    }
    return result;
}

QString dimension(double metres, bool metric) {
    return PlanCanvas::drawingLengthText(metres, metric);
}

QString relation_label(ConstraintRelationKind kind) {
    if (kind == ConstraintRelationKind::fixed_length) return QStringLiteral("Endpoint distance");
    if (kind == ConstraintRelationKind::fixed_arc_length) return QStringLiteral("Curve length");
    return text(constraint_relation_name(kind)).replace('_', ' ');
}

QString diagnostic_text(std::string_view value) {
    auto result = text(value);
    result.replace(QStringLiteral("fixed_arc_length"), QStringLiteral("Curve length"));
    result.replace(QStringLiteral("fixed physical arc length"), QStringLiteral("Curve length"));
    result.replace(QStringLiteral("Fixed arc length"), QStringLiteral("Curve length"));
    return result;
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
    const std::vector<std::string>& owners, bool active_phase) {
    try {
        return active_phase ? analyze_active_phase_persistent_constraint_component(entities, owners)
                            : analyze_persistent_constraint_component(entities, owners);
    } catch (const std::exception& exception) {
        PersistentConstraintComponentAnalysis unavailable;
        unavailable.diagnostics.push_back(exception.what());
        return unavailable;
    }
}

PersistentConstraintComponentAnalysis stored_component_analysis(
    const DocumentSnapshot& snapshot, const std::vector<std::string>& owners) {
    try {
        return analyze_persistent_constraint_component(snapshot, owners);
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
void validate_solids(const PhaseWallReplacementEntities& entities, const std::vector<std::string>& wall_ids) {
    for (const auto& wall_id : wall_ids) {
        const auto& entity = entities.at(wall_id);
        const auto& p = entity.properties;
        Wall wall{entity.id, baseline(entity), p.at("thickness_m").get<double>(),
                  p.at("height_m").get<double>(), p.at("elevation_m").get<double>(), {}};
        if (const auto layers = p.find("layers"); layers != p.end()) {
            wall.layers = parse_wall_layers(layers.value(), wall.thickness);
        }
        std::string top_error;
        if (!read_document_wall_top_profile(entity,wall,top_error)) throw std::invalid_argument(top_error);
        for (const auto& [id, opening] : entities) {
            if (opening.type != "opening" || opening.properties.value("wall_id", std::string{}) != entity.id)
                continue;
            const auto& o = opening.properties;
            wall.openings.push_back({id, o.at("offset_m").get<double>(), o.at("width_m").get<double>(),
                                      o.at("sill_m").get<double>(), o.at("height_m").get<double>()});
        }
        (void)make_wall(wall);
    }
}
void validate_solids(const ConstraintAuthoringPreview& preview) {
    std::vector<std::string> wall_ids;
    for (const auto& change : preview.changed_walls()) wall_ids.push_back(change.wall_id);
    validate_solids(preview.candidate_entities(), wall_ids);
}
} // namespace

class ConstraintDialog::Impl {
public:
    Impl(ConstraintDialog* owner, DocumentSnapshot source, QString wall_id, bool metric_units)
        : owner(owner), snapshot(std::move(source)), selected_id(wall_id.toStdString()), metric(metric_units) {
        owner->setObjectName(QStringLiteral("constraintDialog"));
        active_phase = snapshot.uses_active_phase_constraints() ||
            std::any_of(snapshot.entities().begin(), snapshot.entities().end(),
                [](const auto& entry) { return entry.second.type == "model_phases"; });
        if (active_phase) phase_scope = constraint_phase_scope(snapshot.entities());
        if (!ownerParticipates(selected_id))
            throw std::invalid_argument("The selected object is inactive in the saved phase. Select an active object to edit its constraints.");
        const auto& selected = snapshot.entities().at(selected_id);
        if (!ConstraintDialog::supportsEntity(selected))
            throw std::invalid_argument("Select a valid wall, measured stroke or identified boundary");
        measured_mode = selected.type == "measurement_linework";
        boundary_mode = selected.type != "wall";
        curved_wall = !boundary_mode && baseline(selected).sweep_radians != 0.0;
        owner->setWindowTitle(measured_mode ? QStringLiteral("Measured stroke dimensions and constraints") : boundary_mode ? QStringLiteral("Boundary dimensions and constraints") : QStringLiteral("Wall dimensions and constraints"));
        const auto available = owner->screen()->availableGeometry();
        owner->resize(std::min(710, std::max(360, available.width() - 48)),
                      std::min(730, std::max(300, available.height() - 48)));
        auto* layout = new QVBoxLayout(owner);
        auto* heading = new QLabel(QStringLiteral("Preview the change, then Apply to update the project."), owner);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        persistent_freedom = new QLabel(owner);
        persistent_freedom->setObjectName(QStringLiteral("constraintPersistentFreedom"));
        persistent_freedom->setAccessibleName(QStringLiteral("Degrees of freedom"));
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
        if (!boundary_mode || measured_mode)
            mode->addItem(measured_mode ? QStringLiteral("Change segment length") : curved_wall ? QStringLiteral("Change curve length") : QStringLiteral("Change wall length"), 0);
        mode->addItem(QStringLiteral("Add constraint"), 1);
        mode->addItem(QStringLiteral("Edit constraint"), 2);
        mode->addItem(QStringLiteral("Remove constraint"), 3);
        form->addRow(QStringLiteral("Operation"), mode);
        stroke_segment = new QComboBox(body);
        stroke_segment->setObjectName(QStringLiteral("constraintStrokeSegment"));
        if(measured_mode)for(const auto& edge:endpoint_edges(selected))
            stroke_segment->addItem(QStringLiteral("Segment %1").arg(stroke_segment->count()+1),text(edge.segment_id));
        form->addRow(QStringLiteral("Segment"),stroke_segment);
        existing = new QComboBox(body);
        existing->setObjectName(QStringLiteral("existingConstraint"));
        existing->setMinimumWidth(0);
        existing->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        existing->setMinimumContentsLength(12);
        prepareOwnerLabels();
        for (const auto& [id, entity] : snapshot.entities()) {
            if (!ownerParticipates(id)) continue;
            if (entity.type == "wall") {
                try {
                    if (!ConstraintDialog::supportsEntity(entity)) continue;
                    for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                        endpoints.push_back({id, role});
                        endpoint_labels.push_back(owner_label(id) + QStringLiteral(" · ") + text(wall_endpoint_role_name(role)));
                    }
                } catch (const std::exception&) { /* Invalid walls are not available as endpoints. */ }
            } else if ((entity.type=="measurement_linework" || can_recognize_boundary_entity_type(entity.type)) && ConstraintDialog::supportsEntity(entity)) {
                const auto edges = endpoint_edges(entity);
                for (std::size_t i = 0; i < edges.size(); ++i) {
                    const auto& edge = edges[i];
                    for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                        const auto& vertex = role == WallEndpointRole::start ? edge.start_vertex_id : edge.end_vertex_id;
                        endpoints.push_back({id, role, edge.segment_id, vertex});
                        endpoint_labels.push_back(owner_label(id) + QStringLiteral(" · Edge %1 · ").arg(i + 1) + text(wall_endpoint_role_name(role)));
                    }
                }
            } else if (entity.type == "constraint") {
                const auto decoded = decode_constraint_entity(entity);
                if (!decoded.constraint) continue;
                if (active_phase && !constraint_participates(*decoded.constraint, phase_scope)) continue;
                const auto& owners = decoded.constraint->bindings;
                if (std::any_of(owners.begin(), owners.end(), [&](const auto& b) { return b.owner_id == selected_id; })) {
                    existing->addItem(relation_label(decoded.constraint->relation) +
                        QStringLiteral(" · %1").arg(existing->count() + 1), text(id));
                    existing->setItemData(existing->count() - 1, text(id), Qt::ToolTipRole);
                }
            }
        }
        form->addRow(QStringLiteral("Existing constraint"), existing);
        relation = new QComboBox(body);
        relation->setObjectName(QStringLiteral("constraintRelation"));
        for (const auto kind : {ConstraintRelationKind::horizontal, ConstraintRelationKind::vertical,
            ConstraintRelationKind::coincident, ConstraintRelationKind::fixed_length,
            ConstraintRelationKind::parallel, ConstraintRelationKind::perpendicular, ConstraintRelationKind::fixed_anchor,
            ConstraintRelationKind::fixed_arc_length, ConstraintRelationKind::tangent})
            relation->addItem(relation_label(kind), static_cast<int>(kind));
        relation->setToolTip(QStringLiteral("Curve length measures along an arc. Endpoint distance measures straight between points. Tangent joins two segments smoothly at their chosen contact endpoints; choose contact then opposite endpoint for each segment."));
        form->addRow(QStringLiteral("Relationship"), relation);
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            bindings[index] = new QComboBox(body);
            bindings[index]->setObjectName(QStringLiteral("constraintBinding%1").arg(index));
            bindings[index]->addItems(endpoint_labels);
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                bindings[index]->setItemData(static_cast<int>(i), text(endpoints[i].owner_id), Qt::UserRole);
                bindings[index]->setItemData(static_cast<int>(i), endpoint_identity(endpoints[i]), Qt::ToolTipRole);
            }
            bindings[index]->setMinimumWidth(0);
            bindings[index]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            bindings[index]->setMinimumContentsLength(12);
            form->addRow(QStringLiteral("Endpoint %1").arg(index + 1), bindings[index]);
        }
        arc_chain_info = new QLabel(body);
        arc_chain_info->setObjectName(QStringLiteral("constraintArcChain"));
        arc_chain_info->setAccessibleName(QStringLiteral("Saved curve chain membership"));
        arc_chain_info->setTextFormat(Qt::PlainText);
        arc_chain_info->setWordWrap(true);
        form->addRow(QStringLiteral("Curve chain"), arc_chain_info);
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
        changes->setHorizontalHeaderLabels({QStringLiteral("Wall / measured segment / boundary edge"), QStringLiteral("Current length"),
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
        QObject::connect(stroke_segment,&QComboBox::currentIndexChanged,owner,[this] {if(!loading)configure(true);});
        QObject::connect(existing, &QComboBox::currentIndexChanged, owner, [this] { configure(true); });
        QObject::connect(relation, &QComboBox::currentIndexChanged, owner, [this] { if (!loading) configure(false); });
        for (auto* field : {length, anchor_x, anchor_y})
            QObject::connect(field, &QLineEdit::textChanged, owner, [this] { invalidate(); });
        for (std::size_t index = 0; index < bindings.size(); ++index)
            QObject::connect(bindings[index], &QComboBox::currentIndexChanged, owner, [this, index] {
                if (!loading && !editingArcChain() && index == 0 && relation->currentData().toInt() == static_cast<int>(ConstraintRelationKind::fixed_arc_length)) {
                    loading = true;
                    prefillCurveTarget();
                    loading = false;
                }
                invalidate();
            });
        QObject::connect(anchor, &QComboBox::currentIndexChanged, owner, [this] { invalidate(); });
        QObject::connect(connected, &QCheckBox::toggled, owner, [this] { invalidate(); });
        source_freedom = stored_component_analysis(snapshot, {selected_id});
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
                    names.push_back(relation_label(decoded.constraint->relation) + QStringLiteral(" constraint"));
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
                before = stored_component_analysis(snapshot, seeds);
                after = stored_component_analysis(candidate->candidate_entities(), seeds, active_phase);
                auto expanded = owners;
                expanded.insert(before.owner_ids.begin(), before.owner_ids.end());
                expanded.insert(after->owner_ids.begin(), after->owner_ids.end());
                if (expanded == owners) break;
                owners = std::move(expanded);
            }
        }
        QString value = QStringLiteral("Degrees of freedom: %1").arg(stored_freedom_value(before));
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
                details.push_back(phase + QStringLiteral(" stored component: ") + diagnostic_text(diagnostic));
        };
        append_details(before, snapshot.entities(), QStringLiteral("Current"));
        if (after) append_details(*after, candidate->candidate_entities(), QStringLiteral("Proposed"));
        persistent_freedom->setText(value);
        persistent_freedom->setToolTip(details.join('\n'));
    }

    void prepareOwnerLabels() {
        const auto type_label = [](const Entity& value) {
            return value.type == "wall" ? QStringLiteral("Wall") :
                value.type == "measurement_linework" ? QStringLiteral("Measured stroke") :
                value.type == "room_boundary" ? QStringLiteral("Room boundary") : QStringLiteral("Boundary");
        };
        const auto authored_name = [](const Entity& value) {
            const auto name = value.properties.find("name");
            return name != value.properties.end() && name->is_string()
                ? text(name->get_ref<const std::string&>()).trimmed() : QString{};
        };
        // Snapshot order makes unnamed labels stable throughout this dialog.
        // Count only unnamed supported owners of the same displayed type.
        std::map<QString, int> ordinals;
        for (const auto& [id, entity] : snapshot.entities()) {
            if (!ownerParticipates(id)) continue;
            if (!ConstraintDialog::supportsEntity(entity)) continue;
            const auto type = type_label(entity);
            const auto name = authored_name(entity);
            owner_labels.emplace(id, name.isEmpty()
                ? type + QStringLiteral(" %1").arg(++ordinals[type])
                : type + QStringLiteral(" · ") + name);
        }
    }

    QString owner_label(const std::string& id) const {
        const auto found = owner_labels.find(id);
        return found == owner_labels.end() ? text(id) : found->second;
    }

    bool ownerParticipates(const std::string& id) const {
        return !active_phase || !phase_scope.inactive_owner_ids.contains(id);
    }

    QString endpoint_identity(const WallEndpointBinding& binding) const {
        QStringList identity{text(binding.owner_id), text(wall_endpoint_role_name(binding.role))};
        if (!binding.segment_id.empty()) identity.push_back(QStringLiteral("Segment: ") + text(binding.segment_id));
        if (!binding.vertex_id.empty()) identity.push_back(QStringLiteral("Vertex: ") + text(binding.vertex_id));
        return identity.join(QStringLiteral(" · "));
    }

    void appendCurrentGeometry(std::vector<WallPreviewDrawing>& drawing, const std::string& id) const {
        if (!ownerParticipates(id)) return;
        const auto& entity = snapshot.entities().at(id);
        if (!ConstraintDialog::supportsEntity(entity)) return;
        if (entity.type == "wall") {
            const auto line = baseline(entity);
            drawing.push_back({owner_label(id), line, line});
        } else {
            const auto edges = endpoint_edges(entity);
            for (std::size_t i = 0; i < edges.size(); ++i) {
                const auto& edge = edges[i].segment;
                drawing.push_back({owner_label(id) + QStringLiteral(" · Edge %1").arg(i + 1), edge, edge});
            }
        }
    }

    void invalidate() {
        if (loading) return;
        preview.reset(); accepted.reset(); replacement_preview.reset(); replacement_intent.reset();
        accepted_command.reset(); unchanged_resize = false; apply_button->setEnabled(false);
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

    std::optional<Segment> bindingSegment(const WallEndpointBinding& binding) const {
        if (!ownerParticipates(binding.owner_id)) return std::nullopt;
        const auto found = snapshot.entities().find(binding.owner_id);
        if (found == snapshot.entities().end()) return std::nullopt;
        if (found->second.type == "wall") return baseline(found->second);
        for (const auto& edge : endpoint_edges(found->second))
            if (edge.segment_id == binding.segment_id) return edge.segment;
        return std::nullopt;
    }

    void selectAnchor(const WallEndpointBinding& binding) {
        for (std::size_t index = 0; index < endpoints.size(); ++index)
            if (endpoints[index] == binding) { anchor->setCurrentIndex(static_cast<int>(index)); return; }
    }

    void prefillCurveTarget() {
        const auto index = bindings[0]->currentIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= endpoints.size()) return;
        const auto first = endpoints[static_cast<std::size_t>(index)];
        const auto segment = bindingSegment(first);
        if (!segment) return;
        for (const auto& other : endpoints)
            if (other.owner_id == first.owner_id && other.segment_id == first.segment_id && other.role != first.role) {
                selectBinding(1, other);
                break;
            }
        selectAnchor(first);
        length->setText(editable_dimension(segment_length(*segment), metric));
    }

    void selectCurveDefaults() {
        const auto index = bindings[0]->currentIndex();
        const auto current = index >= 0 && static_cast<std::size_t>(index) < endpoints.size()
            ? std::optional<WallEndpointBinding>{endpoints[static_cast<std::size_t>(index)]} : std::nullopt;
        const auto segment = current ? bindingSegment(*current) : std::nullopt;
        if (!current || current->owner_id != selected_id || !segment || segment->sweep_radians == 0.0) {
            for (const auto& endpoint : endpoints) {
                if (endpoint.owner_id != selected_id || endpoint.role != WallEndpointRole::start) continue;
                const auto edge = bindingSegment(endpoint);
                if (edge && edge->sweep_radians != 0.0) { selectBinding(0, endpoint); break; }
            }
        }
        prefillCurveTarget();
    }

    void prefillEndpointDistance() {
        std::array<Vec2, 2> positions;
        for (std::size_t index = 0; index < positions.size(); ++index) {
            const auto selected = bindings[index]->currentIndex();
            if (selected < 0 || static_cast<std::size_t>(selected) >= endpoints.size()) return;
            const auto& endpoint = endpoints[static_cast<std::size_t>(selected)];
            const auto segment = bindingSegment(endpoint);
            if (!segment) return;
            positions[index] = endpoint.role == WallEndpointRole::start ? segment->start : segment->end;
        }
        length->setText(editable_dimension(std::hypot(positions[1].x - positions[0].x,
                                                   positions[1].y - positions[0].y), metric));
    }

    bool selectedArcChain() const {
        return selected_constraint && selected_constraint->relation == ConstraintRelationKind::fixed_arc_length &&
            selected_constraint->bindings.size() > 2;
    }

    bool editingArcChain() const {
        return mode->currentData().toInt() == 2 && selectedArcChain();
    }

    void configure(bool load_values) {
        loading = true;
        const auto operation = mode->currentData().toInt();
        const bool wall_resize = operation == 0;
        if (anchor->count() == 0 || anchor_for_wall_resize != wall_resize) {
            anchor->clear();
            if (wall_resize) {
                anchor->addItems(measured_mode ? QStringList{QStringLiteral("Keep segment start fixed"),QStringLiteral("Keep segment end fixed")} : QStringList{QStringLiteral("Keep selected wall start fixed"), QStringLiteral("Keep selected wall end fixed")});
            } else {
                for (std::size_t i = 0; i < endpoints.size(); ++i) {
                    anchor->addItem(QStringLiteral("Keep ") + endpoint_labels[static_cast<int>(i)] + QStringLiteral(" fixed"), static_cast<int>(i));
                    anchor->setItemData(static_cast<int>(i), endpoint_identity(endpoints[i]), Qt::ToolTipRole);
                }
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
                // The pair controls cannot represent a saved chain. Its full
                // ordered bindings remain in selected_constraint instead.
                if (!selectedArcChain())
                    for (std::size_t i = 0; i < std::min(c.bindings.size(), bindings.size()); ++i)
                        selectBinding(i, c.bindings[i]);
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
                    const auto edges = boundary_mode ? endpoint_edges(snapshot.entities().at(selected_id)) : std::vector<EndpointEdge>{};
                    const auto segment = boundary_mode ? edges.at(measured_mode ? static_cast<std::size_t>(std::max(0,stroke_segment->currentIndex())) : 0).segment : baseline(snapshot.entities().at(selected_id));
                    length->setText(editable_dimension(wall_resize ? segment_length(segment) :
                        std::hypot(segment.end.x - segment.start.x, segment.end.y - segment.start.y), metric));
                    anchor_x->setText(editable_dimension(segment.start.x, true));
                    anchor_y->setText(editable_dimension(segment.start.y, true));
                } catch (const std::exception&) { length->clear(); }
            }
        }
        const bool arc_chain = operation >= 2 && selectedArcChain();
        if (arc_chain) {
            relation->setCurrentIndex(relation->findData(static_cast<int>(ConstraintRelationKind::fixed_arc_length)));
            const auto endpoint_label = [&](const WallEndpointBinding& binding) {
                const auto found = std::find(endpoints.begin(), endpoints.end(), binding);
                return found != endpoints.end() ? endpoint_labels[static_cast<int>(found - endpoints.begin())]
                    : owner_label(binding.owner_id) + QStringLiteral(" · ") + text(wall_endpoint_role_name(binding.role));
            };
            QStringList members;
            const auto& chain = selected_constraint->bindings;
            for (std::size_t i = 0; i < chain.size(); i += 2)
                members.push_back(QStringLiteral("%1. %2 → %3").arg(static_cast<qulonglong>(i / 2 + 1))
                    .arg(endpoint_label(chain[i]), endpoint_label(chain[i + 1])));
            arc_chain_info->setText(QStringLiteral("Total length of %1 curved segments. Chain membership stays fixed during this edit.\n%2")
                .arg(static_cast<qulonglong>(chain.size() / 2)).arg(members.join('\n')));
        }
        const auto kind = static_cast<ConstraintRelationKind>(relation->currentData().toInt());
        const bool editing_relation = operation == 1 || operation == 2;
        if (editing_relation && kind == ConstraintRelationKind::fixed_arc_length &&
            (load_values || !configured_relation || *configured_relation != kind)) {
            if (load_values && selected_constraint && selected_constraint->relation == kind) {
                selectAnchor(selected_constraint->bindings.front());
            } else selectCurveDefaults();
        }
        if (editing_relation && !load_values && kind == ConstraintRelationKind::fixed_length &&
            configured_relation == ConstraintRelationKind::fixed_arc_length) prefillEndpointDistance();
        if (auto* label = qobject_cast<QLabel*>(form->labelForField(length)))
            label->setText(wall_resize ? (measured_mode ? QStringLiteral("Segment length") : curved_wall ? QStringLiteral("Curve length") : QStringLiteral("Wall length")) : kind == ConstraintRelationKind::fixed_arc_length
                ? (arc_chain ? QStringLiteral("Total curve length") : QStringLiteral("Curve length")) : QStringLiteral("Endpoint distance"));
        const auto count = kind == ConstraintRelationKind::fixed_anchor ? 1U :
            (kind == ConstraintRelationKind::parallel || kind == ConstraintRelationKind::perpendicular || kind == ConstraintRelationKind::tangent ? 4U : 2U);
        form->setRowVisible(stroke_segment,measured_mode && operation==0);
        form->setRowVisible(existing, operation >= 2);
        form->setRowVisible(relation, editing_relation);
        relation->setEnabled(!arc_chain);
        form->setRowVisible(arc_chain_info, arc_chain);
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            bindings[i]->setEnabled(!arc_chain);
            form->setRowVisible(bindings[i], editing_relation && !arc_chain && i < count);
            if (auto* label=qobject_cast<QLabel*>(form->labelForField(bindings[i])))
                label->setText(kind==ConstraintRelationKind::tangent
                    ? (i%2==0 ? QStringLiteral("Segment %1 contact").arg(i/2+1) : QStringLiteral("Segment %1 other end").arg(i/2+1))
                    : QStringLiteral("Endpoint %1").arg(i+1));
        }
        form->setRowVisible(length, operation == 0 || (editing_relation &&
            (kind == ConstraintRelationKind::fixed_length || kind == ConstraintRelationKind::fixed_arc_length)));
        form->setRowVisible(anchor, operation != 3);
        form->setRowVisible(connected, operation != 3);
        form->setRowVisible(anchor_x, editing_relation && kind == ConstraintRelationKind::fixed_anchor);
        form->setRowVisible(anchor_y, editing_relation && kind == ConstraintRelationKind::fixed_anchor);
        configured_relation = kind;
        loading = false;
        invalidate();
    }

    ConstraintAuthoringIntent intent() const {
        ConstraintAuthoringIntent result;
        const auto unit = metric ? Unit::metre : Unit::foot;
        const auto role = anchor->currentIndex() == 0 ? WallEndpointRole::start : WallEndpointRole::end;
        const auto operation = mode->currentData().toInt();
        if (operation == 0) {
            if(measured_mode) {
                BoundaryGeometryEdit edit;edit.boundary_id=selected_id;edit.kind=BoundaryGeometryEditKind::resize_segment;
                edit.target_id=stroke_segment->currentData().toString().toStdString();
                const auto quantity=parse_quantity(length->text().toStdString(),unit);edit.target_length_metres=quantity.metres;
                edit.fixed_endpoint=role==WallEndpointRole::start?BoundaryFixedEndpoint::start:BoundaryFixedEndpoint::end;
                result.measured_stroke_resize=MeasuredStrokeResizeIntent{edit,quantity,connected->isChecked()};
                result.message="change measured segment length with preview";
                return result;
            }
            result.wall_resize = WallResizeIntent{selected_id, parse_quantity(length->text().toStdString(), unit),
                role == WallEndpointRole::start ? WallResizeAnchor::start : WallResizeAnchor::end, connected->isChecked()};
            result.message = curved_wall ? "change curve length with preview" : "change wall length with preview";
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
            if (editingArcChain()) {
                // A chain edit changes its one exact physical total, never its
                // saved topology through the ordinary pair-only controls.
                auto value = *selected_constraint;
                value.length = parse_quantity(length->text().toStdString(), unit);
                (void)resolve_constraint_arc_segments(value, snapshot.entities());
                result.relation_mutations.push_back(ConstraintRelationMutation::upsert(std::move(value)));
                result.message = "edit total curve chain length";
                return result;
            }
            PersistentConstraint value;
            value.id = operation == 2 ? selected_constraint->id : new_constraint_id;
            value.relation = static_cast<ConstraintRelationKind>(relation->currentData().toInt());
            const auto count = value.relation == ConstraintRelationKind::fixed_anchor ? 1U :
                (value.relation == ConstraintRelationKind::parallel || value.relation == ConstraintRelationKind::perpendicular || value.relation == ConstraintRelationKind::tangent ? 4U : 2U);
            for (std::size_t i = 0; i < count; ++i) {
                const auto index = bindings[i]->currentIndex();
                if (index < 0 || static_cast<std::size_t>(index) >= endpoints.size())
                    throw std::invalid_argument("Select every endpoint required by the relationship");
                value.bindings.push_back(endpoints[static_cast<std::size_t>(index)]);
            }
            if (value.relation == ConstraintRelationKind::fixed_arc_length) {
                const auto& first = value.bindings[0];
                const auto& second = value.bindings[1];
                const auto segment = bindingSegment(first);
                if (first.owner_id != second.owner_id || first.segment_id != second.segment_id ||
                    first.role == second.role || !segment || segment->sweep_radians == 0.0)
                    throw std::invalid_argument("Choose both ends of the same curved wall, measured segment or boundary edge for Curve length");
            }
            if (value.relation == ConstraintRelationKind::fixed_length || value.relation == ConstraintRelationKind::fixed_arc_length)
                value.length = parse_quantity(length->text().toStdString(), unit);
            if (value.relation == ConstraintRelationKind::fixed_arc_length)
                (void)resolve_constraint_arc_segment(value, snapshot.entities());
            if (value.relation == ConstraintRelationKind::tangent)
                (void)resolve_constraint_tangent_segments(value,snapshot.entities());
            if (value.relation == ConstraintRelationKind::fixed_anchor)
                value.anchor = Vec2{parse_quantity(anchor_x->text().toStdString(), unit).metres,
                                    parse_quantity(anchor_y->text().toStdString(), unit).metres};
            result.relation_mutations.push_back(ConstraintRelationMutation::upsert(std::move(value)));
            result.message = operation == 1 ? "add geometry constraint" : "edit geometry constraint";
        }
        return result;
    }

    DocumentSnapshot currentSource() const {
        return current_source ? current_source() : snapshot;
    }

    void requireCurrentSource() const {
        const auto current = currentSource();
        if (!snapshot.is_editable() || !current.is_editable() ||
            document_snapshot_digest(current) != document_snapshot_digest(snapshot))
            throw std::invalid_argument("The project or edit authority changed. Reopen the constraint dialog before applying this edit.");
    }

    bool previewReplacement(const ConstraintAuthoringIntent& command, const PhaseWallReplacementRequest& request) {
        const auto plan = inspect_phase_wall_replacement_plan(snapshot.entities(), request.seed_wall_ids,
            request.registry_id, request.alternative_id, true);
        if (!plan.ready()) {
            QStringList reasons;
            for (const auto& diagnostic : plan.diagnostics)
                if (diagnostic.blocking) reasons.push_back(diagnostic_text(diagnostic.reason));
            throw std::invalid_argument(reasons.isEmpty() ? "The proposed wall replacement has unsupported dependencies."
                : reasons.join('\n').toStdString());
        }
        // Keep each allocation for the captured source throughout this dialog.
        // The command receives exactly the independently discovered inventory,
        // including typed child IDs, even if another preview needs fewer copies.
        PhaseWallReplacementAuthoring edit;
        edit.complete_presentations = true;
        edit.registry_id = request.registry_id;
        edit.alternative_id = request.alternative_id;
        edit.seed_wall_ids = request.seed_wall_ids;
        std::set<std::string, std::less<>> reserved;
        std::size_t reserved_nodes{}, reserved_bytes{};
        const auto reserve_text = [&](const std::string& value) {
            if (value.size() > 64 * 1024 * 1024 - reserved_bytes)
                throw std::invalid_argument("Wall replacement identity reservation exceeds its string budget.");
            reserved_bytes += value.size();
            reserved.insert(value);
        };
        const auto reserve = [&](const json& root) {
            std::vector<const json*> pending{&root};
            while (!pending.empty()) {
                const auto& value = *pending.back();
                pending.pop_back();
                if (++reserved_nodes > 4 * 1024 * 1024)
                    throw std::invalid_argument("Wall replacement identity reservation exceeds its JSON budget.");
                if (value.is_string()) reserve_text(value.get_ref<const std::string&>());
                else if (value.is_object()) for (const auto& [key, child] : value.items()) {
                    reserve_text(key);
                    pending.push_back(&child);
                } else if (value.is_array()) for (const auto& child : value) pending.push_back(&child);
            }
        };
        const auto reserve_entities = [&](const auto& entities) {
            for (const auto& [id, entity] : entities) {
                reserve_text(id);
                reserve_text(entity.type);
                reserve(entity.properties);
                reserve(entity.extensions);
            }
        };
        reserve_entities(snapshot.entities());
        for (const auto& revision : snapshot.history()) {
            reserve_entities(revision.entities);
            for (const auto& [id, asset] : revision.assets) { (void)asset; reserve_text(id); }
            if (revision.boundary_constraint_changes)
                reserve(command_to_json(Command{*revision.boundary_constraint_changes}));
        }
        for (const auto& [id, asset] : snapshot.assets()) { (void)asset; reserve_text(id); }
        for (const auto& [original, proposed] : replacement_identities) {
            (void)original;
            reserved.insert(proposed);
        }
        const auto allocate = [&](const std::string& original) {
            auto found = replacement_identities.find(original);
            if (found == replacement_identities.end()) {
                auto fresh = make_stable_id();
                while (!reserved.insert(fresh).second) fresh = make_stable_id();
                found = replacement_identities.emplace(original, std::move(fresh)).first;
            }
            edit.identities.emplace(original, found->second);
        };
        for (const auto& id : plan.required_entity_ids) allocate(id);
        for (const auto& id : plan.required_child_ids) allocate(id);
        auto phase_intent = make_phase_constraint_authoring_intent(snapshot, command);
        phase_intent.wall_replacement = encode_phase_wall_replacement_authoring(edit);
        auto candidate = inspect_phase_wall_replacement_authoring(snapshot, phase_intent);
        const auto candidate_scope = constraint_phase_scope(candidate.edited_entities);
        std::map<std::string, std::string, std::less<>> originals;
        for (const auto& [original, proposed] : candidate.replacement.original_to_proposed)
            originals.emplace(proposed, original);
        std::vector<WallPreviewDrawing> drawing;
        std::vector<std::string> proposed_walls;
        QStringList summary{QStringLiteral("This edit creates proposed objects in the saved active alternative. Original baseline geometry is preserved.")};
        changes->setRowCount(0);
        const auto add_segment = [&](const QString& label, const std::string& proposed_id,
            const Segment& before, const Segment& after) {
            drawing.push_back({label, before, after});
            const auto displacement = std::max(std::hypot(before.start.x - after.start.x, before.start.y - after.start.y),
                std::hypot(before.end.x - after.end.x, before.end.y - after.end.y));
            const int row = changes->rowCount();
            changes->insertRow(row);
            const QStringList values{label, dimension(segment_length(before), metric),
                dimension(segment_length(after), metric), dimension(displacement, metric)};
            for (int column = 0; column < values.size(); ++column) {
                auto* item = new QTableWidgetItem(values[column]);
                item->setToolTip(text(proposed_id));
                changes->setItem(row, column, item);
            }
            if (summary.size() < 4)
                summary.push_back(QStringLiteral("%1: %2 → %3; endpoint movement up to %4")
                    .arg(values[0], values[1], values[2], values[3]));
        };
        const auto same_segment = [](const Segment& first, const Segment& second) {
            return first.start.x == second.start.x && first.start.y == second.start.y &&
                first.end.x == second.end.x && first.end.y == second.end.y && first.sweep_radians == second.sweep_radians;
        };
        for (const auto& [id, entity] : candidate.edited_entities) {
            if (candidate_scope.inactive_owner_ids.contains(id) || !ConstraintDialog::supportsEntity(entity)) continue;
            const auto mapping = originals.find(id);
            const bool copied = mapping != originals.end();
            const auto original = copied ? mapping->second : id;
            const auto source = snapshot.entities().find(original);
            if (source == snapshot.entities().end())
                throw std::invalid_argument("Proposed geometry has no original object for comparison.");
            const auto label = owner_label(original) + (copied ? QStringLiteral(" · Proposed") : QString{});
            if (entity.type == "wall") {
                const auto before = baseline(source->second), after = baseline(entity);
                if (!copied && original != selected_id && same_segment(before, after)) continue;
                proposed_walls.push_back(id);
                add_segment(label, id, before, after);
            } else {
                const auto before = endpoint_edges(source->second), after = endpoint_edges(entity);
                for (std::size_t index = 0; index < before.size(); ++index) {
                    const auto& edge = before[index];
                    const auto target = candidate.replacement.original_to_proposed.find(edge.segment_id);
                    const auto proposed_segment = target == candidate.replacement.original_to_proposed.end()
                        ? edge.segment_id : target->second;
                    const auto changed = std::find_if(after.begin(), after.end(), [&](const auto& value) {
                        return value.segment_id == proposed_segment;
                    });
                    if (changed == after.end())
                        throw std::invalid_argument("Proposed geometry changed an edge identity outside the reviewed replacement.");
                    if (!copied && original != selected_id && same_segment(edge.segment, changed->segment)) continue;
                    add_segment(label + QStringLiteral(" · %1 %2").arg(entity.type == "measurement_linework"
                        ? QStringLiteral("Segment") : QStringLiteral("Edge")).arg(index + 1), id, edge.segment, changed->segment);
                }
            }
        }
        validate_solids(candidate.edited_entities, proposed_walls);
        // Deferred room relations deliberately did not constrain the physical
        // stage. Report that same stage's freedom, without implying completed
        // room constraints or substituting a fabricated source Snapshot.
        auto physical_entities = candidate.edited_entities;
        for (const auto& [id, entity] : candidate.replacement.deferred_room_constraints) physical_entities.erase(id);
        std::vector<std::string> seeds;
        for (const auto& id : request.seed_wall_ids) seeds.push_back(candidate.replacement.original_to_proposed.at(id));
        const auto freedom = stored_component_analysis(physical_entities, seeds, true);
        persistent_freedom->setText(QStringLiteral("Degrees of freedom: current %1 · proposed physical stage %2")
            .arg(stored_freedom_value(source_freedom), stored_freedom_value(freedom)));
        QStringList freedom_details{QStringLiteral("The proposed value counts stored constraints on the actual proposed physical geometry. Room relationships awaiting explicit review are excluded. Temporary edit anchors and pins are excluded; room review can change the final value.")};
        for (const auto& diagnostic : freedom.diagnostics) freedom_details.push_back(diagnostic_text(diagnostic));
        persistent_freedom->setToolTip(freedom_details.join('\n'));
        if (candidate.needs_room_review)
            summary.push_back(QStringLiteral("Apply opens the required proposed-room and relationship review. Cancel leaves this edit unapplied."));
        else
            summary.push_back(QStringLiteral("Apply records one undoable proposed wall edit."));
        requireCurrentSource();
        canvas->setWalls(std::move(drawing));
        replacement_intent = std::move(phase_intent);
        replacement_preview = std::move(candidate);
        apply_button->setEnabled(true);
        status->setPlainText(summary.join('\n'));
        return true;
    }

    bool previewEdit() {
        invalidate();
        try {
            requireCurrentSource();
            const auto command = intent();
            const auto replacements = phase_scope.registries.empty() ? std::vector<PhaseWallReplacementRequest>{}
                : phase_wall_replacement_requests(snapshot.entities(), command);
            if (replacements.size() > 1)
                throw std::invalid_argument("This edit requires wall replacements in multiple phase registries. Edit one design registry at a time.");
            if (!replacements.empty()) return previewReplacement(command, replacements.front());
            // A validated unchanged resize closes the dialog without inventing
            // an accepted service receipt or recording an empty history event.
            if (command.measured_stroke_resize) {
                const auto decoded = decode_measurement_linework_model(snapshot.entities().at(selected_id).properties.at("model"));
                if (!decoded.supported()) throw std::invalid_argument(decoded.diagnostic);
                const auto& resize = *command.measured_stroke_resize;
                const auto edited = edited_measurement_linework(*decoded.model, resize.edit, resize.exact_length);
                if (encode_measurement_linework_model(edited) == encode_measurement_linework_model(*decoded.model)) {
                    unchanged_resize = true;
                    apply_button->setEnabled(true);
                    status->setPlainText(QStringLiteral("Length is unchanged. Apply closes this dialog without adding an undo step."));
                    return true;
                }
            }
            auto candidate = preview_constraint_authoring(snapshot, command);
            if (!candidate.accepted()) {
                QStringList reasons;
                for (const auto& diagnostic : candidate.diagnostics()) reasons.push_back(diagnostic_text(diagnostic));
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
                const auto display_name = owner_label(wall.wall_id);
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
                    for (int column = 0; column < values.size(); ++column) {
                        auto* item = new QTableWidgetItem(values[column]);
                        item->setToolTip(text(boundary.before.id));
                        changes->setItem(row, column, item);
                    }
                    ++row;
                }
                summary.push_back(QStringLiteral("%1: boundary movement shown in preview.").arg(owner_label(boundary.before.id)));
            }
            for(const auto& stroke:candidate.changed_measured_strokes()) {
                drawn_owners.insert(stroke.stroke_id);
                for(std::size_t index=0;index<stroke.before.edges.size();++index) {
                    const auto& before=stroke.before.edges[index].segment;const auto& after=stroke.after.edges[index].segment;
                    const auto label=owner_label(stroke.stroke_id)+QStringLiteral(" · Segment %1").arg(index+1);
                    drawing.push_back({label,before,after});changes->insertRow(row);
                    const auto displacement=std::max(std::hypot(before.start.x-after.start.x,before.start.y-after.start.y),std::hypot(before.end.x-after.end.x,before.end.y-after.end.y));
                    const QStringList values{label,dimension(segment_length(before),metric),dimension(segment_length(after),metric),dimension(displacement,metric)};
                    for(int column=0;column<values.size();++column) {
                        auto* item = new QTableWidgetItem(values[column]);
                        item->setToolTip(text(stroke.stroke_id));
                        changes->setItem(row,column,item);
                    }
                    ++row;
                }
                summary.push_back(QStringLiteral("%1: measured segment movement shown in preview.").arg(owner_label(stroke.stroke_id)));
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
                summary.push_back(QStringLiteral("Edit preview: ") + diagnostic_text(diagnostic));
            summary.push_back(QStringLiteral("Apply records one undoable command."));
            status->setPlainText(summary.join('\n'));
            return true;
        } catch (const std::exception& exception) {
            error = diagnostic_text(exception.what());
            status->setStyleSheet(status->palette().color(QPalette::Window).lightness() < 128
                ? QStringLiteral("color:#ffb4a2;") : QStringLiteral("color:#9f1b11;"));
            status->setPlainText(QStringLiteral("Edit preview: ") +
                (error.isEmpty() ? QStringLiteral("The requested constraints could not be satisfied.") : error));
            return false;
        }
    }

    bool submit() {
        if (replacement_preview && replacement_intent) {
            try {
                requireCurrentSource();
                std::optional<ApplyBoundaryConstraintChanges> command;
                if (replacement_preview->needs_room_review) {
                    PhysicalWallPhaseRoomReviewDialog review(snapshot, *replacement_intent, metric,
                        [this] { return currentSource(); }, owner);
                    if (review.exec() != QDialog::Accepted) {
                        accepted_command.reset();
                        error = review.lastError();
                        status->setPlainText(error.isEmpty()
                            ? QStringLiteral("Room review was canceled. The wall edit has not been applied.") : error);
                        return false;
                    }
                    command = review.acceptedCommand();
                    if (!command) throw std::invalid_argument("Complete the required room review before applying the wall edit.");
                } else {
                    command = phase_wall_replacement_authoring_command(*replacement_intent);
                }
                requireCurrentSource();
                // Validate the complete command against the actual capture,
                // including retained history and exact fresh-ID reservations.
                // The live Document independently repeats this admission.
                (void)Document::preview_command(snapshot, *command);
                requireCurrentSource();
                accepted_command = std::move(command);
                accepted.reset();
                error.clear();
                owner->accept();
                return true;
            } catch (const std::exception& exception) {
                accepted_command.reset();
                error = diagnostic_text(exception.what());
                status->setPlainText(QStringLiteral("Apply: ") + error);
                return false;
            }
        }
        try {
            requireCurrentSource();
        } catch (const std::exception& exception) {
            accepted.reset();
            error = diagnostic_text(exception.what());
            status->setPlainText(QStringLiteral("Apply: ") + error);
            return false;
        }
        if (unchanged_resize) {
            owner->accept();
            return true;
        }
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
    bool active_phase{};
    ConstraintPhaseScope phase_scope;
    bool boundary_mode{};
    bool measured_mode{};
    bool curved_wall{};
    bool loading{};
    bool unchanged_resize{};
    bool anchor_for_wall_resize{};
    std::string new_constraint_id = make_stable_id();
    std::vector<WallEndpointBinding> endpoints;
    QStringList endpoint_labels;
    std::optional<PersistentConstraint> selected_constraint;
    std::optional<ConstraintRelationKind> configured_relation;
    std::optional<ConstraintAuthoringPreview> preview, accepted;
    std::function<DocumentSnapshot()> current_source;
    PhaseWallReplacementIdentityMap replacement_identities;
    std::optional<PhaseWallReplacementAuthoringPreview> replacement_preview;
    std::optional<PhaseConstraintAuthoringIntent> replacement_intent;
    std::optional<ApplyBoundaryConstraintChanges> accepted_command;
    std::map<std::string, QString, std::less<>> owner_labels;
    PersistentConstraintComponentAnalysis source_freedom;
    QString error;
    QFormLayout* form{};
    QComboBox *mode{}, *existing{}, *relation{}, *anchor{}, *stroke_segment{};
    std::array<QComboBox*, 4> bindings{};
    QLineEdit *length{}, *anchor_x{}, *anchor_y{};
    QCheckBox* connected{};
    ConstraintPreviewCanvas* canvas{};
    QTableWidget* changes{};
    QPlainTextEdit* status{};
    QLabel *persistent_freedom{}, *arc_chain_info{};
    QPushButton *apply_button{}, *preview_button{};
};

bool ConstraintDialog::supportsEntity(const Entity& entity) noexcept {
    try {
        if (entity.type == "wall") return segment_length(baseline(entity)) > default_geometry_tolerance_metres;
        if (entity.type == "measurement_linework") return !endpoint_edges(entity).empty();
        if (!can_recognize_boundary_entity_type(entity.type)) return false;
        const auto boundary = decode_identified_boundary_entity(entity);
        return !boundary.segments.empty();
    } catch (...) { return false; }
}

ConstraintDialog::ConstraintDialog(DocumentSnapshot snapshot, QString selected_entity_id,
                                   bool metric_units, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, std::move(snapshot), std::move(selected_entity_id), metric_units)) {
    if (parent && parent->testAttribute(Qt::WA_DontShowOnScreen)) setAttribute(Qt::WA_DontShowOnScreen);
}
ConstraintDialog::~ConstraintDialog() = default;
void ConstraintDialog::setLengthExpression(const QString& expression) { m_impl->length->setText(expression); }
void ConstraintDialog::setCurrentSource(std::function<DocumentSnapshot()> current_source) {
    m_impl->current_source = std::move(current_source);
    m_impl->invalidate();
}
bool ConstraintDialog::previewEdit() { return m_impl->previewEdit(); }
bool ConstraintDialog::submit() { return m_impl->submit(); }
std::optional<ConstraintAuthoringPreview> ConstraintDialog::acceptedPreview() const { return m_impl->accepted; }
std::optional<ApplyBoundaryConstraintChanges> ConstraintDialog::acceptedCommand() const { return m_impl->accepted_command; }
QString ConstraintDialog::lastError() const { return m_impl->error; }

} // namespace sketch::desktop

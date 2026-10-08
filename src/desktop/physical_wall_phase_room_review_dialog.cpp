#include "sketch/desktop/physical_wall_phase_room_review_dialog.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/physical_wall_phase_review.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/document_wall_plan.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "plan_canvas.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sketch::desktop {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
std::string property_text(const Entity& entity,const char* key) {
    const auto found=entity.properties.find(key);
    return found!=entity.properties.end() && found->is_string()?found->get<std::string>():std::string{};
}
bool exact_entities(const std::map<std::string,Entity,std::less<>>& first,
    const std::map<std::string,Entity,std::less<>>& second) {
    if (first!=second) return false;
    for (const auto& [id,entity]:first) {
        const auto& other=second.at(id);
        if (entity.properties.dump()!=other.properties.dump() || entity.extensions.dump()!=other.extensions.dump()) return false;
    }
    return true;
}
QString area(double value,bool metric) {
    return QStringLiteral("%1 %2").arg(metric?value:value/0.09290304,0,'f',2)
        .arg(metric?QStringLiteral("m²"):QStringLiteral("sq ft"));
}
QString quantity(const BoundaryDimensionResolution& value,bool metric) {
    if (value.kind==BoundaryDimensionKind::area) return area(value.area_square_metres,metric);
    if (value.kind==BoundaryDimensionKind::angle)
        return QStringLiteral("%1°").arg(value.angle_radians*180/std::numbers::pi,0,'f',1);
    return PlanCanvas::drawingLengthText(value.segment_length_metres,metric);
}
QString relation_name(RoomRelationKind value) {
    if (value==RoomRelationKind::independent) return QStringLiteral("independent");
    if (value==RoomRelationKind::follows) return QStringLiteral("follows");
    return QStringLiteral("derived from");
}
using Child=std::tuple<std::string,bool,std::string>; // owner, vertex, original child ID
QString relationship(PhysicalWallRoomCorrespondenceKind value) {
    switch (value) {
    case PhysicalWallRoomCorrespondenceKind::unique_continuation:return QStringLiteral("Continuation");
    case PhysicalWallRoomCorrespondenceKind::split:return QStringLiteral("Split");
    case PhysicalWallRoomCorrespondenceKind::merge:return QStringLiteral("Merge");
    case PhysicalWallRoomCorrespondenceKind::new_space:return QStringLiteral("New space");
    case PhysicalWallRoomCorrespondenceKind::retired:return QStringLiteral("No current space");
    default:return QStringLiteral("Needs review");
    }
}
QTableWidget* table(QWidget* parent,const QString& name,const QStringList& headers) {
    auto* result=new QTableWidget(0,headers.size(),parent);result->setObjectName(name);
    result->setHorizontalHeaderLabels(headers);result->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result->verticalHeader()->hide();result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->setSelectionBehavior(QAbstractItemView::SelectRows);return result;
}
QComboBox* choice(QWidget* parent,const QString& name) {
    auto* result=new QComboBox(parent);result->setObjectName(name);
    result->addItem(QStringLiteral("Choose…"),QString{});return result;
}
std::string value(const QComboBox* combo) { return combo->currentData().toString().toStdString(); }
void option(QComboBox* combo,const QString& label,const QString& data,bool enabled,const QString& reason={}) {
    combo->addItem(label,data);
    if (auto* model=qobject_cast<QStandardItemModel*>(combo->model())) {
        if (auto* item=model->item(combo->count()-1)) {item->setEnabled(enabled);item->setToolTip(reason);}
    }
}
PhysicalWallSpace space(const FreshPhysicalWallRoomCorrespondence& candidate) {
    return {candidate.baseline_face_index,candidate.boundary,candidate.holes,candidate.area_square_metres,candidate.source_lineage};
}
bool interior_point(const FreshPhysicalWallRoomCorrespondence& candidate,Vec2 point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !PlanCanvas::containsAreaPoint(candidate.boundary,point)) return false;
    // Use the same analytical strict-containment admission as the core review,
    // including separation from every hole edge, rather than screen hit testing.
    constexpr double radius=8*default_geometry_tolerance_metres;
    auto holes=candidate.holes;
    holes.push_back({{{point.x-radius,point.y-radius},{point.x+radius,point.y-radius},0},
        {{point.x+radius,point.y-radius},{point.x,point.y+radius},0},
        {{point.x,point.y+radius},{point.x-radius,point.y-radius},0}});
    return !validate_boundary_holes(candidate.boundary,holes).has_value();
}
} // namespace

class PhysicalWallPhaseRoomReviewDialog::Impl {
public:
    struct FreshRow {
        QComboBox* assignment{};QLineEdit* name{};QLineEdit* classification{};QLineEdit* factor{};QPushButton* pick{};
        std::string allocated_room_id;LegacyBoundaryIdentityOptions ids;std::vector<std::string> dimension_ids;std::optional<Vec2> point;
    };
    struct PlaneRows {
        QTableWidget* previous_table{};QTableWidget* fresh_table{};
        std::map<std::string,QComboBox*,std::less<>> previous;
        std::vector<FreshRow> fresh;
        std::map<std::string,std::set<std::size_t>,std::less<>> matching;
    };
    struct ReferenceRow {
        PhysicalWallRoomPhaseBaselineAcknowledgement evidence;QCheckBox* preserve{};
    };
    struct ProposedReferenceRow {
        std::string id;std::set<std::string> owners;std::set<Child> children;
        bool dimension{};bool automatic_lengths{};QComboBox* decision{};
    };
    struct GraphRows {
        PhysicalWallRoomRelationshipRemoval evidence;
        std::vector<QCheckBox*> memberships,relations;
    };
    struct PresentationRow {
        PhysicalWallRoomPhasePresentationRemoval evidence;QCheckBox* acknowledge{};
    };
    struct RoomEndpointRow {
        std::size_t binding_index{};WallEndpointBinding original;
        QComboBox* target{};std::vector<WallEndpointBinding> targets;
        std::vector<std::string> target_evidence;
    };
    struct RoomConstraintRow {
        std::string original_id;std::string fresh_id;QComboBox* decision{};
        std::vector<RoomEndpointRow> endpoints;bool keep_available{};bool removal_requested{};
    };
    PhysicalWallPhaseRoomReviewDialog* dialog;
    DocumentSnapshot source;
    ApplyEntityChanges registry_command;
    std::optional<PhaseConstraintAuthoringIntent> replacement_intent;
    std::optional<PhaseWallReplacementAuthoringPreview> replacement_preview;
    std::set<std::string,std::less<>> deferred_constraint_ids;
    std::vector<RoomConstraintRow> room_constraints;
    PhysicalWallPhaseSelection destination;
    std::function<DocumentSnapshot()> current_source;
    std::string source_digest;
    bool metric{};bool rebuilding{};bool invalidated{};
    std::optional<PhysicalWallRoomPhaseReviewInventory> inventory;
    std::vector<PlaneRows> planes;
    std::set<std::string> baseline_rooms,active_rooms,proposed_rooms;
    std::map<std::string,QString,std::less<>> token_labels;
    std::vector<ReferenceRow> references;
    std::vector<std::string> reference_superseded;
    std::vector<ProposedReferenceRow> proposed_references;
    std::vector<GraphRows> graphs;
    std::vector<PresentationRow> presentation_rows;
    std::vector<std::string> presentation_removed;
    std::map<Child,QComboBox*> mappings;
    std::vector<std::string> reference_changed,graph_retiring;
    QComboBox* plane_choice{};PlanCanvas* canvas{};QStackedWidget* pages{};QTableWidget* reference_table{};
    QTableWidget* proposed_reference_table{};QTableWidget* mapping_table{};QTableWidget* graph_table{};
    QTableWidget* presentation_table{};
    QTableWidget* room_constraint_table{};
    QTabWidget* tabs{};QLabel* status{};QLabel* plane_status{};QPushButton* apply{};
    std::optional<std::pair<std::size_t,std::size_t>> active_pick;
    std::optional<ApplyBoundaryConstraintChanges> candidate,accepted;
    std::optional<DocumentSnapshot> candidate_snapshot;
    QString error;

    Impl(PhysicalWallPhaseRoomReviewDialog* owner,DocumentSnapshot captured,ApplyEntityChanges command,
        PhysicalWallPhaseSelection target,bool metric_units,std::function<DocumentSnapshot()> current,
        std::optional<PhaseConstraintAuthoringIntent> replacement=std::nullopt):
        dialog(owner),source(std::move(captured)),registry_command(std::move(command)),replacement_intent(std::move(replacement)),
        destination(std::move(target)),
        current_source(std::move(current)),source_digest(document_snapshot_digest(source)),metric(metric_units) {
        dialog->setObjectName(QStringLiteral("physicalPhaseRoomReviewDialog"));
        dialog->setWindowTitle(destination.alternative_id?QStringLiteral("Review rooms in the alternative"):QStringLiteral("Review baseline rooms"));dialog->resize(1180,900);
        auto* layout=new QVBoxLayout(dialog);
        auto* help=new QLabel(QStringLiteral("Review every previous room and current clear space on each listed floor. "
            "Unchanged rooms can be shared. Changed baseline rooms stay preserved while you choose rooms for this alternative. "
            "Redefining a proposed room keeps its identity and authored facts; retiring it removes that proposal. "
            "Enter new room facts explicitly; attached references require separate confirmation."),dialog);
        help->setWordWrap(true);layout->addWidget(help);
        auto* limitation=new QLabel(QStringLiteral("References are preserved in the baseline; they are not copied into this alternative. "
            "For changed proposed rooms, choose Keep or Remove for each supported dimension and constraint, map kept children, "
            "and acknowledge each removed relationship row and saved presentation change. Unsupported incoming references prevent Apply."),dialog);
        limitation->setWordWrap(true);limitation->setObjectName(QStringLiteral("physicalPhaseRoomReviewLimitations"));layout->addWidget(limitation);
        plane_choice=new QComboBox(dialog);plane_choice->setObjectName(QStringLiteral("physicalPhaseRoomReviewPlane"));layout->addWidget(plane_choice);
        plane_status=new QLabel(dialog);plane_status->setWordWrap(true);plane_status->setTextFormat(Qt::PlainText);layout->addWidget(plane_status);
        canvas=new PlanCanvas(dialog);canvas->setObjectName(QStringLiteral("physicalPhaseRoomReviewCanvas"));canvas->setMinimumHeight(270);
        canvas->setGridEnabled(false);canvas->setSnapEnabled(false);canvas->setOverviewMapEnabled(false);
        canvas->setSelectionTransformEnabled(false,false);
        canvas->setPointPlacementRequested([this](Vec2 point){if (active_pick) pick(point);});layout->addWidget(canvas,1);
        auto* legend=new QLabel(QStringLiteral("Blue: current clear spaces · Dashed gray: previous room outlines · Green: prepared room preview · ●: chosen interior point"),dialog);
        legend->setWordWrap(true);layout->addWidget(legend);
        tabs=new QTabWidget(dialog);tabs->setObjectName(QStringLiteral("physicalPhaseRoomReviewTabs"));
        pages=new QStackedWidget(tabs);tabs->addTab(pages,QStringLiteral("Rooms"));
        auto* reference_page=new QWidget(tabs);auto* reference_layout=new QVBoxLayout(reference_page);
        auto* reference_help=new QLabel(QStringLiteral("Confirm each listed reference remains with its original baseline room. "
            "Saved dimensions, relationships and other attached data are included. No reference is copied or reassigned."),reference_page);
        reference_help->setWordWrap(true);reference_layout->addWidget(reference_help);
        reference_table=table(reference_page,QStringLiteral("physicalPhaseRoomReviewReferences"),
            {QStringLiteral("Attached reference"),QStringLiteral("Touches previous room / edge / corner"),QStringLiteral("Confirmation")});
        reference_layout->addWidget(reference_table);tabs->addTab(reference_page,QStringLiteral("Baseline references"));
        auto* proposed_page=new QWidget(tabs);auto* proposed_layout=new QVBoxLayout(proposed_page);
        auto* proposed_help=new QLabel(QStringLiteral("Choose Keep or Remove for every reference touching a changed proposed room. "
            "Retirement requires removal. Kept edges and corners need explicit mappings to the chosen current space. "
            "Keeping automatic lengths replaces them with fresh dimensions on every new edge."),proposed_page);
        proposed_help->setWordWrap(true);proposed_layout->addWidget(proposed_help);
        proposed_reference_table=table(proposed_page,QStringLiteral("physicalPhaseRoomReviewProposedReferences"),
            {QStringLiteral("Attached reference"),QStringLiteral("Decision"),QStringLiteral("Prepared preview"),QStringLiteral("Replacement dimension identities")});
        mapping_table=table(proposed_page,QStringLiteral("physicalPhaseRoomReviewMappings"),
            {QStringLiteral("Previous edge / corner"),QStringLiteral("Chosen current edge / corner")});
        proposed_layout->addWidget(proposed_reference_table);proposed_layout->addWidget(mapping_table);
        tabs->addTab(proposed_page,QStringLiteral("Proposed references"));
        graph_table=table(tabs,QStringLiteral("physicalPhaseRoomReviewRelationships"),
            {QStringLiteral("Relationship model / exact row"),QStringLiteral("Removal acknowledgement")});
        tabs->addTab(graph_table,QStringLiteral("Proposed relationships"));
        auto* presentation_page=new QWidget(tabs);auto* presentation_layout=new QVBoxLayout(presentation_page);
        auto* presentation_help=new QLabel(QStringLiteral("Review every saved presentation or annotation record affected by retiring a proposed room, "
            "removing a reference, or replacing automatic dimensions. Confirm each listed change separately. "
            "The listed items are removed from object restrictions, overlays, appearance rows and annotation overrides on Apply."),presentation_page);
        presentation_help->setWordWrap(true);presentation_layout->addWidget(presentation_help);
        presentation_table=table(presentation_page,QStringLiteral("physicalPhaseRoomReviewPresentationRemovals"),
            {QStringLiteral("Saved presentation / annotation record"),QStringLiteral("Affected items"),QStringLiteral("Change acknowledgement")});
        presentation_layout->addWidget(presentation_table);tabs->addTab(presentation_page,QStringLiteral("Presentation changes"));layout->addWidget(tabs,2);
        if (replacement_intent) {
            dialog->setWindowTitle(QStringLiteral("Review rooms for the proposed wall edit"));
            limitation->setText(QStringLiteral("Review copied room constraints individually. Keep requires unchanged active original rooms. "
                "Remap every room endpoint to a reviewed active room edge and corner, or explicitly omit the new copy. "
                "The original baseline constraint remains preserved."));
            reference_help->setText(QStringLiteral("Confirm each listed original reference remains preserved in the baseline. "
                "For a copied constraint, this confirmation preserves its captured evidence during room review; "
                "choose its final Keep, Remap or Omit disposition in Copied room constraints."));
            room_constraint_table=table(tabs,QStringLiteral("physicalPhaseWallCopiedRoomConstraints"),
                {QStringLiteral("Copied room constraint"),QStringLiteral("Decision"),QStringLiteral("Original room endpoint"),QStringLiteral("Reviewed endpoint")});
            tabs->addTab(room_constraint_table,QStringLiteral("Copied room constraints"));
        }
        status=new QLabel(dialog);status->setObjectName(QStringLiteral("physicalPhaseRoomReviewStatus"));
        status->setWordWrap(true);status->setTextFormat(Qt::PlainText);layout->addWidget(status);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,dialog);
        apply=buttons->button(QDialogButtonBox::Apply);apply->setObjectName(QStringLiteral("physicalPhaseRoomReviewApply"));
        apply->setText(destination.alternative_id?QStringLiteral("Apply alternative and reviewed rooms"):QStringLiteral("Apply baseline selection"));
        if (replacement_intent) apply->setText(QStringLiteral("Apply wall edit and reviewed rooms"));
        apply->setEnabled(false);layout->addWidget(buttons);
        QObject::connect(apply,&QPushButton::clicked,dialog,[this]{dialog->accept();});
        QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,[this]{dialog->reject();});
        QObject::connect(plane_choice,&QComboBox::currentIndexChanged,dialog,[this](int index){
            if (rebuilding) return;
            active_pick.reset();pages->setCurrentIndex(index);scene();canvas->fitView();
        });
        auto* timer=new QTimer(dialog);timer->setInterval(100);
        QObject::connect(timer,&QTimer::timeout,dialog,[this]{
            if (dialog->isVisible() && !is_current()) invalidate();
        });timer->start();
        initialize();
    }
    bool is_current() const {
        try {
            if (invalidated || !source.is_editable() || !current_source) return false;
            const auto current=current_source();return current.is_editable() && document_snapshot_digest(current)==source_digest;
        }
        catch (...) {return false;}
    }
    void require_current() const {
        if (!is_current()) throw std::invalid_argument("The project or editing permissions changed. Cancel and start a new review.");
    }
    void fail(const QString& reason) {
        if (!invalidated && !is_current()) {invalidate();return;}
        candidate.reset();candidate_snapshot.reset();accepted.reset();apply->setEnabled(false);status->setText(reason);error=reason;
        for (int row=0;row<proposed_reference_table->rowCount();++row) proposed_reference_table->item(row,2)->setText({});
    }
    void invalidate() {
        invalidated=true;active_pick.reset();canvas->setEnabled(false);canvas->setEntities({});canvas->setLabels({});
        plane_choice->setEnabled(false);pages->setEnabled(false);reference_table->setEnabled(false);
        proposed_reference_table->setEnabled(false);mapping_table->setEnabled(false);graph_table->setEnabled(false);
        presentation_table->setEnabled(false);
        if (room_constraint_table) room_constraint_table->setEnabled(false);
        fail(QStringLiteral("The project or editing permissions changed. Cancel and start a new review."));
    }
    QString entity_name(const std::string& id) const {
        const auto& entities=analytical_entities();const auto found=entities.find(id);
        if (found==entities.end()) return text(id);
        const auto name=text(property_text(found->second,"name"));
        return name.isEmpty()?text(id):name;
    }
    const PhaseWallReplacementEntities& analytical_entities() const {
        return replacement_preview?replacement_preview->edited_entities:source.entities();
    }
    QString endpoint_label(const WallEndpointBinding& binding,const PhaseWallReplacementEntities& entities) const {
        const auto& room=entities.at(binding.owner_id);const auto boundary=decode_identified_boundary_entity(room);
        auto name=text(property_text(room,"name"));if (name.isEmpty()) name=QStringLiteral("Unnamed room");
        const auto floor=entities.find(property_text(room,"floor_id"));
        if (floor!=entities.end()) {
            const auto floor_name=text(property_text(floor->second,"name"));
            if (!floor_name.isEmpty()) name+=QStringLiteral(" / ")+floor_name;
        }
        for (std::size_t edge=0;edge<boundary.segments.size();++edge) {
            const auto& segment=boundary.segments[edge];if (segment.segment_id!=binding.segment_id) continue;
            if (binding.vertex_id!=(binding.role==WallEndpointRole::start?segment.start_vertex_id:segment.end_vertex_id))
                throw std::invalid_argument("A copied room constraint corner does not match its edge endpoint.");
            const auto corner=binding.role==WallEndpointRole::start?edge:(edge+1)%boundary.segments.size();
            return QStringLiteral("%1 · edge %2 · corner %3 (%4)").arg(name)
                .arg(edge+1).arg(corner+1).arg(binding.role==WallEndpointRole::start?QStringLiteral("start"):QStringLiteral("end"));
        }
        throw std::invalid_argument("A copied room constraint endpoint has no identified room edge.");
    }
    void initialize_room_constraints() {
        if (!replacement_preview || !room_constraint_table) return;
        for (const auto& id:replacement_preview->replacement.room_constraint_ids_requiring_review) {
            RoomConstraintRow row;row.original_id=id;row.fresh_id=replacement_preview->replacement.original_to_proposed.at(id);
            row.removal_requested=std::any_of(replacement_intent->intent.relation_mutations.begin(),
                replacement_intent->intent.relation_mutations.end(),[&](const auto& mutation) {
                    return mutation.kind==ConstraintRelationMutationKind::remove && mutation.constraint_id==id;
                });
            const auto decoded=decode_constraint_entity(analytical_entities().at(row.fresh_id));
            if (!decoded.supported()) throw std::invalid_argument("A copied room constraint cannot be reviewed safely.");
            for (std::size_t index=0;index<decoded.constraint->bindings.size();++index) {
                const auto& binding=decoded.constraint->bindings[index];
                const auto room=analytical_entities().find(binding.owner_id);
                if (room==analytical_entities().end() || !is_physical_wall_room(room->second)) continue;
                RoomEndpointRow endpoint;endpoint.binding_index=index;endpoint.original=binding;
                const auto table_row=room_constraint_table->rowCount();room_constraint_table->insertRow(table_row);
                if (row.endpoints.empty()) {
                    const auto name=text(property_text(source.entities().at(id),"name"));
                    auto* label=new QTableWidgetItem((name.isEmpty()?QStringLiteral("Constraint %1").arg(room_constraints.size()+1):name)+
                        QStringLiteral(" · ")+text(std::string(constraint_relation_name(decoded.constraint->relation))));
                    label->setToolTip(text(id));room_constraint_table->setItem(table_row,0,label);
                    row.decision=choice(room_constraint_table,QStringLiteral("phaseCopiedRoomConstraintDecision:")+text(id));
                    option(row.decision,QStringLiteral("Keep unchanged active room endpoints"),QStringLiteral("keep"),false);
                    row.decision->addItem(QStringLiteral("Remap every room endpoint"),QStringLiteral("remap"));
                    row.decision->addItem(QStringLiteral("Omit new copy; preserve original"),QStringLiteral("omit"));
                    room_constraint_table->setCellWidget(table_row,1,row.decision);
                    QObject::connect(row.decision,&QComboBox::currentIndexChanged,dialog,[this]{changed(true);});
                }
                auto* label=new QTableWidgetItem(endpoint_label(binding,analytical_entities()));
                label->setToolTip(QStringLiteral("Binding %1").arg(index+1));room_constraint_table->setItem(table_row,2,label);
                endpoint.target=choice(room_constraint_table,QStringLiteral("phaseCopiedRoomConstraintEndpoint:")+text(id)+":"+QString::number(index));
                endpoint.target->setEnabled(false);room_constraint_table->setCellWidget(table_row,3,endpoint.target);
                QObject::connect(endpoint.target,&QComboBox::currentIndexChanged,dialog,[this]{changed();});
                row.endpoints.push_back(std::move(endpoint));
            }
            if (row.endpoints.empty()) throw std::invalid_argument("A copied constraint has no original physical room endpoint.");
            room_constraints.push_back(std::move(row));
        }
        tabs->setTabText(5,QStringLiteral("Copied room constraints (%1)").arg(room_constraints.size()));
    }
    void update_room_constraint_targets(const PhaseWallReplacementEntities& rooms) {
        const auto active=active_physical_wall_room_ids(rooms);const std::set<std::string> active_ids(active.begin(),active.end());
        const auto original_organization=organize_project(analytical_entities());
        const auto reviewed_organization=organize_project(rooms);
        struct Target {WallEndpointBinding binding;QString label;std::string evidence;DrawingContext context;double elevation{};};
        std::vector<Target> targets;
        for (const auto& id:active) {
            const auto& room=rooms.at(id);const auto boundary=decode_identified_boundary_entity(room);
            const auto context=reviewed_organization.drawing_context(id);
            if (!context || !context->complete()) throw std::invalid_argument("A reviewed room has no resolved drawing context.");
            const auto lineage=validate_retained_physical_wall_room_lineage(room,*context);
            const auto evidence=entity_map_digest(PhaseWallReplacementEntities{{id,room}});
            for (const auto& segment:boundary.segments) for (const auto role:{WallEndpointRole::start,WallEndpointRole::end}) {
                WallEndpointBinding binding{id,role,segment.segment_id,role==WallEndpointRole::start?segment.start_vertex_id:segment.end_vertex_id};
                targets.push_back({binding,endpoint_label(binding,rooms),evidence,*context,lineage.effective_elevation_m});
            }
        }
        rebuilding=true;
        for (auto& row:room_constraints) {
            row.keep_available=!row.removal_requested;
            for (auto& endpoint:row.endpoints) {
                const auto original=analytical_entities().find(endpoint.original.owner_id),reviewed=rooms.find(endpoint.original.owner_id);
                if (!active_ids.contains(endpoint.original.owner_id) || original==analytical_entities().end() || reviewed==rooms.end() ||
                    !exact_entities(PhaseWallReplacementEntities{{original->first,original->second}},PhaseWallReplacementEntities{{reviewed->first,reviewed->second}}))
                    row.keep_available=false;
                std::optional<WallEndpointBinding> previous;std::string previous_evidence;
                const auto chosen=endpoint.target->currentIndex()-1;
                if (chosen>=0 && static_cast<std::size_t>(chosen)<endpoint.targets.size()) {
                    previous=endpoint.targets[chosen];previous_evidence=endpoint.target_evidence[chosen];
                }
                endpoint.target->clear();endpoint.target->addItem(QStringLiteral("Choose reviewed room edge / corner…"),QString{});
                endpoint.targets.clear();endpoint.target_evidence.clear();
                const auto original_context=original_organization.drawing_context(endpoint.original.owner_id);
                if (!original_context || !original_context->complete())
                    throw std::invalid_argument("An original room endpoint has no resolved drawing context.");
                const auto original_plane=validate_retained_physical_wall_room_lineage(
                    analytical_entities().at(endpoint.original.owner_id),*original_context);
                for (const auto& target:targets) {
                    if (target.context!=*original_context ||
                        std::abs(target.elevation-original_plane.effective_elevation_m)>default_geometry_tolerance_metres) continue;
                    endpoint.target->addItem(target.label);endpoint.targets.push_back(target.binding);endpoint.target_evidence.push_back(target.evidence);
                    if (previous && *previous==target.binding && previous_evidence==target.evidence)
                        endpoint.target->setCurrentIndex(endpoint.target->count()-1);
                }
                endpoint.target->setEnabled(!row.removal_requested && value(row.decision)=="remap");
            }
            if (auto* model=qobject_cast<QStandardItemModel*>(row.decision->model())) {
                if (auto* keep=model->item(1)) keep->setEnabled(row.keep_available);
                if (auto* remap=model->item(2)) remap->setEnabled(!row.removal_requested);
            }
            if ((value(row.decision)=="keep" && !row.keep_available) ||
                (value(row.decision)=="remap" && row.removal_requested)) row.decision->setCurrentIndex(0);
        }
        rebuilding=false;
    }
    std::vector<PhaseWallRoomConstraintDecision> room_constraint_decisions() const {
        std::vector<PhaseWallRoomConstraintDecision> decisions;
        for (const auto& row:room_constraints) {
            const auto action=value(row.decision);PhaseWallRoomConstraintDecision decision;decision.constraint_id=row.original_id;
            if (action=="omit") decision.disposition=PhaseWallRoomConstraintDisposition::omit;
            else if (action=="keep" && row.keep_available) decision.disposition=PhaseWallRoomConstraintDisposition::keep;
            else if (action=="remap") {
                decision.disposition=PhaseWallRoomConstraintDisposition::remap;
                for (const auto& endpoint:row.endpoints) {
                    const auto index=endpoint.target->currentIndex()-1;
                    if (index<0 || static_cast<std::size_t>(index)>=endpoint.targets.size())
                        throw std::invalid_argument("Choose a reviewed active room edge and corner for every remapped room endpoint.");
                    decision.endpoints.push_back({endpoint.binding_index,endpoint.targets.at(index)});
                }
            } else throw std::invalid_argument("Choose Keep, Remap or Omit for every copied room constraint.");
            decisions.push_back(std::move(decision));
        }
        return decisions;
    }
    QString plane_name(const PhysicalWallRoomPhasePlaneReview& plane) const {
        const auto height=metric?plane.effective_elevation_m:plane.effective_elevation_m/0.3048;
        return QStringLiteral("%1 / %2 / %3 · %4 %5").arg(entity_name(plane.context.building_id),
            entity_name(plane.context.floor_id),entity_name(plane.context.layer_id)).arg(height,0,'f',2)
            .arg(metric?QStringLiteral("m"):QStringLiteral("ft"));
    }
    void initialize() {
        rebuilding=true;
        try {
            require_current();
            if (replacement_intent) {
                replacement_preview=inspect_phase_wall_replacement_authoring(source,*replacement_intent);
                const auto record=decode_phase_wall_replacement_authoring(replacement_intent->wall_replacement);
                destination={record.registry_id,record.alternative_id};
                registry_command.expected_revision=source.revision();registry_command.message="Review proposed rooms";
                registry_command.entity_changes={EntityChange::upsert(analytical_entities().at(destination.registry_id))};
                inventory=replacement_preview->room_inventory;
                for (const auto& [id,entity]:replacement_preview->replacement.deferred_room_constraints) deferred_constraint_ids.insert(id);
                initialize_room_constraints();
            } else inventory=inspect_physical_wall_phase_room_review(source,registry_command,destination);
            if (inventory->reports.size()!=inventory->intent.planes.size()) throw std::invalid_argument("The room review is incomplete. Cancel and start again.");
            auto target_entities=analytical_entities();
            for (const auto& change:registry_command.entity_changes) if (change.kind==EntityChangeKind::upsert)
                target_entities.insert_or_assign(change.entity.id,change.entity);
            const auto model=ModelPhases::from_json(target_entities.at(destination.registry_id).properties.at("model"));
            baseline_rooms.insert(model.baseline_ids().begin(),model.baseline_ids().end());
            if (destination.alternative_id) for (const auto& alternative:model.alternatives()) if (alternative.id==*destination.alternative_id)
                proposed_rooms.insert(alternative.proposed_ids.begin(),alternative.proposed_ids.end());
            const auto roster=physical_wall_phase_room_roster(target_entities,destination);
            active_rooms.insert(roster.active_room_ids.begin(),roster.active_room_ids.end());
            planes.reserve(inventory->reports.size());
            for (std::size_t p=0;p<inventory->reports.size();++p) {
                const auto& report=inventory->reports[p].correspondence;const auto& plane=inventory->intent.planes[p];
                if (report.context!=plane.context || report.effective_elevation_m!=plane.effective_elevation_m ||
                    inventory->reports[p].destination_selection!=destination)
                    throw std::invalid_argument("The room review is incomplete. Cancel and start again.");
                plane_choice->addItem(plane_name(plane));
                auto* page=new QWidget(pages);auto* page_layout=new QVBoxLayout(page);PlaneRows controls;
                controls.previous_table=table(page,QStringLiteral("phasePreviousRooms:%1").arg(p),
                    {QStringLiteral("Previous room / classification"),QStringLiteral("Relationship"),QStringLiteral("Decision"),QStringLiteral("Review note")});
                controls.fresh_table=table(page,QStringLiteral("phaseCurrentSpaces:%1").arg(p),
                    {QStringLiteral("Current space"),QStringLiteral("Assign"),QStringLiteral("New name"),QStringLiteral("New classification"),QStringLiteral("Factor"),QStringLiteral("Interior point")});
                page_layout->addWidget(controls.previous_table);page_layout->addWidget(controls.fresh_table);pages->addWidget(page);
                for (const auto& old:report.retained) {
                    const auto identified=decode_identified_boundary_entity(old.room);
                    token_labels.insert_or_assign(old.room.id,entity_name(old.room.id));
                    for (std::size_t edge=0;edge<identified.segments.size();++edge) {
                        const auto& child=identified.segments[edge];
                        token_labels.insert_or_assign(child.segment_id,QStringLiteral("%1 · edge %2").arg(entity_name(old.room.id)).arg(edge+1));
                        token_labels.insert_or_assign(child.start_vertex_id,QStringLiteral("%1 · corner %2").arg(entity_name(old.room.id)).arg(edge+1));
                    }
                    auto& matches=controls.matching[old.room.id];
                    if (active_rooms.contains(old.room.id)) for (const auto& fresh:report.fresh) {
                        try {if (fresh.diagnostic.empty() && physical_wall_room_lineage_matches_current_inventory(old.room,report.context,space(fresh))) matches.insert(fresh.index);}
                        catch (const std::exception&) {} // Invalid original evidence never enables sharing.
                    }
                    const auto row=controls.previous_table->rowCount();controls.previous_table->insertRow(row);
                    controls.previous_table->setItem(row,0,new QTableWidgetItem(entity_name(old.room.id)+QStringLiteral(" · ")+text(property_text(old.room,"classification"))));
                    controls.previous_table->item(row,0)->setToolTip(text(old.room.id)+QStringLiteral("\nAuthored facts remain on this identity when redefined."));
                    controls.previous_table->setItem(row,1,new QTableWidgetItem(relationship(old.kind)));
                    auto* decision=choice(controls.previous_table,QStringLiteral("phasePreviousDecision:")+text(old.room.id));
                    option(decision,QStringLiteral("Share unchanged"),QStringLiteral("share"),!matches.empty(),QStringLiteral("Sharing requires the same walls and exact clear outline."));
                    const bool supersede=destination.alternative_id.has_value() && baseline_rooms.contains(old.room.id);
                    option(decision,QStringLiteral("Preserve baseline and replace in alternative"),QStringLiteral("supersede"),supersede,
                        QStringLiteral("Only a baseline room can be replaced in this alternative."));
                    const bool proposed=inventory->intent.proposed_room_completion && proposed_rooms.contains(old.room.id);
                    option(decision,QStringLiteral("Redefine proposed room; preserve facts"),QStringLiteral("redefine"),proposed,
                        QStringLiteral("Assign this same proposed identity to exactly one current space."));
                    option(decision,QStringLiteral("Retire proposed room"),QStringLiteral("retire"),proposed,
                        QStringLiteral("Remove this proposal and explicitly review its references and relationship rows."));
                    controls.previous_table->setCellWidget(row,2,decision);controls.previous.emplace(old.room.id,decision);
                    const auto note=!matches.empty()?QStringLiteral("Exact unchanged match available"):
                        supersede?QStringLiteral("Choose what replaces this baseline room"):
                        proposed?QStringLiteral("Choose one current space for redefinition, or retire explicitly"):
                        QStringLiteral("This review can only share this room unchanged");
                    auto* note_item=new QTableWidgetItem(note);note_item->setToolTip(note);controls.previous_table->setItem(row,3,note_item);
                    QObject::connect(decision,&QComboBox::currentIndexChanged,dialog,[this]{changed(true);});
                }
                for (std::size_t i=0;i<report.fresh.size();++i) {
                    const auto& fresh=report.fresh[i];if (fresh.index!=i) throw std::invalid_argument("The current room list changed. Cancel and start again.");
                    const auto row=controls.fresh_table->rowCount();controls.fresh_table->insertRow(row);FreshRow f;
                    controls.fresh_table->setItem(row,0,new QTableWidgetItem(QStringLiteral("C%1 · %2 · %3").arg(i+1).arg(area(fresh.area_square_metres,metric),relationship(fresh.kind))));
                    f.assignment=choice(controls.fresh_table,QStringLiteral("phaseFreshAssignment:%1:%2").arg(p).arg(i));
                    for (const auto& old:report.retained) if (controls.matching.at(old.room.id).contains(i))
                        f.assignment->addItem(QStringLiteral("Share %1 unchanged").arg(entity_name(old.room.id)),QStringLiteral("share:")+text(old.room.id));
                    if (inventory->intent.proposed_room_completion && fresh.diagnostic.empty())
                        for (const auto& old:report.retained) if (proposed_rooms.contains(old.room.id))
                            f.assignment->addItem(QStringLiteral("Redefine %1 [%2]; preserve facts").arg(entity_name(old.room.id),text(old.room.id)),QStringLiteral("redefine:")+text(old.room.id));
                    option(f.assignment,QStringLiteral("Create proposed room"),QStringLiteral("create"),destination.alternative_id.has_value() && fresh.diagnostic.empty(),
                        destination.alternative_id?QStringLiteral("This clear space must have complete geometry."):QStringLiteral("Creating rooms requires an alternative."));
                    f.assignment->addItem(QStringLiteral("Leave unclassified"),QStringLiteral("unclassified"));
                    f.name=new QLineEdit(controls.fresh_table);f.name->setMaxLength(4096);f.name->setPlaceholderText(QStringLiteral("Required for new room"));
                    f.classification=new QLineEdit(controls.fresh_table);f.classification->setMaxLength(4096);f.classification->setPlaceholderText(QStringLiteral("Required for new room"));
                    f.factor=new QLineEdit(controls.fresh_table);f.factor->setPlaceholderText(QStringLiteral("0–1000000"));
                    auto* validator=new QDoubleValidator(0,1000000,12,f.factor);validator->setNotation(QDoubleValidator::StandardNotation);
                    validator->setLocale(QLocale::c());f.factor->setValidator(validator);
                    f.name->setObjectName(QStringLiteral("phaseFreshName:%1:%2").arg(p).arg(i));
                    f.classification->setObjectName(QStringLiteral("phaseFreshClassification:%1:%2").arg(p).arg(i));
                    f.factor->setObjectName(QStringLiteral("phaseFreshFactor:%1:%2").arg(p).arg(i));
                    f.pick=new QPushButton(QStringLiteral("Pick inside C%1").arg(i+1),controls.fresh_table);
                    f.pick->setObjectName(QStringLiteral("phaseFreshPick:%1:%2").arg(p).arg(i));
                    controls.fresh_table->setCellWidget(row,1,f.assignment);controls.fresh_table->setCellWidget(row,2,f.name);
                    controls.fresh_table->setCellWidget(row,3,f.classification);controls.fresh_table->setCellWidget(row,4,f.factor);
                    controls.fresh_table->setCellWidget(row,5,f.pick);controls.fresh.push_back(f);
                    QObject::connect(f.assignment,&QComboBox::currentIndexChanged,dialog,[this,p,i]{
                        if (rebuilding) return;
                        auto& f=planes.at(p).fresh.at(i);
                        // Reassigning a current space retires its allocated
                        // endpoint evidence even before the next room replay.
                        rebuilding=true;
                        for (auto& relation:room_constraints) for (auto& endpoint:relation.endpoints) {
                            const auto selected=endpoint.target->currentIndex()-1;
                            if (selected<0 || static_cast<std::size_t>(selected)>=endpoint.targets.size()) continue;
                            const auto& target=endpoint.targets.at(selected);
                            if ((!f.allocated_room_id.empty() && target.owner_id==f.allocated_room_id) ||
                                std::find(f.ids.segment_ids.begin(),f.ids.segment_ids.end(),target.segment_id)!=f.ids.segment_ids.end())
                                endpoint.target->setCurrentIndex(0);
                        }
                        rebuilding=false;f.point.reset();f.ids={};f.dimension_ids.clear();f.allocated_room_id.clear();
                        active_pick.reset();changed(true);
                    });
                    for (auto* edit:{f.name,f.classification,f.factor}) QObject::connect(edit,&QLineEdit::textChanged,dialog,[this]{changed();});
                    QObject::connect(f.pick,&QPushButton::clicked,dialog,[this,p,i]{begin_pick(p,i);});
                }
                planes.push_back(std::move(controls));
            }
            if (planes.empty()) {plane_choice->addItem(QStringLiteral("No affected room planes"));plane_choice->setEnabled(false);}
            rebuilding=false;changed(true);scene();canvas->fitView();
        } catch (const std::exception& e) {
            rebuilding=false;inventory.reset();planes.clear();pages->setEnabled(false);plane_choice->setEnabled(false);canvas->setEnabled(false);
            fail(QStringLiteral("Room review is unavailable: %1").arg(QString::fromUtf8(e.what())));scene();
        }
    }
    std::vector<std::string> superseded() const {
        std::vector<std::string> result;
        for (const auto& plane:planes) for (const auto& [id,decision]:plane.previous) if (value(decision)=="supersede") result.push_back(id);
        std::sort(result.begin(),result.end());return result;
    }
    void rebuild_references() {
        const auto selected=superseded();if (selected==reference_superseded) return;
        std::vector<PhysicalWallRoomPhaseBaselineAcknowledgement> evidence;
        try {evidence=replacement_intent?physical_wall_phase_room_baseline_dependents(source,analytical_entities(),destination.registry_id,selected):
            physical_wall_phase_room_baseline_dependents(source,destination.registry_id,selected);}
        catch (const std::exception&) {throw std::invalid_argument("The baseline reference list is unavailable. Cancel and review the original rooms.");}
        std::map<std::string,PhysicalWallRoomPhaseBaselineAcknowledgement,std::less<>> confirmed;
        for (const auto& old:references) if (old.preserve->isChecked()) confirmed.emplace(old.evidence.entity_id,old.evidence);
        rebuilding=true;
        references.clear();reference_table->setRowCount(0);
        for (const auto& reference:evidence) {
            const auto row=reference_table->rowCount();reference_table->insertRow(row);
            const auto& entity=analytical_entities().at(reference.entity_id);
            const auto category=entity.type=="room_relationships"?QStringLiteral("Room relationships"):
                entity.type=="constraint"?QStringLiteral("Attached constraint"):
                entity.type=="model_phases"?QStringLiteral("Alternative settings"):
                entity.type.find("dimension")!=std::string::npos?QStringLiteral("Saved dimension"):
                QStringLiteral("Saved reference");
            const auto name=text(property_text(entity,"name"));
            auto* label=new QTableWidgetItem(name.isEmpty()?QStringLiteral("%1 %2").arg(category).arg(row+1):name+QStringLiteral(" · ")+category);
            label->setToolTip(text(reference.entity_id));reference_table->setItem(row,0,label);
            QStringList mentions;for (const auto& id:reference.referenced_ids)
                mentions.push_back(token_labels.contains(id)?token_labels.at(id):entity_name(id));
            reference_table->setItem(row,1,new QTableWidgetItem(mentions.join(QStringLiteral(", "))));
            auto* check=new QCheckBox(deferred_constraint_ids.contains(reference.entity_id)?
                QStringLiteral("Preserve captured copy for room review"):QStringLiteral("Preserve in baseline"),reference_table);
            check->setObjectName(QStringLiteral("phasePreserveBaselineReference:")+text(reference.entity_id));
            const auto old=confirmed.find(reference.entity_id);
            if (old!=confirmed.end() && old->second.expected_entity_digest==reference.expected_entity_digest &&
                old->second.referenced_ids==reference.referenced_ids) check->setChecked(true);
            reference_table->setCellWidget(row,2,check);references.push_back({reference,check});
            QObject::connect(check,&QCheckBox::toggled,dialog,[this]{changed();});
        }
        reference_superseded=selected;rebuilding=false;
        tabs->setTabText(1,QStringLiteral("Baseline references (%1)").arg(references.size()));
    }
    std::vector<std::string> changed_proposals(bool retiring_only=false) const {
        std::vector<std::string> result;
        for (const auto& plane:planes) for (const auto& [id,decision]:plane.previous) {
            const auto action=value(decision);
            if (action=="retire" || (!retiring_only && action=="redefine")) result.push_back(id);
        }
        std::sort(result.begin(),result.end());return result;
    }
    using Assignment=std::pair<std::size_t,std::size_t>; // plane, candidate
    std::map<std::string,Assignment,std::less<>> redefinitions() const {
        std::map<std::string,Assignment,std::less<>> result;
        for (std::size_t p=0;p<planes.size();++p) for (std::size_t i=0;i<planes[p].fresh.size();++i) {
            const auto action=value(planes[p].fresh[i].assignment);
            if (action.starts_with("redefine:") && !result.emplace(action.substr(9),Assignment{p,i}).second)
                throw std::invalid_argument("Assign each redefined proposed identity to exactly one current space.");
        }
        return result;
    }
    void allocate(std::size_t p,std::size_t i) {
        auto& f=planes.at(p).fresh.at(i);const auto action=value(f.assignment);
        if (action!="create" && !action.starts_with("redefine:")) return;
        if (f.ids.segment_ids.empty()) for (std::size_t edge=0;edge<inventory->reports.at(p).correspondence.fresh.at(i).boundary.size();++edge) {
            f.ids.segment_ids.push_back("segment-"+make_stable_id());f.ids.vertex_ids.push_back("vertex-"+make_stable_id());
            f.dimension_ids.push_back("dimension-"+make_stable_id());
        }
        if (action=="create" && f.allocated_room_id.empty()) f.allocated_room_id="physical-room-"+make_stable_id();
    }
    void rebuild_proposed_references() {
        const auto changed_ids=changed_proposals();
        if (changed_ids!=reference_changed) {
            const auto evidence=replacement_intent?physical_wall_phase_room_proposed_dependents(source,analytical_entities(),destination,changed_ids):
                physical_wall_phase_room_proposed_dependents(source,destination,changed_ids);
            const std::set<std::string> owners(changed_ids.begin(),changed_ids.end());
            std::vector<ProposedReferenceRow> rows;std::vector<QString> labels;
            for (const auto& id:evidence.reference_ids) {
                if (deferred_constraint_ids.contains(id)) continue; // Disposition belongs to the copied-constraint review.
                const auto& entity=analytical_entities().at(id);ProposedReferenceRow row;row.id=id;QString label;
                if (can_recognize_boundary_dimension_entity_type(entity.type)) {
                    const auto decoded=decode_boundary_dimension_entity(entity);
                    if (!decoded.supported() || !owners.contains(decoded.dimension->boundary_id))
                        throw std::invalid_argument("A proposed dimension cannot be reviewed safely.");
                    const auto& d=*decoded.dimension;row.dimension=true;row.owners.insert(d.boundary_id);
                    row.automatic_lengths=d.kind==BoundaryDimensionKind::segment_length && d.placement==BoundaryDimensionPlacement::automatic;
                    label=entity_name(id)+QStringLiteral(" · %1 dimension · %2").arg(text(std::string(boundary_dimension_kind_name(d.kind))),entity_name(d.boundary_id));
                    if (row.automatic_lengths) label+=QStringLiteral(" · replace with all current edge lengths");
                    else if (d.kind!=BoundaryDimensionKind::area) {
                        row.children.emplace(d.boundary_id,false,d.segment_id);
                        for (const auto& child:d.segment_chain_ids) row.children.emplace(d.boundary_id,false,child);
                        if (d.kind==BoundaryDimensionKind::angle) {
                            row.children.emplace(d.boundary_id,false,d.secondary_segment_id);row.children.emplace(d.boundary_id,true,d.vertex_id);
                        }
                    }
                } else if (entity.type=="constraint") {
                    const auto decoded=decode_constraint_entity(entity);
                    if (!decoded.supported()) throw std::invalid_argument("A proposed constraint cannot be reviewed safely.");
                    label=entity_name(id)+QStringLiteral(" · ")+text(std::string(constraint_relation_name(decoded.constraint->relation)));
                    for (const auto& binding:decoded.constraint->bindings) if (owners.contains(binding.owner_id)) {
                        row.owners.insert(binding.owner_id);row.children.emplace(binding.owner_id,false,binding.segment_id);
                        row.children.emplace(binding.owner_id,true,binding.vertex_id);
                    }
                    if (row.owners.empty()) throw std::invalid_argument("A proposed constraint has no changed owner.");
                } else throw std::invalid_argument("An unsupported proposed reference prevents Apply.");
                rows.push_back(std::move(row));labels.push_back(std::move(label));
            }
            std::map<std::string,std::pair<ProposedReferenceRow,QString>,std::less<>> previous;
            for (const auto& row:proposed_references) previous.emplace(row.id,std::make_pair(row,row.decision->currentData().toString()));
            rebuilding=true;proposed_references.clear();proposed_reference_table->setRowCount(0);
            for (std::size_t i=0;i<rows.size();++i) {
                auto row=std::move(rows[i]);const auto index=proposed_reference_table->rowCount();proposed_reference_table->insertRow(index);
                auto* label=new QTableWidgetItem(labels[i]);label->setToolTip(text(row.id));proposed_reference_table->setItem(index,0,label);
                row.decision=choice(proposed_reference_table,QStringLiteral("phaseProposedReferenceDecision:")+text(row.id));
                row.decision->addItem(row.automatic_lengths?QStringLiteral("Keep; regenerate all current edge lengths"):
                    row.children.empty()?QStringLiteral("Keep"):QStringLiteral("Keep; map edges / corners"),QStringLiteral("keep"));
                row.decision->addItem(QStringLiteral("Remove"),QStringLiteral("remove"));
                const auto old=previous.find(row.id);
                if (old!=previous.end() && old->second.first.owners==row.owners && old->second.first.children==row.children)
                    row.decision->setCurrentIndex(std::max(0,row.decision->findData(old->second.second)));
                proposed_reference_table->setCellWidget(index,1,row.decision);proposed_reference_table->setItem(index,2,new QTableWidgetItem);
                proposed_reference_table->setItem(index,3,new QTableWidgetItem);
                QObject::connect(row.decision,&QComboBox::currentIndexChanged,dialog,[this]{changed(true);});proposed_references.push_back(std::move(row));
            }
            reference_changed=changed_ids;rebuilding=false;
            tabs->setTabText(2,QStringLiteral("Proposed references (%1)").arg(proposed_references.size()));
        }
        const auto retiring=changed_proposals(true);
        if (retiring==graph_retiring) return;
        // Request only retiring owners. The helper's rows are evidence, and
        // each new retirement set requires fresh individual acknowledgement.
        const auto evidence=replacement_intent?physical_wall_phase_room_proposed_dependents(source,analytical_entities(),destination,retiring):
            physical_wall_phase_room_proposed_dependents(source,destination,retiring);
        rebuilding=true;graphs.clear();graph_table->setRowCount(0);
        const auto add_row=[this](const QString& label,const QString& name) {
            const auto index=graph_table->rowCount();graph_table->insertRow(index);graph_table->setItem(index,0,new QTableWidgetItem(label));
            auto* check=new QCheckBox(QStringLiteral("Remove shown row"),graph_table);check->setObjectName(name);
            graph_table->setCellWidget(index,1,check);QObject::connect(check,&QCheckBox::toggled,dialog,[this]{changed();});return check;
        };
        for (const auto& removal:evidence.relationship_removals) {
            GraphRows graph;graph.evidence=removal;
            const auto model_label=QStringLiteral("%1 [%2]").arg(entity_name(removal.entity_id),text(removal.entity_id));
            for (const auto& id:removal.removed_room_ids) graph.memberships.push_back(add_row(
                model_label+QStringLiteral(" · membership ")+entity_name(id)+QStringLiteral(" [")+text(id)+QStringLiteral("]"),
                QStringLiteral("phaseGraphMembership:")+text(removal.entity_id)+":"+text(id)));
            for (std::size_t i=0;i<removal.acknowledged_relations.size();++i) {
                const auto& relation=removal.acknowledged_relations[i];
                graph.relations.push_back(add_row(model_label+QStringLiteral(" · ")+text(relation.source_id)+" "+
                    relation_name(relation.kind)+" "+text(relation.target_id),
                    QStringLiteral("phaseGraphRelation:")+text(removal.entity_id)+":"+QString::number(i)));
            }
            graphs.push_back(std::move(graph));
        }
        graph_retiring=retiring;rebuilding=false;
        tabs->setTabText(3,QStringLiteral("Proposed relationships (%1)").arg(graph_table->rowCount()));
    }
    void rebuild_mappings() {
        std::map<Child,QString> previous;for (const auto& [child,combo]:mappings) previous.emplace(child,combo->currentData().toString());
        rebuilding=true;mapping_table->setRowCount(0);mappings.clear();
        try {
            for (std::size_t p=0;p<planes.size();++p) for (std::size_t i=0;i<planes[p].fresh.size();++i) allocate(p,i);
            const auto assigned=redefinitions();std::set<Child> required;
            for (const auto& reference:proposed_references) if (value(reference.decision)=="keep")
                required.insert(reference.children.begin(),reference.children.end());
            for (const auto& child:required) {
                const auto& [owner,vertex,old_id]=child;const auto row=mapping_table->rowCount();mapping_table->insertRow(row);
                auto* label=new QTableWidgetItem(token_labels.contains(old_id)?token_labels.at(old_id):entity_name(owner)+" · "+text(old_id));
                label->setToolTip(text(owner)+" / "+text(old_id));mapping_table->setItem(row,0,label);
                auto* combo=choice(mapping_table,QStringLiteral("phaseChildMapping:")+text(owner)+(vertex?":vertex:":":segment:")+text(old_id));
                if (assigned.contains(owner)) {
                    const auto [p,i]=assigned.at(owner);
                    if (planes[p].previous.contains(owner) && value(planes[p].previous.at(owner))=="redefine") {
                        const auto& f=planes[p].fresh[i];const auto& ids=vertex?f.ids.vertex_ids:f.ids.segment_ids;
                        for (std::size_t edge=0;edge<ids.size();++edge)
                            combo->addItem(plane_name(inventory->intent.planes[p])+QStringLiteral(" · C%1 %2%3").arg(i+1).arg(vertex?"V":"E").arg(edge+1),text(ids[edge]));
                    }
                }
                if (previous.contains(child)) combo->setCurrentIndex(std::max(0,combo->findData(previous.at(child))));
                mapping_table->setCellWidget(row,1,combo);mappings.emplace(child,combo);
                QObject::connect(combo,&QComboBox::currentIndexChanged,dialog,[this]{changed();});
            }
            rebuilding=false;
        } catch (...) {rebuilding=false;throw;}
    }
    std::vector<std::string> removed_presentation_items() const {
        const auto retiring=changed_proposals(true);std::set<std::string> removed(retiring.begin(),retiring.end()),redefined;
        for (const auto& plane:planes) for (const auto& [id,decision]:plane.previous)
            if (value(decision)=="redefine") redefined.insert(id);
        for (const auto& reference:proposed_references) {
            const auto action=value(reference.decision);
            if (action=="remove" || (action=="keep" && reference.automatic_lengths &&
                std::any_of(reference.owners.begin(),reference.owners.end(),[&](const auto& id){return redefined.contains(id);})))
                removed.insert(reference.id);
        }
        if (replacement_intent) {
            const auto affected=physical_wall_phase_room_proposed_dependents(source,analytical_entities(),destination,changed_proposals());
            const std::set<std::string> affected_ids(affected.reference_ids.begin(),affected.reference_ids.end());
            for (const auto& row:room_constraints) if (affected_ids.contains(row.fresh_id) &&
                (value(row.decision)=="remap" || value(row.decision)=="omit")) removed.insert(row.fresh_id);
        }
        return {removed.begin(),removed.end()};
    }
    void rebuild_presentation_removals() {
        const auto removed=removed_presentation_items();if (removed==presentation_removed) return;
        // Evidence is rederived from the original captured source, including
        // its exact before/after records; no prepared snapshot becomes authority.
        const auto evidence=replacement_intent?physical_wall_phase_room_presentation_removals(source,analytical_entities(),removed):
            physical_wall_phase_room_presentation_removals(source,removed);
        std::map<std::string,PhysicalWallRoomPhasePresentationRemoval,std::less<>> confirmed;
        for (const auto& row:presentation_rows) if (row.acknowledge->isChecked()) confirmed.emplace(row.evidence.entity_id,row.evidence);
        rebuilding=true;presentation_rows.clear();presentation_table->setRowCount(0);
        for (const auto& removal:evidence) {
            const auto row=presentation_table->rowCount();presentation_table->insertRow(row);
            auto* record=new QTableWidgetItem(entity_name(removal.entity_id));record->setToolTip(text(removal.entity_id));
            presentation_table->setItem(row,0,record);
            QStringList labels;
            for (const auto& id:removal.removed_entity_ids) {
                const auto name=token_labels.contains(id)?token_labels.at(id):entity_name(id);
                labels.push_back(name==text(id)?name:name+QStringLiteral(" [")+text(id)+QStringLiteral("]"));
            }
            auto* items=new QTableWidgetItem(labels.join(QStringLiteral("\n")));items->setToolTip(labels.join(QStringLiteral("\n")));
            presentation_table->setItem(row,1,items);
            auto* check=new QCheckBox(QStringLiteral("Apply this saved presentation change"),presentation_table);
            check->setObjectName(QStringLiteral("phasePresentationRemoval:")+text(removal.entity_id));
            const auto old=confirmed.find(removal.entity_id);
            if (old!=confirmed.end() && old->second.expected_entity_digest==removal.expected_entity_digest &&
                old->second.expected_replacement_entity_digest==removal.expected_replacement_entity_digest &&
                old->second.removed_entity_ids==removal.removed_entity_ids) check->setChecked(true);
            presentation_table->setCellWidget(row,2,check);presentation_rows.push_back({removal,check});
            QObject::connect(check,&QCheckBox::toggled,dialog,[this]{changed();});
        }
        presentation_removed=removed;rebuilding=false;
        tabs->setTabText(4,QStringLiteral("Presentation changes (%1)").arg(presentation_rows.size()));
    }
    PhysicalWallRoomPhaseReviewIntent intent() {
        require_current();if (!inventory) throw std::invalid_argument("The room review is unavailable. Cancel and start again.");
        auto result=inventory->intent;result.baseline_only_acknowledgements.clear();
        result.removed_reference_ids.clear();result.kept_reference_ids.clear();result.relationship_removals.clear();
        result.presentation_removals.clear();
        for (std::size_t p=0;p<planes.size();++p) {
            auto& review=result.planes.at(p);review.source_rooms.clear();review.fresh.clear();
            const auto& report=inventory->reports.at(p).correspondence;auto& controls=planes[p];
            std::set<std::string> assigned;
            for (const auto& old:report.retained) {
                const auto action=value(controls.previous.at(old.room.id));
                if (action.empty()) {
                    throw std::invalid_argument("Choose a decision for every previous room on every listed floor.");
                }
                PhysicalWallRoomPhaseSourceDecision d;d.room_id=old.room.id;d.expected_descriptor_digest=old.descriptor_digest;
                if (action=="supersede") {
                    if (!destination.alternative_id || !baseline_rooms.contains(old.room.id)) throw std::invalid_argument("Only baseline rooms can be replaced in an alternative.");
                    d.disposition=PhysicalWallRoomPhaseSourceDisposition::supersede_in_target;
                } else if (action=="redefine" || action=="retire") {
                    if (!result.proposed_room_completion || !destination.alternative_id || !proposed_rooms.contains(old.room.id))
                        throw std::invalid_argument("Only an original proposed room in this alternative can be redefined or retired.");
                    d.disposition=action=="redefine"?PhysicalWallRoomPhaseSourceDisposition::redefine_proposed:
                        PhysicalWallRoomPhaseSourceDisposition::retire_proposed;
                } else if (action!="share" || controls.matching.at(old.room.id).empty())
                    throw std::invalid_argument("Sharing requires unchanged walls and the same clear room outline.");
                review.source_rooms.push_back(std::move(d));
            }
            for (std::size_t i=0;i<controls.fresh.size();++i) {
                auto& f=controls.fresh[i];const auto action=value(f.assignment);const auto& current=report.fresh.at(i);
                if (action.empty()) throw std::invalid_argument("Choose a decision for every current space on every listed floor.");
                PhysicalWallRoomPhaseFreshDecision d;d.candidate_index=i;d.reviewed_source_lineage=current.source_lineage;
                if (action=="unclassified") d.disposition=PhysicalWallRoomPhaseFreshDisposition::leave_unclassified;
                else {
                    if (!f.point) throw std::invalid_argument("Pick an interior point for every shared or proposed room.");
                    d.interior_witness=*f.point;
                    if (action=="create") {
                        if (!destination.alternative_id) throw std::invalid_argument("Choose an alternative before creating a proposed room.");
                        d.name=f.name->text().trimmed().toStdString();d.classification=f.classification->text().trimmed().toStdString();
                        bool numeric=false;const auto factor=QLocale::c().toDouble(f.factor->text(),&numeric);
                        if (d.name.empty() || d.classification.empty() || f.factor->text().trimmed().isEmpty() || !f.factor->hasAcceptableInput() ||
                            !numeric || !std::isfinite(factor) || factor<0 || factor>1000000)
                            throw std::invalid_argument("Enter a name, classification and factor from 0 to 1000000 for every new room.");
                        d.factor=factor;d.disposition=PhysicalWallRoomPhaseFreshDisposition::create_proposed;
                        allocate(p,i);
                        d.room_id=f.allocated_room_id;d.fresh_ids=f.ids;
                    } else if (action.starts_with("redefine:")) {
                        d.room_id=action.substr(9);
                        if (!result.proposed_room_completion || !proposed_rooms.contains(d.room_id) ||
                            !controls.previous.contains(d.room_id) || value(controls.previous.at(d.room_id))!="redefine" ||
                            !assigned.insert(d.room_id).second)
                            throw std::invalid_argument("Assign each proposed room chosen for redefinition to exactly one current space on its floor.");
                        allocate(p,i);d.fresh_ids=f.ids;d.disposition=PhysicalWallRoomPhaseFreshDisposition::redefine_proposed;
                        // Default empty name/classification and absent factor
                        // preserve all facts on this same original identity.
                    } else if (action.starts_with("share:")) {
                        d.room_id=action.substr(6);
                        if (!controls.previous.contains(d.room_id) || value(controls.previous.at(d.room_id))!="share" ||
                            !controls.matching.at(d.room_id).contains(i) || !assigned.insert(d.room_id).second)
                            throw std::invalid_argument("Share each unchanged previous room with exactly one matching current space.");
                    } else throw std::invalid_argument("Choose a supported current-space decision.");
                }
                review.fresh.push_back(std::move(d));
            }
            for (const auto& [id,decision]:controls.previous) {
                const auto action=value(decision);
                if ((action=="share" || action=="redefine") && !assigned.contains(id))
                    throw std::invalid_argument("Assign exactly one current space to every room chosen for sharing or redefinition.");
            }
        }
        for (const auto& reference:references) {
            if (!reference.preserve->isChecked()) throw std::invalid_argument("Confirm Preserve in baseline for every listed reference.");
            result.baseline_only_acknowledgements.push_back(reference.evidence);
        }
        const auto assigned=redefinitions();const auto retiring_ids=changed_proposals(true);
        const std::set<std::string> retiring(retiring_ids.begin(),retiring_ids.end());
        const auto source_decision=[&result](const std::string& id)->PhysicalWallRoomPhaseSourceDecision& {
            for (auto& plane:result.planes) for (auto& decision:plane.source_rooms) if (decision.room_id==id) return decision;
            throw std::invalid_argument("A kept reference has no reviewed proposed room owner.");
        };
        for (const auto& reference:proposed_references) {
            const auto action=value(reference.decision);
            if (action=="remove") {result.removed_reference_ids.push_back(reference.id);continue;}
            if (action!="keep") throw std::invalid_argument("Choose Keep or Remove for every listed proposed reference.");
            if (std::any_of(reference.owners.begin(),reference.owners.end(),[&](const auto& id){return retiring.contains(id);}))
                throw std::invalid_argument("A reference touching a retired proposed room requires explicit removal.");
            result.kept_reference_ids.push_back(reference.id);
            if (reference.automatic_lengths) for (const auto& owner:reference.owners) {
                if (!assigned.contains(owner)) throw std::invalid_argument("Assign a current space before regenerating its automatic edge dimensions.");
                const auto [p,i]=assigned.at(owner);source_decision(owner).replacement_dimension_ids=planes.at(p).fresh.at(i).dimension_ids;
            }
            for (const auto& child:reference.children) {
                const auto& [owner,vertex,old_id]=child;const auto mapping=mappings.find(child);
                if (mapping==mappings.end() || value(mapping->second).empty())
                    throw std::invalid_argument("Map every kept proposed edge and corner explicitly to the chosen current space.");
                auto& decision=source_decision(owner);
                if (decision.disposition!=PhysicalWallRoomPhaseSourceDisposition::redefine_proposed)
                    throw std::invalid_argument("Kept child mappings require a proposed room redefinition.");
                if (decision.child_mapping.empty()) decision.child_mapping={{"segments",nlohmann::json::object()},{"vertices",nlohmann::json::object()}};
                decision.child_mapping[vertex?"vertices":"segments"][old_id]=value(mapping->second);
            }
        }
        if (replacement_intent) {
            const auto affected=physical_wall_phase_room_proposed_dependents(source,analytical_entities(),destination,changed_proposals());
            const std::set<std::string> affected_ids(affected.reference_ids.begin(),affected.reference_ids.end());
            for (const auto& row:room_constraints) if (affected_ids.contains(row.fresh_id)) {
                const auto action=value(row.decision);
                if (action=="remap" || action=="omit") result.removed_reference_ids.push_back(row.fresh_id);
                else if (action=="keep") result.kept_reference_ids.push_back(row.fresh_id);
                else throw std::invalid_argument("Choose a disposition for every copied room constraint before reviewing changed proposed rooms.");
                // Only reconstructed fresh copies receive this transient room
                // disposition. The enclosing typed command reinstates a Remap
                // copy with its explicitly chosen final bindings after room33.
            }
        }
        for (const auto& graph:graphs) {
            for (auto* check:graph.memberships) if (!check->isChecked())
                throw std::invalid_argument("Acknowledge every removed proposed room-relationship membership and incident row.");
            for (auto* check:graph.relations) if (!check->isChecked())
                throw std::invalid_argument("Acknowledge every removed proposed room-relationship membership and incident row.");
            result.relationship_removals.push_back(graph.evidence);
        }
        for (const auto& row:presentation_rows) {
            if (!row.acknowledge->isChecked())
                throw std::invalid_argument("Acknowledge each listed saved presentation and annotation change.");
            result.presentation_removals.push_back(row.evidence);
        }
        return result;
    }
    std::map<std::string,Entity,std::less<>> numerical_entities(const DocumentSnapshot& snapshot) const {
        auto entities=snapshot.entities();
        entities.at(destination.registry_id).properties.at("model")["active_alternative"]=
            destination.alternative_id?nlohmann::json(*destination.alternative_id):nlohmann::json(nullptr);
        return entities;
    }
    void update_reference_preview(const DocumentSnapshot& exact,const PhysicalWallRoomPhaseReviewIntent& decisions) {
        const auto numerical=numerical_entities(exact);
        for (std::size_t i=0;i<proposed_references.size();++i) {
            const auto& reference=proposed_references[i];QString description=QStringLiteral("Remove on Apply");
            if (value(reference.decision)=="keep") {
                if (reference.automatic_lengths) {
                    QStringList replacements;
                    for (const auto& plane:decisions.planes) for (const auto& owner:plane.source_rooms) if (reference.owners.contains(owner.room_id))
                        for (std::size_t edge=0;edge<owner.replacement_dimension_ids.size();++edge) {
                            const auto& id=owner.replacement_dimension_ids[edge];const auto decoded=decode_boundary_dimension_entity(exact.entities().at(id));
                            if (!decoded.supported()) throw std::invalid_argument("A reviewed replacement dimension cannot be resolved.");
                            replacements.push_back(QStringLiteral("E%1: %2").arg(edge+1).arg(quantity(resolve_boundary_dimension(*decoded.dimension,numerical),metric)));
                        }
                    description=QStringLiteral("Replace with %1 dimensions · %2").arg(replacements.size()).arg(replacements.join(QStringLiteral(", ")));
                } else if (reference.dimension) {
                    const auto decoded=decode_boundary_dimension_entity(exact.entities().at(reference.id));
                    if (!decoded.supported()) throw std::invalid_argument("A kept proposed dimension cannot be resolved.");
                    description=quantity(resolve_boundary_dimension(*decoded.dimension,numerical),metric);
                } else description=QStringLiteral("Keep on explicitly mapped edges / corners");
            }
            proposed_reference_table->item(static_cast<int>(i),2)->setText(description);
        }
    }
    void update_copied_constraint_preview(const DocumentSnapshot& exact) {
        if (!room_constraint_table) return;
        int table_row=0;
        for (const auto& row:room_constraints) {
            const auto found=exact.entities().find(row.fresh_id);
            for (const auto& endpoint:row.endpoints) {
                QString label=QStringLiteral("New copy omitted; original preserved");
                if (found!=exact.entities().end()) {
                    const auto decoded=decode_constraint_entity(found->second);
                    if (!decoded.supported()) throw std::invalid_argument("The prepared copied constraint cannot be displayed.");
                    label=endpoint_label(decoded.constraint->bindings.at(endpoint.binding_index),exact.entities());
                }
                room_constraint_table->item(table_row,2)->setToolTip(QStringLiteral("Original: %1\nPrepared: %2")
                    .arg(endpoint_label(endpoint.original,analytical_entities()),label));
                ++table_row;
            }
        }
    }
    void update_replacement_identities() {
        const auto assigned=redefinitions();
        for (std::size_t i=0;i<proposed_references.size();++i) {
            const auto& reference=proposed_references[i];QStringList ids;
            if (reference.automatic_lengths && value(reference.decision)=="keep") for (const auto& owner:reference.owners) if (assigned.contains(owner)) {
                const auto [p,c]=assigned.at(owner);const auto& dimensions=planes[p].fresh[c].dimension_ids;
                for (std::size_t edge=0;edge<dimensions.size();++edge)
                    ids.push_back(QStringLiteral("C%1 E%2: %3").arg(c+1).arg(edge+1).arg(text(dimensions[edge])));
            }
            auto* item=proposed_reference_table->item(static_cast<int>(i),3);
            item->setText(ids.join(QStringLiteral("\n")));item->setToolTip(ids.join(QStringLiteral("\n")));
        }
    }
    void changed(bool topology=false) {
        if (rebuilding || invalidated) return;
        candidate.reset();candidate_snapshot.reset();accepted.reset();apply->setEnabled(false);
        rebuilding=true;
        for (auto& relation:room_constraints) {
            const bool changed_original=std::any_of(relation.endpoints.begin(),relation.endpoints.end(),[this](const auto& endpoint) {
                for (const auto& plane:planes) if (plane.previous.contains(endpoint.original.owner_id)) {
                    const auto action=value(plane.previous.at(endpoint.original.owner_id));
                    return action=="supersede" || action=="redefine" || action=="retire";
                }
                return false;
            });
            if (changed_original) {
                relation.keep_available=false;
                if (auto* model=qobject_cast<QStandardItemModel*>(relation.decision->model())) if (auto* keep=model->item(1)) keep->setEnabled(false);
                if (value(relation.decision)=="keep") relation.decision->setCurrentIndex(0);
            }
        }
        rebuilding=false;
        for (auto& plane:planes) for (auto& f:plane.fresh) {
            const auto action=value(f.assignment);const bool create=action=="create";
            f.name->setEnabled(create);f.classification->setEnabled(create);f.factor->setEnabled(create);
            f.pick->setEnabled(!action.empty() && action!="unclassified");
            f.pick->setText(f.point?QStringLiteral("● Interior chosen · pick again"):QStringLiteral("Pick inside"));
        }
        for (int row=0;row<proposed_reference_table->rowCount();++row) proposed_reference_table->item(row,2)->setText({});
        try {require_current();rebuild_references();rebuild_proposed_references();rebuild_presentation_removals();if (topology) rebuild_mappings();update_replacement_identities();const auto decisions=intent();
            try {
                ApplyBoundaryConstraintChanges command;std::optional<PreparedPhysicalWallPhaseRoomReview> prepared;
                if (replacement_intent) {
                    const auto encoded=encode_physical_wall_phase_room_review_intent(decisions);
                    const auto reviewed=replay_physical_wall_phase_room_review_with_deferred_constraints(analytical_entities(),encoded,deferred_constraint_ids);
                    update_room_constraint_targets(reviewed.entities);
                    auto replacement=*replacement_intent;
                    auto record=decode_phase_wall_replacement_authoring(replacement.wall_replacement);
                    record.room_review_intent=encoded;record.room_constraint_decisions=room_constraint_decisions();
                    replacement.wall_replacement=encode_phase_wall_replacement_authoring(record);
                    command=phase_wall_replacement_authoring_command(replacement);
                } else {
                    prepared=prepare_physical_wall_phase_room_review(source,decisions);
                    command.expected_revision=source.revision();command.message="Review alternative rooms";
                    command.phase_room_review_completion=true;command.phase_room_review_intent=prepared->intent;
                }
                auto exact=Document::preview_command(source,command);
                if (prepared && !exact_entities(exact.entities(),prepared->entities))
                    throw std::invalid_argument("Prepared room decisions differ from the complete command.");
                if (exact.assets()!=source.assets()) throw std::invalid_argument("The reviewed room command changed project assets.");
                update_reference_preview(exact,decisions);
                update_copied_constraint_preview(exact);
                require_current();candidate=std::move(command);candidate_snapshot=std::move(exact);
                error.clear();status->setText(QStringLiteral("All listed rooms and references are reviewed. Apply saves these choices together."));apply->setEnabled(true);
            } catch (const std::exception& e) {
                fail(QStringLiteral("These room choices cannot be applied: %1").arg(QString::fromUtf8(e.what())));
            }
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));}
        if (invalidated) return;
        scene();
    }
    void begin_pick(std::size_t p,std::size_t i) {
        try {
            require_current();auto& f=planes.at(p).fresh.at(i);const auto action=value(f.assignment);
            if (action.empty() || action=="unclassified") throw std::invalid_argument("Assign this space before choosing an interior point.");
            active_pick=std::make_pair(p,i);scene();
            status->setText(QStringLiteral("Click strictly inside C%1, outside holes and wall material.").arg(i+1));
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));}
    }
    void pick(Vec2 point) {
        try {
            require_current();if (!active_pick || !inventory) throw std::invalid_argument("Choose Pick inside for the intended space first.");
            const auto [p,i]=*active_pick;auto& f=planes.at(p).fresh.at(i);const auto& current=inventory->reports.at(p).correspondence.fresh.at(i);
            f.point.reset();
            if (!interior_point(current,point))
                throw std::invalid_argument("Click inside the chosen clear space, outside holes and wall material.");
            f.point=point;f.pick->setToolTip(QStringLiteral("Chosen interior: %1, %2 %3")
                .arg(metric?point.x:point.x/0.3048,0,'f',3).arg(metric?point.y:point.y/0.3048,0,'f',3).arg(metric?"m":"ft"));
            active_pick.reset();changed();
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));scene();}
    }
    void scene() {
        std::vector<CanvasEntity> entities;std::vector<CanvasLabel> labels;
        const auto p=plane_choice->currentIndex();
        if (!invalidated && inventory && p>=0 && static_cast<std::size_t>(p)<planes.size()) {
            const auto& report=inventory->reports.at(static_cast<std::size_t>(p)).correspondence;
            plane_status->setText(report.fresh.empty()?QStringLiteral("No current clear spaces on this plane. Every previous room still needs a decision."):
                QStringLiteral("Review %1 previous rooms and %2 current clear spaces on this plane.").arg(report.retained.size()).arg(report.fresh.size()));
            if (replacement_intent) {
                try {
                    const auto& displayed=candidate_snapshot?candidate_snapshot->entities():analytical_entities();
                    const auto scope=constraint_phase_scope(displayed);const auto organization=organize_project(displayed);
                    auto projection=displayed;
                    for (const auto& id:scope.inactive_owner_ids) if (projection.contains(id) && projection.at(id).type=="wall") projection.erase(id);
                    // Scene-only derived geometry uses the actual full stage's
                    // active walls and placements; it carries no source authority.
                    for (auto& [id,entity]:projection) if (entity.type=="wall") entity=resolve_vertical_placement(displayed,entity);
                    const auto walls=document_wall_plan_geometry(projection);const auto& plane=inventory->intent.planes.at(static_cast<std::size_t>(p));
                    for (const auto& [id,geometry]:walls) {
                        if (scope.inactive_owner_ids.contains(id)) continue;
                        const auto context=organization.drawing_context(id);if (!context || *context!=plane.context) continue;
                        const auto entity=resolve_vertical_placement(displayed,displayed.at(id));Wall wall;std::string diagnostic;
                        if (!read_document_wall(entity,{},wall,diagnostic) ||
                            std::abs(wall.elevation-plane.effective_elevation_m)>default_geometry_tolerance_metres) continue;
                        CanvasEntity e{QStringLiteral("phase-wall:")+text(id),QStringLiteral("wall"),geometry.strokes,0,false};
                        e.stroke_color=QColor(73,85,99);e.dark_stroke_color=QColor(195,205,216);entities.push_back(std::move(e));
                    }
                } catch (const std::exception& e) {fail(QStringLiteral("The proposed wall scene cannot be displayed: %1").arg(QString::fromUtf8(e.what())));}
            }
            for (const auto& old:report.retained) if (!old.boundary.empty()) {
                CanvasEntity e{text(old.room.id),QStringLiteral("boundary"),old.boundary,0,false};e.holes=old.holes;
                e.stroke_color=QColor(130,143,158);e.dark_stroke_color=QColor(168,181,196);e.dashed_stroke=true;entities.push_back(std::move(e));
                if (value(planes.at(static_cast<std::size_t>(p)).previous.at(old.room.id))=="redefine")
                    for (std::size_t edge=0;edge<old.boundary.size();++edge) {
                        const auto& segment=old.boundary[edge];
                        labels.push_back({{},segment.start,entity_name(old.room.id)+QStringLiteral(" old V%1").arg(edge+1)});
                        labels.push_back({{},{(segment.start.x+segment.end.x)/2,(segment.start.y+segment.end.y)/2},
                            entity_name(old.room.id)+QStringLiteral(" old E%1").arg(edge+1)});
                    }
            }
            for (std::size_t i=0;i<report.fresh.size();++i) {
                const auto& current=report.fresh[i];CanvasEntity e{QStringLiteral("phase-candidate:%1:%2").arg(p).arg(i),QStringLiteral("boundary"),current.boundary,0,false};
                e.holes=current.holes;e.stroke_color=QColor(36,107,206);e.dark_stroke_color=QColor(104,171,255);
                e.filled=active_pick && active_pick->first==static_cast<std::size_t>(p) && active_pick->second==i;
                e.fill_color=QColor(36,107,206,35);entities.push_back(std::move(e));
                if (!current.boundary.empty()) labels.push_back({{},current.boundary.front().start,QStringLiteral("C%1").arg(i+1)});
                const auto& f=planes.at(static_cast<std::size_t>(p)).fresh.at(i);
                if (value(f.assignment).starts_with("redefine:") || (replacement_intent && value(f.assignment)!="unclassified"))
                    for (std::size_t edge=0;edge<current.boundary.size();++edge) {
                    const auto& segment=current.boundary[edge];
                    labels.push_back({{},segment.start,QStringLiteral("C%1 V%2").arg(i+1).arg(edge+1)});
                    labels.push_back({{},{(segment.start.x+segment.end.x)/2,(segment.start.y+segment.end.y)/2},
                        QStringLiteral("C%1 E%2").arg(i+1).arg(edge+1)});
                }
                if (f.point) {CanvasLabel marker{QStringLiteral("phase-interior:%1:%2").arg(p).arg(i),*f.point,QStringLiteral("●")};
                    marker.bold=true;marker.paper_height_mm=3.5;labels.push_back(std::move(marker));}
            }
            if (candidate_snapshot) {
                // Draw the admitted command's actual entities, independently
                // of the correspondence outlines used to collect decisions.
                std::vector<CanvasEntity> preview_entities;std::vector<CanvasLabel> preview_labels;
                try {
                    const auto numerical=numerical_entities(*candidate_snapshot);std::set<std::string> preview_rooms,dimensions;
                    for (const auto& f:planes.at(static_cast<std::size_t>(p)).fresh) {
                        const auto action=value(f.assignment);std::string id;
                        if (action=="create") id=f.allocated_room_id;
                        else if (action.starts_with("redefine:")) id=action.substr(9);
                        else if (action.starts_with("share:")) id=action.substr(6);
                        if (id.empty() || !preview_rooms.insert(id).second) continue;
                        const auto& room=candidate_snapshot->entities().at(id);
                        const auto boundary=boundary_geometry(decode_identified_boundary_entity(room));
                        CanvasEntity e{QStringLiteral("phase-prepared:")+text(id),QStringLiteral("boundary"),boundary,0,false};
                        e.holes=decode_physical_wall_room_descriptor(room).holes;e.stroke_color=QColor(28,142,87);e.dark_stroke_color=QColor(94,209,145);
                        preview_entities.push_back(std::move(e));
                        if (!boundary.empty()) preview_labels.push_back({{},boundary.front().start,
                            QStringLiteral("Preview: ")+text(property_text(room,"name"))});
                        if (replacement_intent) for (std::size_t edge=0;edge<boundary.size();++edge) {
                            const auto& segment=boundary[edge];const auto name=text(property_text(room,"name"));
                            preview_labels.push_back({{},segment.start,QStringLiteral("%1 V%2").arg(name).arg(edge+1)});
                            preview_labels.push_back({{},{(segment.start.x+segment.end.x)/2,(segment.start.y+segment.end.y)/2},
                                QStringLiteral("%1 E%2").arg(name).arg(edge+1)});
                        }
                        for (const auto& dimension_id:f.dimension_ids) if (candidate_snapshot->entities().contains(dimension_id)) dimensions.insert(dimension_id);
                    }
                    for (const auto& reference:proposed_references) if (reference.dimension && !reference.automatic_lengths && value(reference.decision)=="keep")
                        dimensions.insert(reference.id);
                    for (const auto& id:dimensions) {
                        const auto decoded=decode_boundary_dimension_entity(candidate_snapshot->entities().at(id));
                        if (!decoded.supported()) throw std::invalid_argument("A prepared dimension cannot be displayed.");
                        const auto& dimension=*decoded.dimension;if (!preview_rooms.contains(dimension.boundary_id)) continue;
                        preview_labels.push_back({text(id),dimension.text_position,quantity(resolve_boundary_dimension(dimension,numerical),metric)});
                    }
                    entities.insert(entities.end(),preview_entities.begin(),preview_entities.end());
                    labels.insert(labels.end(),preview_labels.begin(),preview_labels.end());
                    plane_status->setText(plane_status->text()+QStringLiteral(" Prepared preview: %1 assigned rooms.").arg(preview_rooms.size()));
                    if (replacement_intent) {
                        std::size_t retained_copies=0;
                        for (const auto& row:room_constraints) if (candidate_snapshot->entities().contains(row.fresh_id)) ++retained_copies;
                        plane_status->setText(plane_status->text()+QStringLiteral(" %1 copied room constraints retained; %2 omitted.")
                            .arg(retained_copies).arg(room_constraints.size()-retained_copies));
                    }
                } catch (const std::exception& e) {fail(QStringLiteral("The prepared scene cannot be displayed: %1").arg(QString::fromUtf8(e.what())));}
            }
        } else plane_status->setText(invalidated?QString{}:!inventory?QStringLiteral("Room preview is unavailable."):
            QStringLiteral("No room planes require review. This phase selection needs no new room definitions."));
        canvas->setEntities(std::move(entities));canvas->setLabels(std::move(labels));
    }
    bool submit() {
        changed();if (!candidate || !candidate_snapshot) return false;
        try {
            require_current();const auto exact=Document::preview_command(source,*candidate);
            if (!exact_entities(exact.entities(),candidate_snapshot->entities()) || exact.assets()!=candidate_snapshot->assets())
                throw std::invalid_argument("The accepted room preview changed. Review it again.");
            require_current();accepted=candidate;return true;
        } catch (const std::exception&) {fail(QStringLiteral("The project or room preview changed. Cancel and start a new review."));return false;}
    }
};

PhysicalWallPhaseRoomReviewDialog::PhysicalWallPhaseRoomReviewDialog(DocumentSnapshot source,ApplyEntityChanges registry_command,
    PhysicalWallPhaseSelection destination,bool metric_units,std::function<DocumentSnapshot()> current_source,QWidget* parent):
    QDialog(parent),m_impl(std::make_unique<Impl>(this,std::move(source),std::move(registry_command),std::move(destination),metric_units,std::move(current_source))) {}
PhysicalWallPhaseRoomReviewDialog::PhysicalWallPhaseRoomReviewDialog(DocumentSnapshot source,PhaseConstraintAuthoringIntent replacement_intent,
    bool metric_units,std::function<DocumentSnapshot()> current_source,QWidget* parent):
    QDialog(parent),m_impl(std::make_unique<Impl>(this,std::move(source),ApplyEntityChanges{},PhysicalWallPhaseSelection{},
        metric_units,std::move(current_source),std::move(replacement_intent))) {}
PhysicalWallPhaseRoomReviewDialog::~PhysicalWallPhaseRoomReviewDialog()=default;
const std::optional<ApplyBoundaryConstraintChanges>& PhysicalWallPhaseRoomReviewDialog::acceptedCommand() const {return m_impl->accepted;}
QString PhysicalWallPhaseRoomReviewDialog::lastError() const {return m_impl->error;}
void PhysicalWallPhaseRoomReviewDialog::accept() {if (m_impl->submit()) QDialog::accept();}
void PhysicalWallPhaseRoomReviewDialog::reject() {
    m_impl->accepted.reset();m_impl->candidate.reset();m_impl->candidate_snapshot.reset();QDialog::reject();
}
} // namespace sketch::desktop

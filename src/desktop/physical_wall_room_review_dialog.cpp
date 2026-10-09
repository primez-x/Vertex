#include "sketch/desktop/physical_wall_room_review_dialog.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "plan_canvas.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch::desktop {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString area(double value,bool metric) {
    return QStringLiteral("%1 %2").arg(metric?value:value/0.09290304,0,'f',2).arg(metric?QStringLiteral("m²"):QStringLiteral("sq ft"));
}
QString quantity(const BoundaryDimensionResolution& value,bool metric) {
    if (value.kind==BoundaryDimensionKind::area) return area(value.area_square_metres,metric);
    if (value.kind==BoundaryDimensionKind::angle) return QStringLiteral("%1°").arg(value.angle_radians*180/std::numbers::pi,0,'f',1);
    return PlanCanvas::drawingLengthText(value.segment_length_metres,metric);
}
QString kind(PhysicalWallRoomCorrespondenceKind value) {
    switch (value) {
    case PhysicalWallRoomCorrespondenceKind::unique_continuation:return QStringLiteral("Continuation");
    case PhysicalWallRoomCorrespondenceKind::split:return QStringLiteral("Split");
    case PhysicalWallRoomCorrespondenceKind::merge:return QStringLiteral("Merge");
    case PhysicalWallRoomCorrespondenceKind::new_space:return QStringLiteral("New space");
    case PhysicalWallRoomCorrespondenceKind::retired:return QStringLiteral("Retired");
    default:return QStringLiteral("Needs review");
    }
}
QString relation_name(RoomRelationKind value) {
    if (value==RoomRelationKind::independent) return QStringLiteral("independent");
    if (value==RoomRelationKind::follows) return QStringLiteral("follows");
    return QStringLiteral("derived from");
}
using Child=std::tuple<std::string,bool,std::string>; // owner, vertex, child ID
QTableWidget* table(QWidget* parent,const char* name,const QStringList& headers) {
    auto* result=new QTableWidget(0,headers.size(),parent);result->setObjectName(QString::fromLatin1(name));
    result->setHorizontalHeaderLabels(headers);result->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result->verticalHeader()->hide();result->setEditTriggers(QAbstractItemView::NoEditTriggers);return result;
}
QComboBox* choice(QWidget* parent,const QString& name) {
    auto* result=new QComboBox(parent);result->setObjectName(name);result->addItem(QStringLiteral("Choose…"),QString{});return result;
}
std::string value(QComboBox* combo) { return combo->currentData().toString().toStdString(); }
bool curve_proposal(const std::optional<Command>& predecessor) {
    const auto* command=predecessor?std::get_if<ApplyBoundaryConstraintChanges>(&*predecessor):nullptr;
    return command && command->curve_construction_completion;
}
} // namespace

class PhysicalWallRoomReviewDialog::Impl {
public:
    struct FreshRow {
        QComboBox* assignment{};QLineEdit* name{};QLineEdit* classification{};QPushButton* pick{};QTableWidgetItem* witness{};
        std::string allocated_room_id;LegacyBoundaryIdentityOptions ids;std::vector<std::string> dimension_ids;std::optional<Vec2> point;
    };
    struct Reference {
        std::string id;std::set<std::string> owners;std::set<Child> children;bool dimension{};bool automatic_lengths{};bool area_dimension{};bool preserved_constraint{};QComboBox* decision{};
    };
    struct Graph {
        std::string id;RoomRelationshipSnapshot model;
        std::map<std::string,QCheckBox*,std::less<>> memberships;std::vector<QCheckBox*> relations;
    };
    PhysicalWallRoomReviewDialog* dialog;
    DocumentSnapshot original_source;
    DocumentSnapshot source;
    std::optional<Command> predecessor;
    std::function<DocumentSnapshot()> current_source;
    std::string original_digest;
    std::string source_digest;
    bool metric;
    std::optional<DrawingContext> selected_context;
    std::optional<double> selected_elevation;
    QComboBox* walls{};PlanCanvas* canvas{};QLabel* status{};QPushButton* apply{};
    QTabWidget* tabs{};QCheckBox* deletion_acknowledgement{};
    QTableWidget* retained_table{};QTableWidget* fresh_table{};QTableWidget* reference_table{};QTableWidget* mapping_table{};QTableWidget* graph_table{};
    std::optional<PhysicalWallRoomCorrespondenceReport> report;
    std::map<std::string,QComboBox*,std::less<>> retained;
    std::vector<FreshRow> fresh;
    std::vector<Reference> references;
    std::vector<Graph> graphs;
    std::map<Child,QComboBox*> mappings;
    std::optional<std::size_t> active;
    std::optional<ApplyBoundaryConstraintChanges> candidate,accepted;
    std::optional<DocumentSnapshot> candidate_snapshot;
    std::optional<DocumentSnapshot> dimension_source;
    std::vector<PhysicalWallRoomDimensionPlacement> selected_dimension_placements;
    QString error;
    bool rebuilding{};
    bool invalidated{};

    Impl(PhysicalWallRoomReviewDialog* owner,DocumentSnapshot captured,std::string wall_id,bool metric_units,
        std::function<DocumentSnapshot()> current,std::optional<Command> geometry,
        std::optional<DrawingContext> context=std::nullopt,std::optional<double> elevation=std::nullopt):dialog(owner),original_source(std::move(captured)),
        source(geometry?preview_physical_wall_room_review_geometry(original_source,*geometry):original_source),predecessor(std::move(geometry)),
        current_source(std::move(current)),original_digest(document_snapshot_digest(original_source)),
        source_digest(document_snapshot_digest(source)),metric(metric_units),selected_context(std::move(context)),selected_elevation(elevation) {
        dialog->setObjectName(QStringLiteral("physicalRoomReviewDialog"));dialog->setWindowTitle(QStringLiteral("Review rooms from walls"));dialog->resize(1100,900);
        auto* layout=new QVBoxLayout(dialog);
        auto help_text=QStringLiteral("Choose what happens to every previous room and current space. "
            "Names and classifications stay with retained identities. Choose new facts for new rooms. Pick an interior point for each assigned space.");
        if (selected_context) help_text+=QStringLiteral(" Retiring a room also removes its known phase and view memberships.");
        auto* help=new QLabel(help_text,dialog);
        help->setWordWrap(true);layout->addWidget(help);
        walls=choice(dialog,"physicalRoomReviewSource");walls->clear();
        if (!selected_context) {
            const auto organization=organize_project(source);const auto wall_context=organization.drawing_context(wall_id);
            for (const auto& [id,e]:source.entities()) if (e.type=="wall" && organization.drawing_context(id)==wall_context)
                walls->addItem(text(e.properties.value("name",id)),text(id));
            walls->setCurrentIndex(walls->findData(text(wall_id)));
        }
        layout->addWidget(walls);walls->setVisible(!selected_context.has_value());
        canvas=new PlanCanvas(dialog);canvas->setObjectName(QStringLiteral("physicalRoomReviewCanvas"));canvas->setMinimumHeight(270);
        canvas->setGridEnabled(false);canvas->setSnapEnabled(false);canvas->setOverviewMapEnabled(false);canvas->setSelectionTransformEnabled(false,false);layout->addWidget(canvas,1);
        tabs=new QTabWidget(dialog);tabs->setObjectName(QStringLiteral("physicalRoomReviewTabs"));
        auto* rooms=new QWidget(tabs);auto* room_layout=new QVBoxLayout(rooms);
        retained_table=table(rooms,"physicalRoomReviewRetained",{"Previous room / classification","Relationship","Decision","Chosen current space"});
        fresh_table=table(rooms,"physicalRoomReviewFresh",{"Current space","Assign","New name","New classification","Interior point","Context"});
        room_layout->addWidget(retained_table);room_layout->addWidget(fresh_table);tabs->addTab(rooms,QStringLiteral("Rooms"));
        auto* reference_page=new QWidget(tabs);auto* reference_layout=new QVBoxLayout(reference_page);
        auto* reference_help=new QLabel(QStringLiteral("Choose Keep or Remove for every attached reference. Map kept edges and corners explicitly. "
            "Kept room dimensions require current physical-source support; unavailable choices show a validation reason below."),reference_page);
        reference_help->setWordWrap(true);reference_layout->addWidget(reference_help);
        reference_table=table(reference_page,"physicalRoomReviewReferences",{"Attached reference","Decision","Validated preview"});
        mapping_table=table(reference_page,"physicalRoomReviewMappings",{"Previous edge / corner","Chosen edge / corner"});
        reference_layout->addWidget(reference_table);reference_layout->addWidget(mapping_table);tabs->addTab(reference_page,QStringLiteral("References"));
        graph_table=table(tabs,"physicalRoomReviewRelationships",{"Relationship model / exact row","Explicit removal acknowledgement"});
        tabs->addTab(graph_table,QStringLiteral("Room relationships"));layout->addWidget(tabs,2);
        status=new QLabel(dialog);status->setObjectName(QStringLiteral("physicalRoomReviewStatus"));status->setWordWrap(true);status->setTextFormat(Qt::PlainText);layout->addWidget(status);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,dialog);
        apply=buttons->button(QDialogButtonBox::Apply);apply->setObjectName(QStringLiteral("physicalRoomReviewApply"));
        apply->setText(!predecessor?QStringLiteral("Apply reviewed rooms"):
            selected_context?QStringLiteral("Apply wall deletion and reviewed rooms"):
            curve_proposal(predecessor)?QStringLiteral("Apply curve and reviewed rooms"):
            QStringLiteral("Apply wall edit and reviewed rooms"));layout->addWidget(buttons);
        QObject::connect(apply,&QPushButton::clicked,dialog,[this]{dialog->accept();});
        QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,[this]{dialog->reject();});
        QObject::connect(walls,&QComboBox::currentIndexChanged,dialog,[this]{reset();});
        canvas->setPointPlacementRequested([this](Vec2 point){pick(point);});
        auto* timer=new QTimer(dialog);timer->setInterval(100);
        QObject::connect(timer,&QTimer::timeout,dialog,[this]{if (dialog->isVisible() && !is_current()) fail(QStringLiteral("The project changed. Cancel and review the current source."));});timer->start();
        if (predecessor && is_physical_wall_room_deletion_review_command(*predecessor))
            set_deletion_consequences(original_source,std::get<ApplyEntityChanges>(*predecessor));
        reset();
    }
    void set_deletion_consequences(const DocumentSnapshot& original,const ApplyEntityChanges& deletion) {
        if (deletion_acknowledgement) throw std::invalid_argument("Wall deletion consequences are already captured.");
        if (!is_physical_wall_room_deletion_review_command(Command{deletion}))
            throw std::invalid_argument("The wall deletion consequence list has no original removal command.");
        auto* page=new QWidget(tabs);auto* layout=new QVBoxLayout(page);
        auto* help=new QLabel(QStringLiteral("The selected objects and attached items below will be removed together. "
            "Choose what happens to the remaining rooms on the Rooms tab."),page);
        help->setWordWrap(true);layout->addWidget(help);
        auto* removals=table(page,"physicalRoomReviewWallRemovals",{"Object","Removal"});
        for (const auto& change:deletion.entity_changes) {
            if (change.kind!=EntityChangeKind::erase) continue;
            const auto found=original.entities().find(change.entity_id);
            if (found==original.entities().end()) throw std::invalid_argument("An original wall-linked removal is missing.");
            const auto& entity=found->second;
            const auto category=entity.type=="wall"?QStringLiteral("Wall"):
                entity.type=="door"?QStringLiteral("Door"):entity.type=="window"?QStringLiteral("Window"):
                entity.type=="opening"?QStringLiteral("Wall opening"):entity.type=="roof"?QStringLiteral("Roof"):
                entity.type=="slab"?QStringLiteral("Floor or ceiling"):entity.type=="stair"?QStringLiteral("Stair"):
                entity.type=="railing"?QStringLiteral("Railing"):entity.type=="column"?QStringLiteral("Column"):
                entity.type=="beam"?QStringLiteral("Beam"):entity.type=="wall_join"?QStringLiteral("Wall join"):
                entity.type=="roof_join"?QStringLiteral("Roof join"):
                entity.type=="constraint"?QStringLiteral("Attached constraint"):
                entity.type=="boundary_dimension"?QStringLiteral("Saved dimension"):text(entity.type);
            const auto name=text(entity.properties.value("name",std::string{}));
            const auto row=removals->rowCount();removals->insertRow(row);
            removals->setItem(row,0,new QTableWidgetItem(name.isEmpty()?category:name));
            removals->setItem(row,1,new QTableWidgetItem(category));
        }
        for (const auto& change:deletion.entity_changes) {
            if (change.kind!=EntityChangeKind::upsert || change.entity.type!="assembly_model") continue;
            const auto found=original.entities().find(change.entity.id);
            if (found==original.entities().end() || found->second.type!="assembly_model")
                throw std::invalid_argument("An original component catalog is missing.");
            const auto& before=found->second.properties.at("model").at("instances");
            const auto& after=change.entity.properties.at("model").at("instances");
            if (!before.is_array() || !after.is_array())
                throw std::invalid_argument("Component catalogs require saved instance arrays.");
            std::set<std::string> remaining;
            for (const auto& component:after) remaining.insert(component.at("id").get<std::string>());
            for (const auto& component:before) {
                const auto id=component.at("id").get<std::string>();
                if (remaining.contains(id)) continue;
                const auto row=removals->rowCount();removals->insertRow(row);
                const auto name=component.value("name",id);
                auto* item=new QTableWidgetItem(text(name));
                item->setToolTip(text(found->first+" / "+id));
                removals->setItem(row,0,item);
                removals->setItem(row,1,new QTableWidgetItem(QStringLiteral("Placed component")));
            }
        }
        layout->addWidget(removals);
        deletion_acknowledgement=new QCheckBox(QStringLiteral("Remove these objects and attached references"),page);
        deletion_acknowledgement->setObjectName(QStringLiteral("physicalRoomReviewConfirmWallRemoval"));
        layout->addWidget(deletion_acknowledgement);tabs->addTab(page,QStringLiteral("Wall deletion"));
        QObject::connect(deletion_acknowledgement,&QCheckBox::toggled,dialog,[this]{update();});
        candidate.reset();candidate_snapshot.reset();accepted.reset();apply->setEnabled(false);
    }
    bool is_current() const {
        try { return !invalidated && current_source && original_source.is_editable() && document_snapshot_digest(current_source())==original_digest; }
        catch (...) { return false; }
    }
    bool selected_dimension(const std::string& id) const {
        return std::any_of(selected_dimension_placements.begin(),selected_dimension_placements.end(),
            [&](const auto& placement){return placement.dimension_id==id;});
    }
    void set_selected_dimension_placements(const DocumentSnapshot& original,
        const std::vector<PhysicalWallRoomDimensionPlacement>& placements) {
        require_current();
        if (dimension_source) throw std::invalid_argument("Selected dimension placements are already captured.");
        if (!report || original.document_id()!=original_source.document_id() || !original.is_editable())
            throw std::invalid_argument("Selected dimensions require the original editable project source.");
        if (predecessor && document_snapshot_digest(original)!=original_digest)
            throw std::invalid_argument("Selected dimensions do not match the captured wall-edit source.");
        validate_physical_wall_room_dimension_placements(original.entities(),placements);
        for (const auto& placement:placements) {
            const auto found=std::find_if(references.begin(),references.end(),[&](const auto& reference){
                return reference.id==placement.dimension_id && reference.dimension;
            });
            if (found==references.end() || source.entities().at(placement.dimension_id)!=original.entities().at(placement.dimension_id))
                throw std::invalid_argument("Selected dimensions must belong to affected rooms and match their original saved source.");
        }
        dimension_source=original;selected_dimension_placements=placements;
        // Reclassify in place so room assignments, graph acknowledgements and
        // unrelated reference choices survive the owner's pre-exec setter.
        rebuilding=true;
        for (std::size_t i=0;i<references.size();++i) {
            auto& reference=references[i];if (!selected_dimension(reference.id)) continue;
            const auto decoded=decode_boundary_dimension_entity(source.entities().at(reference.id));
            const auto& d=*decoded.dimension;reference.automatic_lengths=false;
            if (!reference.area_dimension) {
                reference.children.emplace(d.boundary_id,false,d.segment_id);
                for (const auto& child:d.segment_chain_ids) reference.children.emplace(d.boundary_id,false,child);
                if (d.kind==BoundaryDimensionKind::angle) {
                    reference.children.emplace(d.boundary_id,false,d.secondary_segment_id);
                    reference.children.emplace(d.boundary_id,true,d.vertex_id);
                }
            }
            reference_table->item(static_cast<int>(i),0)->setText(text(reference.id)+QStringLiteral(" · selected dimension placement"));
            reference.decision->setItemText(1,reference.area_dimension?QStringLiteral("Keep area and move callout"):QStringLiteral("Keep, map and move callout"));
            reference.decision->setCurrentIndex(0);
        }
        rebuilding=false;rebuild_mappings();update();
    }
    void require_current() const {
        if (!original_source.is_editable()) throw std::invalid_argument("This project is read-only: "+original_source.read_only_reason());
        if (!is_current()) throw std::invalid_argument("The complete project source changed. Cancel and start a new review.");
    }
    void fail(QString reason) {
        candidate.reset();candidate_snapshot.reset();accepted.reset();apply->setEnabled(false);error=std::move(reason);status->setText(error);
        if (!invalidated && !is_current()) {
            invalidated=true;const auto previous_rebuilding=rebuilding;rebuilding=true;
            report.reset();retained.clear();fresh.clear();references.clear();graphs.clear();mappings.clear();active.reset();
            for (auto* t:{retained_table,fresh_table,reference_table,mapping_table,graph_table}) t->setRowCount(0);
            rebuilding=previous_rebuilding;scene();
        }
    }
    void changed(bool topology=false) {
        if (rebuilding) return;
        if (topology) rebuild_mappings();update();
    }
    void reset() {
        rebuilding=true;report.reset();retained.clear();fresh.clear();references.clear();graphs.clear();mappings.clear();active.reset();
        candidate.reset();candidate_snapshot.reset();accepted.reset();
        for (auto* t:{retained_table,fresh_table,reference_table,mapping_table,graph_table}) t->setRowCount(0);
        try {
            require_current();
            if (selected_context) report=physical_wall_room_correspondence(source,*selected_context,*selected_elevation);
            else {
                if (walls->currentIndex()<0) throw std::invalid_argument("Choose a current physical source wall.");
                report=physical_wall_room_correspondence(source,value(walls));
            }
            for (const auto& old:report->retained) {
                const auto row=retained_table->rowCount();retained_table->insertRow(row);
                auto* label=new QTableWidgetItem(text(old.room.properties.value("name",old.room.id))+QStringLiteral(" · ")+text(old.room.properties.value("classification",std::string{})));
                label->setToolTip(text(old.room.id));retained_table->setItem(row,0,label);
                retained_table->setItem(row,1,new QTableWidgetItem(kind(old.kind)));retained_table->item(row,1)->setToolTip(text(old.diagnostic));
                auto* decision=choice(retained_table,QStringLiteral("retainedDecision:")+text(old.room.id));
                decision->addItem(QStringLiteral("Retain identity"),"retain");decision->addItem(QStringLiteral("Retire room"),"retire");
                retained_table->setCellWidget(row,2,decision);retained_table->setItem(row,3,new QTableWidgetItem);
                retained.emplace(old.room.id,decision);QObject::connect(decision,&QComboBox::currentIndexChanged,dialog,[this]{changed(true);});
            }
            for (const auto& space:report->fresh) {
                const auto row=fresh_table->rowCount();fresh_table->insertRow(row);FreshRow controls;
                fresh_table->setItem(row,0,new QTableWidgetItem(QStringLiteral("C%1 · %2 · %3").arg(space.index+1).arg(area(space.area_square_metres,metric),kind(space.kind))));
                controls.assignment=choice(fresh_table,QStringLiteral("freshAssignment:%1").arg(space.index));
                for (const auto& old:report->retained) controls.assignment->addItem(QStringLiteral("Retain %1").arg(text(old.room.properties.value("name",old.room.id))),QStringLiteral("retained:")+text(old.room.id));
                controls.assignment->addItem(QStringLiteral("Create new room"),"create");controls.assignment->addItem(QStringLiteral("Leave unclassified"),"unclassified");
                controls.name=new QLineEdit(fresh_table);controls.name->setObjectName(QStringLiteral("freshName:%1").arg(space.index));controls.name->setMaxLength(4096);
                controls.classification=new QLineEdit(fresh_table);controls.classification->setObjectName(QStringLiteral("freshClassification:%1").arg(space.index));controls.classification->setMaxLength(4096);
                controls.pick=new QPushButton(QStringLiteral("Pick inside C%1").arg(space.index+1),fresh_table);controls.pick->setObjectName(QStringLiteral("freshPick:%1").arg(space.index));
                controls.witness=new QTableWidgetItem;
                fresh_table->setCellWidget(row,1,controls.assignment);fresh_table->setCellWidget(row,2,controls.name);fresh_table->setCellWidget(row,3,controls.classification);
                auto* witness_widget=new QWidget(fresh_table);auto* witness_layout=new QVBoxLayout(witness_widget);witness_layout->setContentsMargins(0,0,0,0);witness_layout->addWidget(controls.pick);
                fresh_table->setCellWidget(row,4,witness_widget);fresh_table->setItem(row,4,controls.witness);
                const auto& c=report->context;
                const auto context_name=[this](const std::string& id) {
                    const auto found=source.entities().find(id);return found==source.entities().end()?text(id):text(found->second.properties.value("name",id));
                };
                auto* context_item=new QTableWidgetItem(context_name(c.building_id)+" / "+context_name(c.floor_id)+" / "+context_name(c.layer_id));
                context_item->setToolTip(QStringLiteral("Property: %1\nBuilding: %2\nFloor: %3\nLayer: %4\nLevel: %5\nEffective elevation: %6 m")
                    .arg(context_name(c.property_id),context_name(c.building_id),context_name(c.floor_id),context_name(c.layer_id),
                        c.level_id.empty()?QStringLiteral("Unbound"):context_name(c.level_id)).arg(report->effective_elevation_m,0,'g',12));
                fresh_table->setItem(row,5,context_item);
                fresh.push_back(controls);
                QObject::connect(controls.assignment,&QComboBox::currentIndexChanged,dialog,[this,index=space.index]{
                    if (rebuilding) return;
                    auto& f=fresh.at(index);f.ids={};f.dimension_ids.clear();f.allocated_room_id.clear();f.point.reset();changed(true);
                });
                for (auto* edit:{controls.name,controls.classification}) QObject::connect(edit,&QLineEdit::textChanged,dialog,[this]{changed();});
                QObject::connect(controls.pick,&QPushButton::clicked,dialog,[this,index=space.index]{
                    try {require_current();active=index;scene();error.clear();status->setText(QStringLiteral("Click strictly inside C%1, outside holes and wall material.").arg(index+1));}
                    catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));}
                });
            }
            inventory();rebuilding=false;rebuild_mappings();update();scene();canvas->fitView();
        } catch (const std::exception& e) {rebuilding=false;fail(QString::fromUtf8(e.what()));scene();}
    }
    void inventory() {
        const auto constraint_scope=source.uses_active_phase_constraints()
            ? std::optional<ConstraintPhaseScope>{constraint_phase_scope(source.entities())} : std::nullopt;
        for (const auto& [id,e]:source.entities()) {
            Reference reference;reference.id=id;QString label;
            if (can_recognize_boundary_dimension_entity_type(e.type)) {
                const auto decoded=decode_boundary_dimension_entity(e);if (!decoded.supported()) continue;
                const auto& d=*decoded.dimension;if (!retained.contains(d.boundary_id)) continue;
                reference.dimension=true;reference.owners.insert(d.boundary_id);label=QStringLiteral("%1 · %2 dimension").arg(text(id),text(std::string(boundary_dimension_kind_name(d.kind))));
                reference.area_dimension=d.kind==BoundaryDimensionKind::area;
                reference.automatic_lengths=d.kind==BoundaryDimensionKind::segment_length && d.placement==BoundaryDimensionPlacement::automatic && !selected_dimension(id);
                if (reference.automatic_lengths) label+=QStringLiteral(" · regenerate all new edge lengths");
                if (!reference.area_dimension && !reference.automatic_lengths) {
                    reference.children.emplace(d.boundary_id,false,d.segment_id);
                    for (const auto& child:d.segment_chain_ids) reference.children.emplace(d.boundary_id,false,child);
                    if (d.kind==BoundaryDimensionKind::angle) {reference.children.emplace(d.boundary_id,false,d.secondary_segment_id);reference.children.emplace(d.boundary_id,true,d.vertex_id);}
                }
            } else if (e.type=="constraint") {
                const auto decoded=decode_constraint_entity(e);if (!decoded.supported()) continue;
                reference.preserved_constraint=constraint_scope && !constraint_participates(*decoded.constraint,*constraint_scope);
                label=text(id)+QStringLiteral(" · ")+text(std::string(constraint_relation_name(decoded.constraint->relation)));
                for (const auto& b:decoded.constraint->bindings) if (retained.contains(b.owner_id)) {
                    reference.owners.insert(b.owner_id);reference.children.emplace(b.owner_id,false,b.segment_id);reference.children.emplace(b.owner_id,true,b.vertex_id);
                }
                if (reference.owners.empty()) continue;
            } else if (e.type=="room_relationships") {
                const auto& json=e.properties.at("model");if (room_relationship_model_version(json)>2) continue;
                auto model=RoomRelationshipSnapshot::from_json(json);
                if (std::none_of(model.references().begin(),model.references().end(),[this](const auto& r){return retained.contains(r.id);})) continue;
                Graph graph{id,std::move(model),{}, {}};
                for (const auto& r:graph.model.references()) if (retained.contains(r.id)) {
                    auto* check=graph_row(text(id)+QStringLiteral(" · membership ")+text(r.id),QStringLiteral("graphMembership:")+text(id)+":"+text(r.id));
                    graph.memberships.emplace(r.id,check);
                }
                for (std::size_t i=0;i<graph.model.relations().size();++i) {
                    const auto& r=graph.model.relations()[i];
                    graph.relations.push_back(graph_row(text(id)+QStringLiteral(" · ")+text(r.source_id)+" "+relation_name(r.kind)+" "+text(r.target_id),QStringLiteral("graphRelation:")+text(id)+":"+QString::number(i)));
                }
                graphs.push_back(std::move(graph));continue;
            } else continue;
            const auto row=reference_table->rowCount();reference_table->insertRow(row);reference_table->setItem(row,0,new QTableWidgetItem(label));
            reference_table->setItem(row,2,new QTableWidgetItem);
            reference.decision=choice(reference_table,QStringLiteral("referenceDecision:")+text(id));
            reference.decision->addItem(reference.preserved_constraint?QStringLiteral("Keep unchanged"):
                reference.automatic_lengths?QStringLiteral("Regenerate on new edges"):
                reference.area_dimension?QStringLiteral("Keep area"):QStringLiteral("Keep and map"),"keep");
            if (reference.preserved_constraint) {
                reference.decision->setCurrentIndex(1);reference.decision->setEnabled(false);
                reference.decision->setToolTip(QStringLiteral("This constraint also belongs to an inactive design. Its saved endpoints must stay unchanged."));
            } else reference.decision->addItem(QStringLiteral("Remove"),"remove");
            reference_table->setCellWidget(row,1,reference.decision);QObject::connect(reference.decision,&QComboBox::currentIndexChanged,dialog,[this]{changed(true);});references.push_back(std::move(reference));
        }
    }
    QCheckBox* graph_row(const QString& label,const QString& name) {
        const auto row=graph_table->rowCount();graph_table->insertRow(row);graph_table->setItem(row,0,new QTableWidgetItem(label));
        auto* check=new QCheckBox(QStringLiteral("Remove shown row"),graph_table);check->setObjectName(name);graph_table->setCellWidget(row,1,check);
        QObject::connect(check,&QCheckBox::toggled,dialog,[this]{changed();});return check;
    }
    std::map<std::string,std::size_t,std::less<>> assignments() const {
        std::map<std::string,std::size_t,std::less<>> result;
        for (std::size_t i=0;i<fresh.size();++i) {
            const auto choice=value(fresh[i].assignment);
            if (choice.starts_with("retained:") && !result.emplace(choice.substr(9),i).second)
                throw std::invalid_argument("A retained room identity can be assigned to only one current space.");
        }
        return result;
    }
    void allocate(std::size_t index) {
        auto& f=fresh[index];const auto action=value(f.assignment);
        if (action.empty() || action=="unclassified") return;
        if (f.ids.segment_ids.empty()) for (std::size_t i=0;i<report->fresh[index].boundary.size();++i) {
            f.ids.segment_ids.push_back("segment-"+make_stable_id());f.ids.vertex_ids.push_back("vertex-"+make_stable_id());
            f.dimension_ids.push_back("dimension-"+make_stable_id());
        }
        if (action=="create" && f.allocated_room_id.empty()) f.allocated_room_id="physical-room-"+make_stable_id();
    }
    void rebuild_mappings() {
        if (!report) return;
        std::map<Child,QString> previous;for (const auto& [key,c]:mappings) previous.emplace(key,c->currentData().toString());
        rebuilding=true;mapping_table->setRowCount(0);mappings.clear();
        try {
            require_current();for (std::size_t i=0;i<fresh.size();++i) allocate(i);
            const auto assigned=assignments();std::set<Child> required;
            for (const auto& r:references) if (value(r.decision)=="keep") required.insert(r.children.begin(),r.children.end());
            for (const auto& child:required) {
                const auto& [owner,vertex,old_id]=child;const auto row=mapping_table->rowCount();mapping_table->insertRow(row);
                const auto original=decode_identified_boundary_entity(source.entities().at(owner));
                auto old=std::find_if(original.segments.begin(),original.segments.end(),[&](const auto& edge){return vertex?edge.start_vertex_id==old_id:edge.segment_id==old_id;});
                mapping_table->setItem(row,0,new QTableWidgetItem(text(owner)+QStringLiteral(" · %1%2").arg(vertex?"V":"E").arg(old==original.segments.end()?0:std::distance(original.segments.begin(),old)+1)));
                auto* combo=choice(mapping_table,QStringLiteral("mapping:")+text(owner)+(vertex?":vertex:":":segment:")+text(old_id));
                if (assigned.contains(owner)) {
                    const auto& ids=fresh[assigned.at(owner)].ids;const auto& options=vertex?ids.vertex_ids:ids.segment_ids;
                    for (std::size_t i=0;i<options.size();++i) combo->addItem(QStringLiteral("%1%2").arg(vertex?"V":"E").arg(i+1),text(options[i]));
                }
                if (previous.contains(child)) combo->setCurrentIndex(std::max(0,combo->findData(previous.at(child))));
                mapping_table->setCellWidget(row,1,combo);mappings.emplace(child,combo);
                QObject::connect(combo,&QComboBox::currentIndexChanged,dialog,[this]{changed();});
            }
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));}
        rebuilding=false;
    }
    void pick(Vec2 point) {
        try {
            require_current();if (!report || !active) throw std::invalid_argument("Choose Pick inside for the intended current space first.");
            auto& f=fresh.at(*active);const auto action=value(f.assignment);
            if (action.empty() || action=="unclassified") throw std::invalid_argument("Assign this current space before choosing its interior point.");
            const auto& s=report->fresh.at(*active);
            f.point.reset();
            if (!PlanCanvas::containsAreaPoint(s.boundary,point) || std::any_of(s.holes.begin(),s.holes.end(),[&](const auto& h){return PlanCanvas::containsAreaPoint(h,point);}))
                throw std::invalid_argument("Click inside the chosen clear space, outside wall material and holes.");
            f.point=point;f.pick->setToolTip(QStringLiteral("Interior witness: %1, %2 m").arg(point.x,0,'g',12).arg(point.y,0,'g',12));update();scene();
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));scene();}
    }
    void graph_requirement(QCheckBox* check,bool needed) {
        check->setProperty("required",needed);check->setEnabled(needed);
        if (!needed) {QSignalBlocker blocker(check);check->setChecked(false);}
    }
    PhysicalWallRoomReviewIntent intent() {
        require_current();if (!report) throw std::invalid_argument("Current room detection is unavailable.");
        PhysicalWallRoomReviewIntent result;result.selected_wall_id=report->selected_wall_id;result.source_snapshot_digest=source_digest;
        result.selected_dimension_placements=selected_dimension_placements;
        result.context_plane_selection=report->context_plane_selection;
        result.active_phase_room_scope=report->active_phase_room_scope;
        result.source_entities_digest=entity_map_digest(source.entities());result.context=report->context;result.effective_elevation_m=report->effective_elevation_m;
        result.source_authoring_digest=document_authoring_source_digest_v2(source);result.source_saved_revision=source.saved_revision_optional();
        const auto assigned=assignments();std::set<std::string> retiring;
        for (std::size_t i=0;i<report->retained.size();++i) {
            const auto& old=report->retained[i];const auto action=value(retained.at(old.room.id));
            if (action.empty()) throw std::invalid_argument("Choose Retain or Retire for every previous room.");
            PhysicalWallRoomRetainedDecision decision;decision.room_id=old.room.id;decision.expected_descriptor_digest=old.descriptor_digest;
            if (action=="retire") {decision.disposition=PhysicalWallRoomRetainedDisposition::retire;retiring.insert(old.room.id);}
            else if (!assigned.contains(old.room.id)) throw std::invalid_argument("Assign a current space to every retained identity.");
            retained_table->item(static_cast<int>(i),3)->setText(assigned.contains(old.room.id)?QStringLiteral("C%1").arg(assigned.at(old.room.id)+1):QString{});
            result.retained.push_back(std::move(decision));
        }
        for (std::size_t i=0;i<fresh.size();++i) {
            allocate(i);auto& row=fresh[i];const auto action=value(row.assignment);
            if (action.empty()) throw std::invalid_argument("Choose a disposition for every current space.");
            PhysicalWallRoomFreshDecision d;d.candidate_index=i;d.reviewed_source_lineage=report->fresh[i].source_lineage;
            if (action!="unclassified") {
                if (!row.point) throw std::invalid_argument("Pick an interior point for every assigned current space.");
                d.interior_witness=*row.point;d.fresh_ids=row.ids;
                if (action=="create") {d.disposition=PhysicalWallRoomFreshDisposition::create;d.room_id=row.allocated_room_id;
                    d.name=row.name->text().trimmed().toStdString();d.classification=row.classification->text().trimmed().toStdString();d.context=report->context;}
                else {d.disposition=PhysicalWallRoomFreshDisposition::retained;d.room_id=action.substr(9);}
            }
            result.fresh.push_back(std::move(d));
        }
        for (const auto& r:references) {
            const auto action=value(r.decision);if (action.empty()) throw std::invalid_argument("Choose Keep or Remove for every attached reference.");
            if (selected_dimension(r.id) && action!="keep")
                throw std::invalid_argument("A selected dimension placement requires explicit Keep; it cannot be removed.");
            if (selected_dimension(r.id) && std::any_of(r.owners.begin(),r.owners.end(),[&](const auto& owner){return retiring.contains(owner);}))
                throw std::invalid_argument("A room owning a selected dimension placement must retain its identity.");
            if (action=="remove") {result.removed_reference_ids.push_back(r.id);continue;}
            if (std::any_of(r.owners.begin(),r.owners.end(),[&](const auto& owner){return retiring.contains(owner);}))
                throw std::invalid_argument("References attached to a retired room require explicit removal.");
            result.kept_reference_ids.push_back(r.id);
            if (r.automatic_lengths) for (const auto& owner:r.owners) {
                auto decision=std::find_if(result.retained.begin(),result.retained.end(),[&](const auto& d){return d.room_id==owner;});
                if (!assigned.contains(owner)) throw std::invalid_argument("Assign a destination before regenerating its edge dimensions.");
                decision->replacement_dimension_ids=fresh.at(assigned.at(owner)).dimension_ids;
            }
            for (const auto& child:r.children) {
                const auto& [owner,vertex,old_id]=child;const auto found=mappings.find(child);
                if (found==mappings.end() || found->second->currentIndex()==0) throw std::invalid_argument("Map every required kept edge and corner.");
                auto decision=std::find_if(result.retained.begin(),result.retained.end(),[&](const auto& d){return d.room_id==owner;});
                if (decision->child_mapping.empty()) decision->child_mapping={{"segments",nlohmann::json::object()},{"vertices",nlohmann::json::object()}};
                decision->child_mapping[vertex?"vertices":"segments"][old_id]=value(found->second);
            }
        }
        for (auto& graph:graphs) {
            PhysicalWallRoomRelationshipRemoval d;d.entity_id=graph.id;
            for (auto& [owner,check]:graph.memberships) if (retiring.contains(owner)) {
                if (!check->isChecked()) throw std::invalid_argument("Acknowledge every removed room-relationship membership and incident row.");d.removed_room_ids.push_back(owner);
            }
            for (std::size_t i=0;i<graph.relations.size();++i) {
                const auto& relation=graph.model.relations()[i];if (!retiring.contains(relation.source_id) && !retiring.contains(relation.target_id)) continue;
                if (!graph.relations[i]->isChecked()) throw std::invalid_argument("Acknowledge every removed room-relationship membership and incident row.");d.acknowledged_relations.push_back(relation);
            }
            if (!d.removed_room_ids.empty()) result.relationship_removals.push_back(std::move(d));
        }
        return result;
    }
    void update() {
        candidate.reset();candidate_snapshot.reset();accepted.reset();apply->setEnabled(false);
        for (int row=0;row<reference_table->rowCount();++row) reference_table->item(row,2)->setText({});
        std::set<std::string> retiring;for (const auto& [id,c]:retained) if (value(c)=="retire") retiring.insert(id);
        for (auto& g:graphs) {
            for (auto& [id,check]:g.memberships) graph_requirement(check,retiring.contains(id));
            for (std::size_t i=0;i<g.relations.size();++i) {const auto& r=g.model.relations()[i];graph_requirement(g.relations[i],retiring.contains(r.source_id)||retiring.contains(r.target_id));}
        }
        for (auto& f:fresh) {
            const auto action=value(f.assignment);const bool create=action=="create";f.name->setEnabled(create);f.classification->setEnabled(create);
            f.pick->setEnabled(!action.empty() && action!="unclassified");f.pick->setText(f.point?QStringLiteral("Interior chosen · pick again"):QStringLiteral("Pick inside"));
        }
        try {
            if (deletion_acknowledgement && !deletion_acknowledgement->isChecked())
                throw std::invalid_argument("Review the Wall deletion tab and confirm its listed removals.");
            const auto decisions=intent();
            ApplyBoundaryConstraintChanges command;
            auto exact=[&]() {
                if (predecessor) {
                    auto prepared=prepare_physical_wall_room_review_after_geometry(original_source,*predecessor,*report,decisions);
                    command=std::move(prepared.command);return std::move(prepared.snapshot);
                }
                const auto prepared=prepare_physical_wall_room_review(source,*report,decisions,dimension_source?&*dimension_source:nullptr);
                command.expected_revision=source.revision();command.message="Review physical rooms";
                command.room_review_completion=true;command.room_review_intent=prepared.intent;
                auto reviewed=Document::preview_command(source,command);
                if (reviewed.entities()!=prepared.entities || reviewed.assets()!=source.assets())
                    throw std::invalid_argument("The complete room preview differs from the prepared decisions.");
                return reviewed;
            }();
            for (std::size_t i=0;i<references.size();++i) {
                const auto& reference=references[i];QString description=QStringLiteral("Removed together on Apply");
                if (value(reference.decision)=="keep") {
                    if (reference.automatic_lengths) {
                        std::size_t count=0;for (const auto& retained_decision:decisions.retained) if (reference.owners.contains(retained_decision.room_id))
                            for (const auto& id:retained_decision.replacement_dimension_ids) {
                                const auto dimension=decode_boundary_dimension_entity(exact.entities().at(id));
                                if (!dimension.supported()) throw std::invalid_argument("A regenerated edge dimension cannot be resolved.");
                                (void)resolve_boundary_dimension(*dimension.dimension,exact);++count;
                            }
                        description=QStringLiteral("Regenerate %1 new edge dimensions").arg(count);
                    } else if (reference.dimension) {
                        const auto dimension=decode_boundary_dimension_entity(exact.entities().at(reference.id));
                        if (!dimension.supported()) throw std::invalid_argument("The kept dimension cannot be resolved.");
                        description=quantity(resolve_boundary_dimension(*dimension.dimension,exact),metric);
                        if (reference.area_dimension) description+=QStringLiteral(" · net clear area");
                    } else description=reference.preserved_constraint?QStringLiteral("Keep saved endpoints unchanged"):
                        QStringLiteral("Keep on explicitly mapped edges / corners");
                }
                reference_table->item(static_cast<int>(i),2)->setText(description);
            }
            require_current();candidate=std::move(command);candidate_snapshot=std::move(exact);error.clear();
            status->clear();apply->setEnabled(true);
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));}
        scene();
    }
    void scene() {
        std::vector<CanvasEntity> entities;std::vector<CanvasLabel> labels;
        if (report) {
            for (const auto& old:report->retained) if (!old.boundary.empty()) {
                CanvasEntity e{text(old.room.id),QStringLiteral("boundary"),old.boundary,0,false};e.holes=old.holes;e.stroke_color=QColor(130,143,158);e.dashed_stroke=true;entities.push_back(std::move(e));
                if (active && *active<fresh.size() && value(fresh[*active].assignment)=="retained:"+old.room.id)
                    for (std::size_t j=0;j<old.boundary.size();++j) {
                        const auto& edge=old.boundary[j];labels.push_back({{},edge.start,QStringLiteral("Old V%1").arg(j+1)});
                        labels.push_back({{},{(edge.start.x+edge.end.x)/2,(edge.start.y+edge.end.y)/2},QStringLiteral("Old E%1").arg(j+1)});
                    }
            }
            for (std::size_t i=0;i<report->fresh.size();++i) {
                const auto& s=report->fresh[i];CanvasEntity e{QStringLiteral("candidate:%1").arg(i),QStringLiteral("boundary"),s.boundary,0,false};
                e.holes=s.holes;e.stroke_color=QColor(36,107,206);e.filled=active && *active==i;e.fill_color=QColor(36,107,206,35);entities.push_back(std::move(e));
                if (!s.boundary.empty()) labels.push_back({{},s.boundary.front().start,QStringLiteral("C%1").arg(i+1)});
                if (active && *active==i) for (std::size_t j=0;j<s.boundary.size();++j) {
                    const auto& edge=s.boundary[j];labels.push_back({{},edge.start,QStringLiteral("V%1").arg(j+1)});
                    labels.push_back({{},{(edge.start.x+edge.end.x)/2,(edge.start.y+edge.end.y)/2},QStringLiteral("E%1").arg(j+1)});
                }
            }
        }
        if (candidate_snapshot) {
            std::set<std::string> dimension_ids;
            for (const auto& reference:references) if (reference.dimension && value(reference.decision)=="keep" && !reference.automatic_lengths) dimension_ids.insert(reference.id);
            for (const auto& f:fresh) for (const auto& id:f.dimension_ids) if (candidate_snapshot->entities().contains(id)) dimension_ids.insert(id);
            for (const auto& id:dimension_ids) {
                const auto decoded=decode_boundary_dimension_entity(candidate_snapshot->entities().at(id));
                if (!decoded.supported()) continue;
                const auto& dimension=*decoded.dimension;
                labels.push_back({text(id),dimension.text_position,quantity(resolve_boundary_dimension(dimension,*candidate_snapshot),metric)});
            }
        }
        canvas->setEntities(std::move(entities));canvas->setLabels(std::move(labels));
    }
    bool submit() {
        update();if (!candidate || !candidate_snapshot) return false;
        try {
            require_current();const auto exact=Document::preview_command(original_source,*candidate);
            if (exact.entities()!=candidate_snapshot->entities() || exact.assets()!=candidate_snapshot->assets()) throw std::invalid_argument("The accepted room preview changed. Review it again.");
            require_current();accepted=candidate;return true;
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));return false;}
    }
};

PhysicalWallRoomReviewDialog::PhysicalWallRoomReviewDialog(DocumentSnapshot source,std::string selected_wall_id,bool metric_units,
    std::function<DocumentSnapshot()> current_source,QWidget* parent,std::optional<Command> predecessor):QDialog(parent),
    m_impl(std::make_unique<Impl>(this,std::move(source),std::move(selected_wall_id),metric_units,std::move(current_source),std::move(predecessor))) {}
PhysicalWallRoomReviewDialog::PhysicalWallRoomReviewDialog(DocumentSnapshot source,DrawingContext context,double effective_elevation_m,
    bool metric_units,std::function<DocumentSnapshot()> current_source,QWidget* parent,std::optional<Command> predecessor):QDialog(parent),
    m_impl(std::make_unique<Impl>(this,std::move(source),std::string{},metric_units,std::move(current_source),std::move(predecessor),
        std::move(context),effective_elevation_m)) {}
PhysicalWallRoomReviewDialog::~PhysicalWallRoomReviewDialog()=default;
const std::optional<ApplyBoundaryConstraintChanges>& PhysicalWallRoomReviewDialog::acceptedCommand() const {return m_impl->accepted;}
QString PhysicalWallRoomReviewDialog::lastError() const {return m_impl->error;}
void PhysicalWallRoomReviewDialog::setDeletionConsequences(const DocumentSnapshot& original,const ApplyEntityChanges& deletion) {
    m_impl->set_deletion_consequences(original,deletion);
}
void PhysicalWallRoomReviewDialog::setSelectedDimensionPlacements(const DocumentSnapshot& original,
    const std::vector<PhysicalWallRoomDimensionPlacement>& placements) {
    m_impl->set_selected_dimension_placements(original,placements);
}
void PhysicalWallRoomReviewDialog::accept() {if (m_impl->submit()) QDialog::accept();}
void PhysicalWallRoomReviewDialog::reject() {m_impl->accepted.reset();m_impl->candidate.reset();m_impl->candidate_snapshot.reset();QDialog::reject();}
} // namespace sketch::desktop

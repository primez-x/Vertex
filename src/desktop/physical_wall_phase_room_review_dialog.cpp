#include "sketch/desktop/physical_wall_phase_room_review_dialog.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/physical_wall_phase_review.hpp"
#include "sketch/physical_wall_spaces.hpp"
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
#include <set>
#include <stdexcept>
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
        std::string allocated_room_id;LegacyBoundaryIdentityOptions ids;std::optional<Vec2> point;
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
    PhysicalWallPhaseRoomReviewDialog* dialog;
    DocumentSnapshot source;
    ApplyEntityChanges registry_command;
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
    QComboBox* plane_choice{};PlanCanvas* canvas{};QStackedWidget* pages{};QTableWidget* reference_table{};
    QTabWidget* tabs{};QLabel* status{};QLabel* plane_status{};QPushButton* apply{};
    std::optional<std::pair<std::size_t,std::size_t>> active_pick;
    std::optional<ApplyBoundaryConstraintChanges> candidate,accepted;
    std::optional<DocumentSnapshot> candidate_snapshot;
    QString error;

    Impl(PhysicalWallPhaseRoomReviewDialog* owner,DocumentSnapshot captured,ApplyEntityChanges command,
        PhysicalWallPhaseSelection target,bool metric_units,std::function<DocumentSnapshot()> current):
        dialog(owner),source(std::move(captured)),registry_command(std::move(command)),destination(std::move(target)),
        current_source(std::move(current)),source_digest(document_snapshot_digest(source)),metric(metric_units) {
        dialog->setObjectName(QStringLiteral("physicalPhaseRoomReviewDialog"));
        dialog->setWindowTitle(destination.alternative_id?QStringLiteral("Review rooms in the alternative"):QStringLiteral("Review baseline rooms"));dialog->resize(1180,900);
        auto* layout=new QVBoxLayout(dialog);
        auto* help=new QLabel(QStringLiteral("Review every previous room and current clear space on each listed floor. "
            "Unchanged rooms can be shared. Changed baseline rooms stay preserved while you choose rooms for this alternative. "
            "Enter new room facts explicitly; attached references require separate confirmation."),dialog);
        help->setWordWrap(true);layout->addWidget(help);
        auto* limitation=new QLabel(QStringLiteral("References are preserved in the baseline; they are not copied into this alternative. "
            "Changing or retiring an existing proposed room requires a separate review and cannot be applied here."),dialog);
        limitation->setWordWrap(true);limitation->setObjectName(QStringLiteral("physicalPhaseRoomReviewLimitations"));layout->addWidget(limitation);
        plane_choice=new QComboBox(dialog);plane_choice->setObjectName(QStringLiteral("physicalPhaseRoomReviewPlane"));layout->addWidget(plane_choice);
        plane_status=new QLabel(dialog);plane_status->setWordWrap(true);plane_status->setTextFormat(Qt::PlainText);layout->addWidget(plane_status);
        canvas=new PlanCanvas(dialog);canvas->setObjectName(QStringLiteral("physicalPhaseRoomReviewCanvas"));canvas->setMinimumHeight(270);
        canvas->setGridEnabled(false);canvas->setSnapEnabled(false);canvas->setOverviewMapEnabled(false);
        canvas->setSelectionTransformEnabled(false,false);
        canvas->setPointPlacementRequested([this](Vec2 point){if (active_pick) pick(point);});layout->addWidget(canvas,1);
        auto* legend=new QLabel(QStringLiteral("Blue: current clear spaces · Dashed gray: previous room outlines · ●: chosen interior point"),dialog);
        legend->setWordWrap(true);layout->addWidget(legend);
        tabs=new QTabWidget(dialog);tabs->setObjectName(QStringLiteral("physicalPhaseRoomReviewTabs"));
        pages=new QStackedWidget(tabs);tabs->addTab(pages,QStringLiteral("Rooms"));
        auto* reference_page=new QWidget(tabs);auto* reference_layout=new QVBoxLayout(reference_page);
        auto* reference_help=new QLabel(QStringLiteral("Confirm each listed reference remains with its original baseline room. "
            "Saved dimensions, relationships and other attached data are included. No reference is copied or reassigned."),reference_page);
        reference_help->setWordWrap(true);reference_layout->addWidget(reference_help);
        reference_table=table(reference_page,QStringLiteral("physicalPhaseRoomReviewReferences"),
            {QStringLiteral("Attached reference"),QStringLiteral("Touches previous room / edge / corner"),QStringLiteral("Confirmation")});
        reference_layout->addWidget(reference_table);tabs->addTab(reference_page,QStringLiteral("Baseline references"));layout->addWidget(tabs,2);
        status=new QLabel(dialog);status->setObjectName(QStringLiteral("physicalPhaseRoomReviewStatus"));
        status->setWordWrap(true);status->setTextFormat(Qt::PlainText);layout->addWidget(status);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,dialog);
        apply=buttons->button(QDialogButtonBox::Apply);apply->setObjectName(QStringLiteral("physicalPhaseRoomReviewApply"));
        apply->setText(destination.alternative_id?QStringLiteral("Apply alternative and reviewed rooms"):QStringLiteral("Apply baseline selection"));
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
        candidate.reset();candidate_snapshot.reset();accepted.reset();apply->setEnabled(false);status->setText(reason);error=reason;
    }
    void invalidate() {
        invalidated=true;active_pick.reset();canvas->setEnabled(false);canvas->setEntities({});canvas->setLabels({});
        plane_choice->setEnabled(false);pages->setEnabled(false);reference_table->setEnabled(false);
        fail(QStringLiteral("The project or editing permissions changed. Cancel and start a new review."));
    }
    QString entity_name(const std::string& id) const {
        const auto found=source.entities().find(id);
        if (found==source.entities().end()) return text(id);
        const auto name=text(property_text(found->second,"name"));
        return name.isEmpty()?text(id):name;
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
            require_current();inventory=inspect_physical_wall_phase_room_review(source,registry_command,destination);
            if (inventory->reports.size()!=inventory->intent.planes.size()) throw std::invalid_argument("The room review is incomplete. Cancel and start again.");
            auto target_entities=source.entities();
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
                    controls.previous_table->setItem(row,1,new QTableWidgetItem(relationship(old.kind)));
                    auto* decision=choice(controls.previous_table,QStringLiteral("phasePreviousDecision:")+text(old.room.id));
                    option(decision,QStringLiteral("Share unchanged"),QStringLiteral("share"),!matches.empty(),QStringLiteral("Sharing requires the same walls and exact clear outline."));
                    const bool supersede=destination.alternative_id.has_value() && baseline_rooms.contains(old.room.id);
                    option(decision,QStringLiteral("Preserve baseline and replace in alternative"),QStringLiteral("supersede"),supersede,
                        QStringLiteral("Only a baseline room can be replaced in this alternative."));
                    controls.previous_table->setCellWidget(row,2,decision);controls.previous.emplace(old.room.id,decision);
                    const auto note=!matches.empty()?QStringLiteral("Exact unchanged match available"):
                        supersede?QStringLiteral("Choose what replaces this baseline room"):
                        proposed_rooms.contains(old.room.id)?QStringLiteral("Changed proposed room: cannot replace or retire here"):
                        QStringLiteral("This review can only share this room unchanged");
                    auto* note_item=new QTableWidgetItem(note);note_item->setToolTip(note);controls.previous_table->setItem(row,3,note_item);
                    QObject::connect(decision,&QComboBox::currentIndexChanged,dialog,[this]{changed();});
                }
                for (std::size_t i=0;i<report.fresh.size();++i) {
                    const auto& fresh=report.fresh[i];if (fresh.index!=i) throw std::invalid_argument("The current room list changed. Cancel and start again.");
                    const auto row=controls.fresh_table->rowCount();controls.fresh_table->insertRow(row);FreshRow f;
                    controls.fresh_table->setItem(row,0,new QTableWidgetItem(QStringLiteral("C%1 · %2 · %3").arg(i+1).arg(area(fresh.area_square_metres,metric),relationship(fresh.kind))));
                    f.assignment=choice(controls.fresh_table,QStringLiteral("phaseFreshAssignment:%1:%2").arg(p).arg(i));
                    for (const auto& old:report.retained) if (controls.matching.at(old.room.id).contains(i))
                        f.assignment->addItem(QStringLiteral("Share %1 unchanged").arg(entity_name(old.room.id)),QStringLiteral("share:")+text(old.room.id));
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
                        auto& f=planes.at(p).fresh.at(i);f.point.reset();f.ids={};f.allocated_room_id.clear();
                        active_pick.reset();changed();
                    });
                    for (auto* edit:{f.name,f.classification,f.factor}) QObject::connect(edit,&QLineEdit::textChanged,dialog,[this]{changed();});
                    QObject::connect(f.pick,&QPushButton::clicked,dialog,[this,p,i]{begin_pick(p,i);});
                }
                planes.push_back(std::move(controls));
            }
            if (planes.empty()) {plane_choice->addItem(QStringLiteral("No affected room planes"));plane_choice->setEnabled(false);}
            rebuilding=false;changed();scene();canvas->fitView();
        } catch (const std::exception&) {
            rebuilding=false;inventory.reset();planes.clear();pages->setEnabled(false);plane_choice->setEnabled(false);canvas->setEnabled(false);
            fail(QStringLiteral("Room review is unavailable for this alternative. Cancel and review its existing rooms and wall choices."));scene();
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
        try {evidence=physical_wall_phase_room_baseline_dependents(source,destination.registry_id,selected);}
        catch (const std::exception&) {throw std::invalid_argument("The baseline reference list is unavailable. Cancel and review the original rooms.");}
        std::map<std::string,PhysicalWallRoomPhaseBaselineAcknowledgement,std::less<>> confirmed;
        for (const auto& old:references) if (old.preserve->isChecked()) confirmed.emplace(old.evidence.entity_id,old.evidence);
        rebuilding=true;
        references.clear();reference_table->setRowCount(0);
        for (const auto& reference:evidence) {
            const auto row=reference_table->rowCount();reference_table->insertRow(row);
            const auto& entity=source.entities().at(reference.entity_id);
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
            auto* check=new QCheckBox(QStringLiteral("Preserve in baseline"),reference_table);
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
    PhysicalWallRoomPhaseReviewIntent intent() {
        require_current();if (!inventory) throw std::invalid_argument("The room review is unavailable. Cancel and start again.");
        auto result=inventory->intent;result.baseline_only_acknowledgements.clear();
        for (std::size_t p=0;p<planes.size();++p) {
            auto& review=result.planes.at(p);review.source_rooms.clear();review.fresh.clear();
            const auto& report=inventory->reports.at(p).correspondence;auto& controls=planes[p];
            std::set<std::string> assigned;
            for (const auto& old:report.retained) {
                const auto action=value(controls.previous.at(old.room.id));
                if (action.empty()) {
                    if (proposed_rooms.contains(old.room.id) && controls.matching.at(old.room.id).empty())
                        throw std::invalid_argument("A changed proposed room cannot be replaced or retired here. Cancel and review that proposal separately.");
                    throw std::invalid_argument("Choose a decision for every previous room on every listed floor.");
                }
                PhysicalWallRoomPhaseSourceDecision d;d.room_id=old.room.id;d.expected_descriptor_digest=old.descriptor_digest;
                if (action=="supersede") {
                    if (!destination.alternative_id || !baseline_rooms.contains(old.room.id)) throw std::invalid_argument("Only baseline rooms can be replaced in an alternative.");
                    d.disposition=PhysicalWallRoomPhaseSourceDisposition::supersede_in_target;
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
                        if (f.allocated_room_id.empty()) {
                            f.allocated_room_id="physical-room-"+make_stable_id();
                            for (std::size_t edge=0;edge<current.boundary.size();++edge) {
                                f.ids.segment_ids.push_back("segment-"+make_stable_id());f.ids.vertex_ids.push_back("vertex-"+make_stable_id());
                            }
                        }
                        d.room_id=f.allocated_room_id;d.fresh_ids=f.ids;
                    } else if (action.starts_with("share:")) {
                        d.room_id=action.substr(6);
                        if (!controls.previous.contains(d.room_id) || value(controls.previous.at(d.room_id))!="share" ||
                            !controls.matching.at(d.room_id).contains(i) || !assigned.insert(d.room_id).second)
                            throw std::invalid_argument("Share each unchanged previous room with exactly one matching current space.");
                    } else throw std::invalid_argument("Choose a supported current-space decision.");
                }
                review.fresh.push_back(std::move(d));
            }
            for (const auto& [id,decision]:controls.previous) if (value(decision)=="share" && !assigned.contains(id))
                throw std::invalid_argument("Assign one matching current space to every room chosen for sharing.");
        }
        for (const auto& reference:references) {
            if (!reference.preserve->isChecked()) throw std::invalid_argument("Confirm Preserve in baseline for every listed reference.");
            result.baseline_only_acknowledgements.push_back(reference.evidence);
        }
        return result;
    }
    void changed() {
        if (rebuilding || invalidated) return;
        candidate.reset();candidate_snapshot.reset();accepted.reset();apply->setEnabled(false);
        for (auto& plane:planes) for (auto& f:plane.fresh) {
            const auto action=value(f.assignment);const bool create=action=="create";
            f.name->setEnabled(create);f.classification->setEnabled(create);f.factor->setEnabled(create);
            f.pick->setEnabled(!action.empty() && action!="unclassified");
            f.pick->setText(f.point?QStringLiteral("● Interior chosen · pick again"):QStringLiteral("Pick inside"));
        }
        try {require_current();rebuild_references();const auto decisions=intent();
            try {
                const auto prepared=prepare_physical_wall_phase_room_review(source,decisions);
                ApplyBoundaryConstraintChanges command;command.expected_revision=source.revision();command.message="Review alternative rooms";
                command.phase_room_review_completion=true;command.phase_room_review_intent=prepared.intent;
                auto exact=Document::preview_command(source,command);
                if (!exact_entities(exact.entities(),prepared.entities) || exact.assets()!=source.assets())
                    throw std::invalid_argument("Prepared room decisions differ from the complete command.");
                require_current();candidate=std::move(command);candidate_snapshot=std::move(exact);
                error.clear();status->setText(QStringLiteral("All listed rooms and references are reviewed. Apply saves these choices together."));apply->setEnabled(true);
            } catch (const std::exception&) {
                fail(QStringLiteral("These room choices cannot be applied. Check the shared room matches and all required floor, room and reference decisions."));
            }
        } catch (const std::exception& e) {fail(QString::fromUtf8(e.what()));}
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
            for (const auto& old:report.retained) if (!old.boundary.empty()) {
                CanvasEntity e{text(old.room.id),QStringLiteral("boundary"),old.boundary,0,false};e.holes=old.holes;
                e.stroke_color=QColor(130,143,158);e.dark_stroke_color=QColor(168,181,196);e.dashed_stroke=true;entities.push_back(std::move(e));
            }
            for (std::size_t i=0;i<report.fresh.size();++i) {
                const auto& current=report.fresh[i];CanvasEntity e{QStringLiteral("phase-candidate:%1:%2").arg(p).arg(i),QStringLiteral("boundary"),current.boundary,0,false};
                e.holes=current.holes;e.stroke_color=QColor(36,107,206);e.dark_stroke_color=QColor(104,171,255);
                e.filled=active_pick && active_pick->first==static_cast<std::size_t>(p) && active_pick->second==i;
                e.fill_color=QColor(36,107,206,35);entities.push_back(std::move(e));
                if (!current.boundary.empty()) labels.push_back({{},current.boundary.front().start,QStringLiteral("C%1").arg(i+1)});
                const auto& f=planes.at(static_cast<std::size_t>(p)).fresh.at(i);
                if (f.point) {CanvasLabel marker{QStringLiteral("phase-interior:%1:%2").arg(p).arg(i),*f.point,QStringLiteral("●")};
                    marker.bold=true;marker.paper_height_mm=3.5;labels.push_back(std::move(marker));}
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
PhysicalWallPhaseRoomReviewDialog::~PhysicalWallPhaseRoomReviewDialog()=default;
const std::optional<ApplyBoundaryConstraintChanges>& PhysicalWallPhaseRoomReviewDialog::acceptedCommand() const {return m_impl->accepted;}
QString PhysicalWallPhaseRoomReviewDialog::lastError() const {return m_impl->error;}
void PhysicalWallPhaseRoomReviewDialog::accept() {if (m_impl->submit()) QDialog::accept();}
void PhysicalWallPhaseRoomReviewDialog::reject() {
    m_impl->accepted.reset();m_impl->candidate.reset();m_impl->candidate_snapshot.reset();QDialog::reject();
}
} // namespace sketch::desktop

#include "sketch/desktop/site_placement_dialog.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/quantity.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {
constexpr std::size_t maximum_impacts=4096;
constexpr std::size_t maximum_annotation_children=4096;
QString q(const std::string& value) { return QString::fromStdString(value); }
void require(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
QString exact(double value) {
    require(std::isfinite(value),"Placement requires finite numbers");
    std::array<char,768> buffer{};
    const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,std::chars_format::fixed);
    require(result.ec==std::errc{},"Cannot display placement number");
    return QString::fromLatin1(buffer.data(),static_cast<qsizetype>(result.ptr-buffer.data()));
}
QString readable(const Entity& entity,std::size_t ordinal) {
    const auto found=entity.properties.find("name");
    if(found!=entity.properties.end() && found->is_string()) {
        const auto name=q(found->get<std::string>()).trimmed();if(!name.isEmpty())return name;
    }
    auto type=q(entity.type);type.replace('_',' ');
    return QString("%1 %2").arg(type).arg(static_cast<qulonglong>(ordinal));
}
QLineEdit* number(QWidget* owner,const char* name,double value,bool angle=false) {
    auto* result=new QLineEdit(exact(angle?value*180/std::numbers::pi:value)+(angle?" deg":" m"),owner);
    result->setObjectName(name);result->setProperty("initialText",result->text());result->setProperty("initialValue",value);
    result->setMaxLength(768);result->setToolTip(angle?"Yaw in degrees. deg and rad suffixes are supported.":"Length in metres or feet and inches. A bare number uses the current drawing units.");
    return result;
}
double read(QLineEdit* field,bool metric,bool angle=false) {
    if(field->text().trimmed()==field->property("initialText").toString().trimmed())
        return field->property("initialValue").toDouble();
    auto input=field->text().trimmed();
    double value;
    if(angle) {
        if(!input.endsWith("deg",Qt::CaseInsensitive) && !input.endsWith("rad",Qt::CaseInsensitive))input+=" deg";
        value=parse_angle(input.toStdString()).radians;
    } else value=parse_quantity(input.toStdString(),metric?Unit::metre:Unit::foot).metres;
    require(std::isfinite(value),"Placement requires finite numbers");return value;
}
bool same_pose(const SitePresentationPlacement& a,const SitePresentationPlacement& b) {
    return a.source_frame==b.source_frame && a.forward.translation_m.x==b.forward.translation_m.x &&
        a.forward.translation_m.y==b.forward.translation_m.y && a.forward.translation_m.z==b.forward.translation_m.z &&
        a.forward.rotation_radians==b.forward.rotation_radians;
}
QString xyz(const SiteRigidTransform& value) {
    return QString("%1, %2, %3").arg(exact(value.translation_m.x),exact(value.translation_m.y),exact(value.translation_m.z));
}
QString frame(const SitePresentationPlacement& value,const SiteFrameEntities& entities) {
    if(value.source_frame.mode==SiteFrameMode::world)return "World";
    const auto& id=value.source_frame.mode==SiteFrameMode::building?value.source_frame.building_id:value.source_frame.property_id;
    const auto found=entities.find(id);
    const auto name=found==entities.end()?q(id):readable(found->second,1);
    return (value.source_frame.mode==SiteFrameMode::building?"Building: ":"Site: ")+name;
}
bool same_change(const ApplyEntityChanges& a,const ApplyEntityChanges& b) {
    // A dialog only prepares one target upsert or an empty no-op. Do not use
    // serialized JSON text as a presentation/edit authority.
    if(a.entity_changes.size()!=b.entity_changes.size())return false;
    if(a.entity_changes.empty())return true;
    return a.entity_changes.front().entity==b.entity_changes.front().entity;
}
} // namespace

class SitePlacementDialog::Impl final {
public:
    SitePlacementDialog* owner;
    DocumentSnapshot source;
    Entity target;
    bool metric;
    std::string digest;
    QComboBox* mode{};
    std::array<QLineEdit*,4> pose{};
    QLineEdit* datum_identifier{};
    QLineEdit* datum_height{};
    QFormLayout* form{};
    QTableWidget* table{};
    QLabel* error{};
    QPushButton* save_button{};
    std::optional<SitePlacementDraft> preview;
    std::optional<SitePlacementDraft> accepted;

    Impl(SitePlacementDialog* dialog,DocumentSnapshot captured,std::string target_id,bool metric_units)
        :owner(dialog),source(std::move(captured)),metric(metric_units),digest(document_snapshot_digest(source)) {
        require(source.is_editable(),"Captured project is read only");
        const auto found=source.entities().find(target_id);
        require(found!=source.entities().end(),"Placement owner is missing from captured source");target=found->second;
        require(target.properties.is_object(),"Placement owner properties must be an object");
        require(SitePlacementDialog::supportsTarget(target),"Placement owner has no supported spatial presentation");
        validate_document_site_frames(source.entities());
        owner->setObjectName("sitePlacementDialog");owner->setWindowTitle("Site placement");owner->resize(760,560);
        auto* layout=new QVBoxLayout(owner);
        auto* heading=new QLabel(readable(target,1),owner);heading->setObjectName("sitePlacementOwnerName");layout->addWidget(heading);
        std::string host_id;
        if(target.type=="opening")host_id=target.properties.at("wall_id").get<std::string>();
        else if(target.type=="railing" && target.properties.contains("host"))
            host_id=target.properties.at("host").at("stair_id").get<std::string>();
        std::optional<SitePresentationPlacement> host_placement;
        if(!host_id.empty())host_placement=resolve_site_presentation(source,host_id);
        const QString explanation=target.type=="property"?
            "Enter the property origin in world coordinates and an explicit vertical datum. Preview shows affected origins in world metres and yaw.":
            target.type=="building"?"Enter the building origin relative to its property. Removing placement restores legacy world coordinates. Preview shows affected origins in world metres and yaw.":
            target.type=="terrain_surface"?"Choose legacy world elevations, heights relative to the site origin, or absolute heights in the property's exact declared datum. Preview shows the resulting origin in world metres.":
            target.type==kAnnotationEntityType?"Choose a frame for this annotation owner. Each label and symbol uses its own drawing layer; referenced geometry does not select its frame.":
            "Choose an explicit coordinate frame for this owner. Preview shows affected origins in world metres and yaw. Authored geometry and measurement facts stay intact.";
        auto* help=new QLabel(host_placement ?
            QString("Physical host: %1. Captured coordinate frame: %2. This owner inherits its host. Clear the explicit frame or retain this captured host mode.")
                .arg(readable(source.entities().at(host_id),1),frame(*host_placement,source.entities())) : explanation,owner);
        help->setObjectName("sitePlacementExplanation");
        help->setWordWrap(true);layout->addWidget(help);
        form=new QFormLayout;layout->addLayout(form);
        mode=new QComboBox(owner);mode->setObjectName("sitePlacementMode");
        mode->addItem("Legacy/default (remove explicit frame)","default");
        if(target.type=="property")mode->addItem("Site origin and vertical datum","site");
        else if(target.type=="building")mode->addItem("Placed relative to property","building");
        else if(target.type=="terrain_surface") {
            mode->addItem("Elevation relative to site origin","relative_site_origin");
            mode->addItem("Absolute elevation in declared datum","declared_absolute");
        } else if(host_placement) {
            const auto captured_mode=host_placement->source_frame.mode;
            mode->addItem("Captured host frame: "+frame(*host_placement,source.entities()),
                captured_mode==SiteFrameMode::world?"world":captured_mode==SiteFrameMode::site?"site":"building");
        } else {
            mode->addItem("World coordinates","world");mode->addItem("Site coordinates","site");mode->addItem("Building coordinates","building");
        }
        form->addRow("Coordinate frame",mode);
        SiteRigidTransform initial{};std::string selected="default";std::string datum;double height=0;
        if(target.type=="property" && target.properties.contains("site_frame")) {
            const auto value=decode_site_frame(target.properties.at("site_frame"));initial=value.to_world;
            datum=value.vertical_datum.identifier;height=value.vertical_datum.height_at_origin_m;selected="site";
        } else if(target.type=="building" && target.properties.contains("site_placement")) {
            initial=decode_building_site_placement(target.properties.at("site_placement"));selected="building";
        } else if(target.type=="terrain_surface" && target.properties.contains("terrain_elevation_binding")) {
            const auto value=decode_terrain_elevation_binding(target.properties.at("terrain_elevation_binding"));
            selected=value.mode==TerrainElevationBinding::Mode::relative_site_origin?"relative_site_origin":"declared_absolute";datum=value.datum_identifier;
        } else if(target.properties.contains("presentation_frame")) {
            const auto value=decode_presentation_frame(target.properties.at("presentation_frame"));
            selected=value==SiteFrameMode::world?"world":value==SiteFrameMode::site?"site":"building";
        }
        const auto selected_index=mode->findData(q(selected));
        require(selected_index>=0,"Placement owner has a conflicting coordinate-frame contract");mode->setCurrentIndex(selected_index);
        const std::array<double,4> values{initial.translation_m.x,initial.translation_m.y,initial.translation_m.z,initial.rotation_radians};
        const std::array<const char*,4> names{"sitePlacementX","sitePlacementY","sitePlacementZ","sitePlacementYaw"};
        const std::array<const char*,4> labels{"Origin X","Origin Y","Origin Z","Yaw"};
        for(std::size_t i=0;i<pose.size();++i) {pose[i]=number(owner,names[i],values[i],i==3);form->addRow(labels[i],pose[i]);}
        datum_identifier=new QLineEdit(q(datum),owner);datum_identifier->setObjectName("sitePlacementDatumIdentifier");
        // The captured contract permits 1024 UTF-8 bytes. Its UTF-16 text
        // cannot require more code units; a shorter control limit would
        // silently truncate a valid datum before an unchanged Preview/Save.
        // Core validation still enforces the exact UTF-8 byte bound on edits.
        datum_identifier->setMaxLength(1024);
        datum_identifier->setToolTip("Exact declared vertical datum identifier; height is explicitly entered, never inferred from terrain or grade.");
        form->addRow("Vertical datum identifier",datum_identifier);
        datum_height=number(owner,"sitePlacementDatumHeight",height);form->addRow("Datum height at origin",datum_height);
        auto* preview_button=new QPushButton("Preview placement",owner);preview_button->setObjectName("sitePlacementPreview");layout->addWidget(preview_button);
        table=new QTableWidget(0,7,owner);table->setObjectName("sitePlacementPreviewTable");
        table->setHorizontalHeaderLabels({"Owner","Before XYZ (m)","Before yaw (deg)","After XYZ (m)","After yaw (deg)","Before frame","After frame"});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->hide();layout->addWidget(table,1);
        error=new QLabel(owner);error->setObjectName("sitePlacementError");error->setWordWrap(true);layout->addWidget(error);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,owner);buttons->setObjectName("sitePlacementButtons");layout->addWidget(buttons);
        save_button=buttons->button(QDialogButtonBox::Save);save_button->setEnabled(false);
        QObject::connect(preview_button,&QPushButton::clicked,owner,[this]{(void)owner->previewUpdate();});
        QObject::connect(buttons,&QDialogButtonBox::accepted,owner,[this]{(void)owner->submit();});
        QObject::connect(buttons,&QDialogButtonBox::rejected,owner,&QDialog::reject);
        QObject::connect(mode,&QComboBox::currentIndexChanged,owner,[this]{visibility();invalidate();});
        for(auto* field:pose)QObject::connect(field,&QLineEdit::textChanged,owner,[this]{invalidate();});
        QObject::connect(datum_identifier,&QLineEdit::textChanged,owner,[this]{invalidate();});
        QObject::connect(datum_height,&QLineEdit::textChanged,owner,[this]{invalidate();});
        visibility();
    }
    void visibility() {
        const auto selected=mode->currentData().toString();const bool coordinates=selected!="default" && (target.type=="property" || target.type=="building");
        for(auto* field:pose)form->setRowVisible(field,coordinates);
        form->setRowVisible(datum_identifier,(target.type=="property" && coordinates) || (target.type=="terrain_surface" && selected=="declared_absolute"));
        form->setRowVisible(datum_height,target.type=="property" && coordinates);
    }
    void invalidate() {preview.reset();accepted.reset();table->setRowCount(0);error->clear();save_button->setEnabled(false);}
    template<class F> bool guard(F&& action) {
        error->clear();accepted.reset();
        try {action();return true;}
        catch(const std::exception& failure) {preview.reset();table->setRowCount(0);save_button->setEnabled(false);error->setText(q(failure.what()));return false;}
    }
    Entity replacement() const {
        auto result=target;const auto selected=mode->currentData().toString();const bool remove=selected=="default";
        const char* key=target.type=="property"?"site_frame":target.type=="building"?"site_placement":
            target.type=="terrain_surface"?"terrain_elevation_binding":"presentation_frame";
        if(remove)result.properties.erase(key);
        else if(target.type=="property" || target.type=="building") {
            const auto x=read(pose[0],metric),y=read(pose[1],metric),z=read(pose[2],metric),yaw=read(pose[3],metric,true);
            if(target.type=="property")result.properties[key]={{"version",1},{"origin_m",{x,y,z}},{"rotation_radians",yaw},
                {"vertical_datum",{{"identifier",datum_identifier->text().toStdString()},{"height_at_origin_m",read(datum_height,metric)}}}};
            else result.properties[key]={{"version",1},{"translation_m",{x,y,z}},{"rotation_radians",yaw}};
        } else if(target.type=="terrain_surface") {
            result.properties[key]={{"version",1},{"mode",selected.toStdString()}};
            if(selected=="declared_absolute")result.properties[key]["datum_identifier"]=datum_identifier->text().toStdString();
        } else result.properties[key]={{"version",1},{"mode",selected.toStdString()}};
        if(!remove && target.properties.contains(key) && (target.type=="property" || target.type=="building")) {
            // Preserve the exact stored numeric representation in untouched
            // fields even when a sibling field is edited.
            const auto& original=target.properties.at(key);auto& contract=result.properties.at(key);
            const char* position=target.type=="property"?"origin_m":"translation_m";
            for(std::size_t i=0;i<3;++i)if(contract.at(position).at(i)==original.at(position).at(i))contract[position][i]=original.at(position).at(i);
            if(contract.at("rotation_radians")==original.at("rotation_radians"))contract["rotation_radians"]=original.at("rotation_radians");
            if(target.type=="property" && contract.at("vertical_datum")==original.at("vertical_datum"))contract["vertical_datum"]=original.at("vertical_datum");
            else if(target.type=="property" && contract.at("vertical_datum").at("height_at_origin_m")==original.at("vertical_datum").at("height_at_origin_m"))
                contract["vertical_datum"]["height_at_origin_m"]=original.at("vertical_datum").at("height_at_origin_m");
        }
        if(target.type==kAnnotationEntityType) {
            if(!remove)result.properties["version"]=3;
            else if(result.properties.at("version")==3) {
                bool scoped=true;for(const auto* scope:{"property_id","building_id","floor_id","layer_id"})scoped=scoped && result.properties.contains(scope);
                result.properties["version"]=scoped?2:1;
            }
        }
        return result;
    }
    SitePlacementDraft validated() const {
        const auto updated=replacement();auto entities=source.entities();entities.at(target.id)=updated;
        validate_document_site_frames(entities);
        ApplyEntityChanges command{source.revision(),{}, {},"Edit site placement"};
        if(updated!=target)command.entity_changes.push_back(EntityChange::upsert(updated));
        // Also validates complete retained history, source geometry, metadata,
        // annotation versions, assets and the normal command boundary.
        const auto trial=Document::preview_command(source,command);
        SitePlacementDraft draft{source.revision(),digest,target.id,std::move(command),{}};
        std::vector<std::string> ids;std::vector<SiteAnnotationTarget> children;
        std::map<SiteAnnotationTarget,std::string> child_names;
        std::map<std::string,std::string,std::less<>> names;
        std::size_t ordinal=0;
        for(const auto& [id,entity]:source.entities()) {
            names.emplace(id,readable(entity,++ordinal).toStdString());
            if(entity.type!=kAnnotationEntityType) {if(SitePlacementDialog::supportsTarget(entity) || entity.type=="floor" || entity.type=="layer")ids.push_back(id);continue;}
            // Strict public decoding; do not strip an explicit owner frame.
            const auto state=decode_annotation_entity(entity);
            require(state.labels.size()<=maximum_annotation_children-children.size(),"Placement preview exceeds annotation child limit");
            require(state.symbols.size()<=maximum_annotation_children-children.size()-state.labels.size(),"Placement preview exceeds annotation child limit");
            for(const auto& label:state.labels) {
                SiteAnnotationTarget key{id,label.id};children.push_back(key);
                auto name=q(label.content).trimmed().left(80);if(name.isEmpty())name="Label "+QString::number(children.size());
                child_names.emplace(key,names.at(id)+" / "+name.toStdString());
            }
            for(const auto& symbol:state.symbols) {
                SiteAnnotationTarget key{id,symbol.id};children.push_back(key);
                const auto name=symbol.definition && !symbol.definition->name.empty()?symbol.definition->name:"Symbol "+std::to_string(children.size());
                child_names.emplace(key,names.at(id)+" / "+name);
            }
        }
        const auto before=resolve_site_presentations(source,ids),after=resolve_site_presentations(trial,ids);
        const auto append=[&](SitePlacementImpact impact) {require(draft.impacts.size()<maximum_impacts,"Placement preview exceeds affected-owner limit");draft.impacts.push_back(std::move(impact));};
        for(const auto& id:ids)if(id==target.id || !same_pose(before.at(id),after.at(id)))append({id,{},names.at(id),before.at(id),after.at(id)});
        const auto before_children=resolve_site_annotation_presentations(source,children),after_children=resolve_site_annotation_presentations(trial,children);
        for(const auto& child:children)if(child.owner_entity_id==target.id || !same_pose(before_children.at(child),after_children.at(child)))
            append({child.owner_entity_id,child.child_id,child_names.at(child),before_children.at(child),after_children.at(child)});
        return draft;
    }
    void show(const SitePlacementDraft& draft) {
        table->setRowCount(static_cast<int>(draft.impacts.size()));
        for(std::size_t row=0;row<draft.impacts.size();++row) {
            const auto& impact=draft.impacts[row];
            const std::array<QString,7> cells{q(impact.display_name),xyz(impact.before.forward),exact(impact.before.forward.rotation_radians*180/std::numbers::pi),
                xyz(impact.after.forward),exact(impact.after.forward.rotation_radians*180/std::numbers::pi),frame(impact.before,source.entities()),frame(impact.after,source.entities())};
            for(std::size_t col=0;col<cells.size();++col) {
                auto* item=new QTableWidgetItem(cells[col]);item->setToolTip(cells[col]);table->setItem(static_cast<int>(row),static_cast<int>(col),item);
            }
            auto* item=table->item(static_cast<int>(row),0);item->setData(Qt::UserRole,q(impact.owner_entity_id));
            if(impact.annotation_child_id)item->setData(Qt::UserRole+1,q(*impact.annotation_child_id));
            item->setToolTip(q(impact.owner_entity_id)+(impact.annotation_child_id?" / "+q(*impact.annotation_child_id):""));
        }
        table->resizeRowsToContents();
    }
};

bool SitePlacementDialog::supportsTarget(const Entity& entity) {
    // Only owners consumed by spatial presentation may advertise this editor.
    // Persisted opaque/future frame validation remains a separate core policy.
    constexpr std::array<std::string_view,21> types{"property","building","terrain_surface",
        "boundary","measurement_boundary","room_boundary","wall","room","slab","roof",
        "stair","column","beam","opening","railing","assembly_instance","reference_asset",
        "reference_grid","annotation_state","measurement_linework","dimension"};
    if(std::find(types.begin(),types.end(),entity.type)==types.end())return false;
    if(entity.type=="dimension") {
        try {return decode_boundary_dimension_entity(entity).supported();}
        catch(const std::exception&) {return false;}
    }
    return true;
}

SitePlacementDialog::SitePlacementDialog(DocumentSnapshot source,std::string target_entity_id,bool metric_units,QWidget* parent)
    :QDialog(parent),m_impl(std::make_unique<Impl>(this,std::move(source),std::move(target_entity_id),metric_units)) {}
SitePlacementDialog::~SitePlacementDialog()=default;
bool SitePlacementDialog::previewUpdate() {
    return m_impl->guard([&]{auto draft=m_impl->validated();m_impl->show(draft);m_impl->preview=std::move(draft);m_impl->save_button->setEnabled(true);});
}
bool SitePlacementDialog::submit() {
    return m_impl->guard([&]{
        auto draft=m_impl->validated();
        require(m_impl->preview && same_change(m_impl->preview->command,draft.command),"Preview the current placement before saving");
        m_impl->accepted=std::move(draft);accept();
    });
}
std::optional<SitePlacementDraft> SitePlacementDialog::candidate() const {return result()==QDialog::Accepted?m_impl->accepted:std::nullopt;}
QString SitePlacementDialog::lastError() const {return m_impl->error->text();}
} // namespace sketch::desktop

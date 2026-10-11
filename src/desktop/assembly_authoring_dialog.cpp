#include "sketch/desktop/assembly_authoring_dialog.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/quantity.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sketch::desktop {
namespace {
constexpr int maximum_rows = 4096;
QString q(const std::string& s) { return QString::fromStdString(s); }
void require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
QString exact(double value) {
    std::array<char,768> buffer{};
    auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,std::chars_format::fixed);
    require(result.ec==std::errc{},"Cannot display assembly number");
    return QString::fromLatin1(buffer.data(),static_cast<qsizetype>(result.ptr-buffer.data()));
}
QString label(const Entity& entity, const QString& fallback) {
    if (entity.properties.is_object() && entity.properties.contains("name") && entity.properties.at("name").is_string()) {
        const auto value=q(entity.properties.at("name").get<std::string>()).trimmed(); if (!value.isEmpty()) return value;
    }
    return fallback;
}
AssemblyModel model_for(const DocumentSnapshot& source, const std::string& id) {
    const auto found=source.entities().find(id);
    require(found!=source.entities().end() && found->second.type=="assembly_model","Assembly catalog source is missing or has the wrong type");
    return AssemblyModel::from_json(found->second.properties.at("model"));
}
const AssemblyType& find_type(const AssemblyModel& model, const std::string& id) {
    auto found=std::find_if(model.types().begin(),model.types().end(),[&](const auto& t){return t.id==id;});
    require(found!=model.types().end(),"Assembly type is unavailable"); return *found;
}
Boundary captured_boundary(const Entity& entity) {
    const auto source=inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::anonymous_legacy
        ? upgrade_legacy_boundary_entity(entity) : entity;
    auto result=boundary_geometry(decode_identified_boundary_entity(source));
    require(result.size()<=1024 && validate_boundary(result).empty(),"Assembly boundary must be valid, closed, and within the segment limit");return result;
}
std::vector<Boundary> captured_holes(const Entity& entity,const DocumentSnapshot& source,const Boundary& outer) {
    std::vector<Boundary> result;
    const auto inline_holes=entity.properties.find("holes");
    const auto deductions=entity.properties.find("deduction_ids");
    require(inline_holes==entity.properties.end() || deductions==entity.properties.end() || inline_holes->empty() || deductions->empty(),"Boundary has ambiguous inline holes and deductions");
    if(inline_holes!=entity.properties.end()) {
        require(inline_holes->is_array() && inline_holes->size()<=1024,"Boundary holes must be a bounded array");
        for(const auto& geometry:*inline_holes) {
            // Decode captured legacy segment arrays through the established
            // boundary codec. Only local geometry is copied into the profile.
            Entity hole{"assembly-captured-cutout","boundary",{{"segments",geometry}}};result.push_back(captured_boundary(hole));
        }
    }
    if(deductions!=entity.properties.end()) {
        require(deductions->is_array() && deductions->size()<=1024,"Boundary deduction identities must be a bounded array");
        for(const auto& identity:*deductions) {
            require(identity.is_string(),"Boundary deduction identity must be text");
            const auto found=source.entities().find(identity.get<std::string>());
            require(found!=source.entities().end() && can_recognize_boundary_entity_type(found->second.type),"Boundary deduction source is missing");
            result.push_back(captured_boundary(found->second));
        }
    }
    std::size_t segments=outer.size();for(const auto& hole:result){require(hole.size()<=1024-segments,"Profile outer and cutouts exceed the segment limit");segments+=hole.size();}
    if(const auto invalid=validate_boundary_holes(outer,result))throw std::invalid_argument(*invalid);
    return result;
}
QLineEdit* field(QWidget* parent, const QString& name, const QString& text) {
    auto* result=new QLineEdit(text,parent); result->setObjectName(name); return result;
}
QLineEdit* number(QWidget* parent, const QString& name, double value, bool length=false, bool angle=false) {
    auto* result=field(parent,name,exact(angle ? value*180/std::numbers::pi : value)+(length ? " m" : ""));
    result->setProperty("initialText",result->text()); result->setProperty("initialValue",value);
    if (angle) result->setToolTip("Yaw in degrees; deg and rad suffixes are supported.");
    return result;
}
double scalar(const QString& text) {
    bool ok=false; const auto value=text.trimmed().toDouble(&ok);
    require(ok && std::isfinite(value),"Enter a finite numeric value"); return value;
}
double read_number(QLineEdit* input, bool metric, bool length=false, bool angle=false) {
    if (input->text().trimmed()==input->property("initialText").toString().trimmed())
        return input->property("initialValue").toDouble();
    if (length) return parse_quantity(input->text().toStdString(),metric ? Unit::metre : Unit::foot).metres;
    if (angle) {
        auto expression=input->text().trimmed();
        if(!expression.endsWith("deg",Qt::CaseInsensitive) && !expression.endsWith("rad",Qt::CaseInsensitive))expression+=" deg";
        return parse_angle(expression.toStdString()).radians;
    }
    return scalar(input->text());
}
struct TransformFields {
    std::array<QLineEdit*,6> fields{};
    bool mirrored_y{false};
    AssemblyTransform initial_transform;
    static TransformFields form(QWidget* parent, QFormLayout* layout, const QString& prefix, const AssemblyTransform& transform) {
        TransformFields result; result.mirrored_y=transform.mirrored_y; result.initial_transform=transform;
        const std::array<double,6> values{transform.translation_m.x,transform.translation_m.y,transform.translation_m.z,transform.rotation_radians,transform.scale,transform.scale*transform.vertical_scale};
        const std::array<QString,6> suffixes{"X","Y","Z","Yaw","Scale","HeightScale"};
        const std::array<QString,6> labels{"X","Y","Base Z","Yaw (degrees)","Width/depth scale","Height scale"};
        for (int i=0;i<6;++i) { result.fields[i]=number(parent,prefix+suffixes[i],values[i],i<3,i==3); layout->addRow(labels[i],result.fields[i]); }
        return result;
    }
    AssemblyTransform read(bool metric) const {
        const auto scale=read_number(fields[4],metric);
        const auto height_scale=read_number(fields[5],metric);
        require(std::isfinite(scale) && scale>0 && std::isfinite(height_scale) && height_scale>0,"Width/depth and height scales must be positive and finite");
        const auto unchanged=[](QLineEdit* input){return input->text().trimmed()==input->property("initialText").toString().trimmed();};
        // Retain the authored factor exactly when neither scale was edited.
        // The height control shows physical Z scale, independent of plan scale.
        const auto vertical_scale=unchanged(fields[4]) && unchanged(fields[5]) ? initial_transform.vertical_scale : height_scale/scale;
        require(std::isfinite(vertical_scale) && vertical_scale>0 && std::isfinite(scale*vertical_scale) && scale*vertical_scale>0,"Resulting height scale must be positive and finite");
        return {{read_number(fields[0],metric,true),read_number(fields[1],metric,true),read_number(fields[2],metric,true)},
                read_number(fields[3],metric,false,true),scale,mirrored_y,vertical_scale};
    }
    void set(const AssemblyTransform& value) {
        mirrored_y=value.mirrored_y;
        initial_transform=value;
        const std::array<double,6> values{value.translation_m.x,value.translation_m.y,value.translation_m.z,value.rotation_radians,value.scale,value.scale*value.vertical_scale};
        for (int i=0;i<6;++i) {
            const auto text=exact(i==3 ? values[i]*180/std::numbers::pi : values[i])+(i<3 ? " m" : "");
            fields[i]->setText(text); fields[i]->setProperty("initialText",text); fields[i]->setProperty("initialValue",values[i]);
        }
    }
};
QTableWidget* table(QWidget* parent, const QString& name, const QStringList& columns) {
    auto* result=new QTableWidget(0,columns.size(),parent); result->setObjectName(name); result->setHorizontalHeaderLabels(columns);
    result->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result->setSelectionBehavior(QAbstractItemView::SelectRows); result->setSelectionMode(QAbstractItemView::SingleSelection);
    result->setMinimumHeight(130); return result;
}
QPushButton* button(QWidget* parent, QHBoxLayout* layout, const QString& name, const QString& text) {
    auto* result=new QPushButton(text,parent); result->setObjectName(name); layout->addWidget(result); return result;
}
void row_controls(QWidget* parent, QVBoxLayout* layout, QTableWidget* rows, const QString& prefix, std::function<void()> add, bool reorder=false) {
    auto* bar=new QHBoxLayout;
    auto* add_button=button(parent,bar,prefix+"Add","Add");
    QObject::connect(add_button,&QPushButton::clicked,parent,[parent,add=std::move(add)]{
        try {add();}
        catch(const std::exception& e) {
            if(auto* error=parent->window()->findChild<QLabel*>("assemblyAuthoringError"))error->setText(q(e.what()));
        }
    });
    auto* remove=button(parent,bar,prefix+"Remove","Remove selected");
    QObject::connect(remove,&QPushButton::clicked,parent,[rows]{if(rows->currentRow()>=0)rows->removeRow(rows->currentRow());});
    if (reorder) {
        rows->verticalHeader()->setSectionsMovable(true);
        for (const auto& entry : {std::pair{"Up",-1},std::pair{"Down",1}}) {
            auto* control=button(parent,bar,prefix+entry.first,entry.second<0 ? "Move up" : "Move down");
            QObject::connect(control,&QPushButton::clicked,parent,[rows,direction=entry.second]{
                if(rows->currentRow()<0)return; auto* header=rows->verticalHeader();
                const auto from=header->visualIndex(rows->currentRow()); const auto to=from+direction;
                if(to>=0 && to<rows->rowCount())header->moveSection(from,to);
            });
        }
    }
    layout->addLayout(bar);
}
int append_row(QTableWidget* rows) {
    require(rows->rowCount()<maximum_rows,"Assembly collection limit reached");
    auto row=rows->rowCount(); rows->insertRow(row); return row;
}
QString cell(QTableWidget* table, int row, int column) {
    require(table->item(row,column)!=nullptr,"Missing assembly table value"); return table->item(row,column)->text();
}
QComboBox* material_choices(QWidget* parent, const AssemblyModel& model, const std::string& selected) {
    auto* result=new QComboBox(parent);
    for (const auto& material:model.materials()) result->addItem(q(material.name),q(material.id));
    if(!selected.empty()) result->setCurrentIndex(result->findData(q(selected))); return result;
}

// Explicit rows are retained even when their values equal the inherited value.
// Removing a row is the only way to remove its authored override.
class NamedValues final {
public:
    QTabWidget* tabs{};
    QTableWidget *properties{}, *materials{}, *quantities{};
    NamedValues(QWidget* parent, const QString& prefix, const AssemblyModel& model) : model_(model) {
        tabs=new QTabWidget(parent); tabs->setObjectName(prefix+"Values");
        properties=page(parent,prefix+"Properties","Properties",{"Name","Value"});
        materials=page(parent,prefix+"Materials","Material slots",{"Slot","Material"});
        quantities=page(parent,prefix+"Quantities","Declared quantities",{"Name","Value (SI)","Unit"});
        properties->setToolTip("Use declared property names for instance and part overrides.");
        quantities->setToolTip("Explicit quantities per instance. Values use the selected SI unit and are not inferred from geometry.");
    }
    void set(const std::map<std::string,std::string>& p, const std::map<std::string,std::string>& m,
             const std::map<std::string,AssemblyQuantityProperty>& quantities_map) {
        properties->setRowCount(0); materials->setRowCount(0); quantities->setRowCount(0);
        for(const auto& [key,value]:p)add(properties,key,value);
        for(const auto& [key,value]:m)add(materials,key,value);
        for(const auto& [key,value]:quantities_map)add_quantity(key,value);
    }
    std::map<std::string,std::string> read_properties() const {return strings(properties,false);}
    std::map<std::string,std::string> read_materials() const {return strings(materials,true);}
    std::map<std::string,AssemblyQuantityProperty> read_quantities() const {
        std::map<std::string,AssemblyQuantityProperty> result;
        for(int row=0;row<quantities->rowCount();++row) {
            auto key=cell(quantities,row,0).trimmed().toStdString(); require(!key.empty(),"Quantity name must not be blank");
            auto* units=qobject_cast<QComboBox*>(quantities->cellWidget(row,2)); require(units && units->currentIndex()>=0,"Select a quantity unit");
            auto value=AssemblyQuantityProperty{scalar(cell(quantities,row,1)),static_cast<AssemblyQuantityUnit>(units->currentData().toInt())};
            require(result.emplace(key,value).second,"Duplicate quantity name");
        }
        return result;
    }
private:
    const AssemblyModel& model_;
    QTableWidget* page(QWidget* parent, const QString& name, const QString& title, const QStringList& columns) {
        auto* page=new QWidget(parent); auto* layout=new QVBoxLayout(page); auto* rows=table(page,name,columns); layout->addWidget(rows);
        row_controls(page,layout,rows,name,[this,rows]{
            if(rows->columnCount()==3)add_quantity({},{}); else add(rows,{},{});
        });
        tabs->addTab(page,title); return rows;
    }
    void add(QTableWidget* rows, const std::string& key, const std::string& value) {
        auto row=append_row(rows); rows->setItem(row,0,new QTableWidgetItem(q(key)));
        if(rows==materials)rows->setCellWidget(row,1,material_choices(rows,model_,value));
        else rows->setItem(row,1,new QTableWidgetItem(q(value)));
    }
    void add_quantity(const std::string& key, const AssemblyQuantityProperty& value) {
        auto row=append_row(quantities); quantities->setItem(row,0,new QTableWidgetItem(q(key)));
        quantities->setItem(row,1,new QTableWidgetItem(exact(value.value)));
        auto* unit=new QComboBox(quantities);
        const std::array<QString,5> names{"Count","Metres","Square metres","Cubic metres","Kilograms"};
        for(int i=0;i<5;++i)unit->addItem(names[i],i); unit->setCurrentIndex(static_cast<int>(value.unit));
        quantities->setCellWidget(row,2,unit);
    }
    std::map<std::string,std::string> strings(QTableWidget* rows, bool material) const {
        std::map<std::string,std::string> result;
        for(int row=0;row<rows->rowCount();++row) {
            auto key=cell(rows,row,0).trimmed().toStdString(); require(!key.empty(),"Property or material slot name must not be blank");
            std::string value;
            if(material) { auto* choice=qobject_cast<QComboBox*>(rows->cellWidget(row,1)); require(choice && choice->currentIndex()>=0,"Select an available material"); value=choice->currentData().toString().toStdString(); }
            else value=cell(rows,row,1).toStdString();
            require(result.emplace(key,value).second,"Duplicate property or material slot name");
        }
        return result;
    }
};
QString quantity_unit(AssemblyQuantityUnit unit) {
    switch(unit) {
        case AssemblyQuantityUnit::count:return "count";
        case AssemblyQuantityUnit::metre:return "m";
        case AssemblyQuantityUnit::square_metre:return "m²";
        case AssemblyQuantityUnit::cubic_metre:return "m³";
        case AssemblyQuantityUnit::kilogram:return "kg";
    }
    return {};
}
QString readable_path(const AssemblyModel& model,const std::string& root,const std::vector<std::string>& path) {
    auto* current=&find_type(model,root);QStringList names;
    for(const auto& id:path) {
        auto found=std::find_if(current->parts.begin(),current->parts.end(),[&](const auto& part){return part.id==id;});
        if(found==current->parts.end())return "Unavailable saved part (remove overrides before changing type)";
        const auto ordinal=std::distance(current->parts.begin(),found)+1;current=&find_type(model,found->type_id);
        names << q(current->name)+QString(" · part %1").arg(ordinal);
    }
    return names.join(" / ");
}
QString overrides(const AssemblyInstance& instance,const AssemblyModel& model) {
    QStringList result;
    auto append=[&](const auto& values,const QString& prefix){
        for(const auto& [key,value]:values) {
            if constexpr(std::is_same_v<std::decay_t<decltype(value)>,AssemblyQuantityProperty>)
                result << prefix+q(key)+" = "+exact(value.value)+" "+quantity_unit(value.unit);
            else {
                auto displayed=q(value);
                if(prefix.contains("Material")) {
                    auto found=std::find_if(model.materials().begin(),model.materials().end(),[&](const auto& item){return item.id==value;});
                    if(found!=model.materials().end())displayed=q(found->name);
                }
                result << prefix+q(key)+" = "+displayed;
            }
        }
    };
    append(instance.property_overrides,"Property: "); append(instance.material_overrides,"Material: "); append(instance.quantity_overrides,"Quantity: ");
    for(const auto& nested:instance.nested_overrides) {
        result << readable_path(model,instance.type_id,nested.part_path);
        if(nested.transform) {
            const auto& t=*nested.transform;
            result << QString("  Local placement: (%1, %2, %3) m; yaw %4°; scale %5")
                .arg(exact(t.translation_m.x),exact(t.translation_m.y),exact(t.translation_m.z),exact(t.rotation_radians*180/std::numbers::pi),exact(t.scale));
        }
        append(nested.property_overrides,"  Property: "); append(nested.material_overrides,"  Material: "); append(nested.quantity_overrides,"  Quantity: ");
    }
    return result.isEmpty() ? "No explicit overrides" : result.join('\n');
}
QString resolved_changes(const AssemblyExpansion& before,const AssemblyExpansion& after,const AssemblyModel& model,const AssemblyModel& updated) {
    QStringList result;
    for(const auto& node:after.nodes) {
        auto found=std::find_if(before.nodes.begin(),before.nodes.end(),[&](const auto& old){return old.part_path==node.part_path;});
        const auto owner=node.part_path.empty() ? QString("Root") : readable_path(updated,after.source_instance.type_id,node.part_path);
        if(found==before.nodes.end()){result<<owner+": added part";continue;}
        if(found->transform!=node.transform) {
            const auto& t=node.transform;
            result<<owner+QString(": placement → (%1, %2, %3) m; yaw %4°; scale %5")
                .arg(exact(t.translation_m.x),exact(t.translation_m.y),exact(t.translation_m.z),exact(t.rotation_radians*180/std::numbers::pi),exact(t.scale));
        }
        auto compare=[&](const auto& old_values,const auto& new_values,const QString& category) {
            for(const auto& [key,value]:new_values) {
                auto old=old_values.find(key);
                if(old!=old_values.end() && old->second==value)continue;
                auto display=[&](const auto& v) {
                    if constexpr(std::is_same_v<std::decay_t<decltype(v)>,AssemblyQuantityProperty>)return exact(v.value)+" "+quantity_unit(v.unit);
                    else {
                        if(category=="Material slot ") {
                            auto material=std::find_if(model.materials().begin(),model.materials().end(),[&](const auto& item){return item.id==v;});
                            if(material!=model.materials().end())return q(material->name);
                        }
                        return q(v);
                    }
                };
                result<<owner+": "+category+q(key)+" = "+(old==old_values.end() ? QString("(new)") : display(old->second))+" → "+display(value);
            }
            for(const auto& [key,value]:old_values){(void)value;if(!new_values.contains(key))result<<owner+": "+category+q(key)+" removed";}
        };
        compare(found->properties,node.properties,"Property ");compare(found->materials,node.materials,"Material slot ");compare(found->quantities,node.quantities,"Quantity ");
    }
    for(const auto& node:before.nodes)if(std::none_of(after.nodes.begin(),after.nodes.end(),[&](const auto& item){return item.part_path==node.part_path;}))
        result<<readable_path(model,before.source_instance.type_id,node.part_path)+": removed part";
    for(const auto& profile:after.profiles) {
        auto found=std::find_if(before.profiles.begin(),before.profiles.end(),[&](const auto& old){return old.part_path==profile.part_path && old.profile.id==profile.profile.id;});
        if(found==before.profiles.end() || !(found->profile==profile.profile))result<<"Solid outline or extrusion changed";
    }
    result.removeDuplicates();return result.isEmpty() ? "Resolved geometry and values retained" : result.join('\n');
}
void finish_dialog(QDialog* dialog, QVBoxLayout* layout, QLabel*& error, std::function<void()> submit) {
    error=new QLabel(dialog); error->setObjectName("assemblyAuthoringError"); error->setWordWrap(true); layout->addWidget(error);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,dialog);buttons->setObjectName("assemblyAuthoringButtons");layout->addWidget(buttons);
    QObject::connect(buttons,&QDialogButtonBox::accepted,dialog,std::move(submit));
    QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
}

void validate_profile_geometry(const AssemblyProfile& profile) {
    require(profile.outer.size()<=1024,"Profile outer and cutouts exceed the segment limit");
    auto segments=profile.outer.size();
    for(const auto& hole:profile.holes) {
        require(hole.size()<=1024-segments,"Profile outer and cutouts exceed the segment limit");
        segments+=hole.size();
    }
    if(const auto invalid=validate_boundary_holes(profile.outer,profile.holes))throw std::invalid_argument(*invalid);
}

// Presentation only: these paths are never converted back into stored geometry.
class ProfileGeometryPreview final : public QWidget {
public:
    explicit ProfileGeometryPreview(QWidget* parent) : QWidget(parent) {
        setObjectName("assemblyProfileGeometryPreview");setMinimumSize(260,240);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        setAccessibleName("Profile-local outline and cutout preview");
        setAccessibleDescription("Filled solid with open cutouts; the edited boundary and selected numbered vertex are highlighted. X runs right and Y runs up.");
    }
    void show_profile(AssemblyProfile value,std::optional<std::size_t> hole,int vertex) {
        profile_=std::move(value);hole_=hole;vertex_=vertex;message_.clear();update();
    }
    void show_error(QString message) {message_=std::move(message);update();}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(),palette().base());
        if(!message_.isEmpty()) {
            painter.setPen(palette().text().color());painter.drawText(rect().adjusted(12,12,-12,-12),Qt::AlignCenter|Qt::TextWordWrap,message_);return;
        }
        try {
            const auto bounds=boundary_bounds(profile_.outer);
            const auto width_m=bounds.maximum.x-bounds.minimum.x,height_m=bounds.maximum.y-bounds.minimum.y;
            require(std::isfinite(width_m) && std::isfinite(height_m) && width_m>0 && height_m>0,"Preview extent is not representable");
            const auto scale=std::min((width()-40)/width_m,(height()-40)/height_m);
            require(std::isfinite(scale) && scale>0,"Preview extent is not representable");
            auto screen=[&](Vec2 point){return QPointF(width()*.5+((point.x-bounds.minimum.x)-width_m*.5)*scale,
                                                     height()*.5-((point.y-bounds.minimum.y)-height_m*.5)*scale);};
            auto path_for=[&](const Boundary& boundary) {
                QPainterPath path;path.moveTo(screen(boundary.front().start));
                for(const auto& edge:boundary) {
                    if(edge.sweep_radians==0) {path.lineTo(screen(edge.end));continue;}
                    const auto dx=edge.end.x-edge.start.x,dy=edge.end.y-edge.start.y;
                    const auto chord=std::hypot(dx,dy);const auto half=edge.sweep_radians*.5;
                    const auto semicircle=std::abs(edge.sweep_radians)==std::numbers::pi;
                    const auto offset=semicircle ? 0.0 : chord/(2*std::tan(half));
                    const Vec2 center{std::midpoint(edge.start.x,edge.end.x)-dy/chord*offset,
                                      std::midpoint(edge.start.y,edge.end.y)+dx/chord*offset};
                    const auto radius=chord/(2*(semicircle ? 1.0 : std::sin(std::abs(half))))*scale;
                    const auto mapped=screen(center);
                    require(std::isfinite(mapped.x()) && std::isfinite(mapped.y()) && std::isfinite(radius) && radius>0,"Preview arc is not representable");
                    const auto angle=std::atan2(edge.start.y-center.y,edge.start.x-center.x)*180/std::numbers::pi;
                    path.arcTo(QRectF(mapped.x()-radius,mapped.y()-radius,2*radius,2*radius),angle,edge.sweep_radians*180/std::numbers::pi);
                }
                path.closeSubpath();return path;
            };
            auto fill=path_for(profile_.outer);fill.setFillRule(Qt::OddEvenFill);
            for(const auto& hole:profile_.holes)fill.addPath(path_for(hole));
            auto tint=palette().highlight().color();tint.setAlpha(45);
            painter.setPen(QPen(palette().text().color(),1.5));painter.setBrush(tint);painter.drawPath(fill);
            const auto& edited=hole_ ? profile_.holes.at(*hole_) : profile_.outer;
            painter.setBrush(Qt::NoBrush);painter.setPen(QPen(palette().highlight().color(),2));painter.drawPath(path_for(edited));
            for(std::size_t i=0;i<edited.size();++i) {
                const auto point=screen(edited[i].start);const auto selected=static_cast<int>(i)==vertex_;
                painter.setBrush(selected ? palette().highlight() : palette().base());
                painter.drawEllipse(point,selected ? 5 : 3,selected ? 5 : 3);
                if(selected){painter.setPen(palette().text().color());painter.drawText(point+QPointF(8,-8),QString::number(i+1));painter.setPen(QPen(palette().highlight().color(),2));}
            }
        } catch(const std::exception& e) {
            painter.setPen(palette().text().color());painter.drawText(rect().adjusted(12,12,-12,-12),Qt::AlignCenter|Qt::TextWordWrap,"Preview unavailable: "+q(e.what()));
        }
    }
private:
    AssemblyProfile profile_;
    std::optional<std::size_t> hole_;
    int vertex_{-1};
    QString message_{"Enter valid profile geometry to preview."};
};

std::optional<Boundary> edit_profile_boundary(QWidget* parent,const AssemblyProfile& retained,
                                             std::optional<std::size_t> hole,bool metric) {
    const auto& original=hole ? retained.holes.at(*hole) : retained.outer;
    std::size_t other_segments=hole ? retained.outer.size() : 0;
    for(std::size_t i=0;i<retained.holes.size();++i)if(!hole || i!=*hole)other_segments+=retained.holes[i].size();
    require(other_segments<=1024 && original.size()<=1024-other_segments,"Profile outer and cutouts exceed the segment limit");
    QDialog dialog(parent);dialog.setObjectName("assemblyProfileGeometryDialog");
    dialog.setWindowTitle(hole ? "Edit cutout geometry" : "Edit outline geometry");dialog.resize(900,590);
    auto* layout=new QVBoxLayout(&dialog);
    auto* note=new QLabel("Coordinates are local to this profile. Each vertex starts a segment ending at the next vertex; the last closes to the first. Zero sweep is straight; positive sweep turns counter-clockwise. Removing or moving a vertex reconnects segments and keeps each outgoing sweep with its vertex.",&dialog);
    note->setWordWrap(true);layout->addWidget(note);
    layout->addWidget(new QLabel(metric ? "Bare X/Y lengths use metres; unit suffixes are supported. Sweeps use degrees, deg or rad." : "Bare X/Y lengths use feet; unit suffixes are supported. Sweeps use degrees, deg or rad.",&dialog));
    auto* body=new QHBoxLayout;auto* editor=new QWidget(&dialog);auto* editor_layout=new QVBoxLayout(editor);editor_layout->setContentsMargins(0,0,0,0);
    auto* rows=table(editor,"assemblyProfileVertices",{"Vertex X","Vertex Y","Outgoing sweep (degrees)"});editor_layout->addWidget(rows,1);
    rows->verticalHeader()->setSectionsMovable(true);
    auto* preview=new ProfileGeometryPreview(&dialog);body->addWidget(editor,3);body->addWidget(preview,2);layout->addLayout(body,1);
    auto add_vertex=[&](Vec2 point,double sweep,int source_index) {
        const auto row=append_row(rows);
        rows->setCellWidget(row,0,number(rows,{},point.x,true));rows->cellWidget(row,0)->setProperty("originalIndex",source_index);
        rows->setCellWidget(row,1,number(rows,{},point.y,true));rows->setCellWidget(row,2,number(rows,{},sweep,false,true));
        rows->cellWidget(row,2)->setToolTip("Outgoing signed sweep: 0 is straight, positive is counter-clockwise; magnitude must be less than 360 degrees.");
        return row;
    };
    for(std::size_t i=0;i<original.size();++i)add_vertex(original[i].start,original[i].sweep_radians,static_cast<int>(i));
    auto read=[&] {
        require(rows->rowCount()>0,"Add vertices for a closed boundary");
        require(static_cast<std::size_t>(rows->rowCount())<=1024-other_segments,"Profile outer and cutouts exceed the segment limit");
        bool same_topology=static_cast<std::size_t>(rows->rowCount())==original.size();
        std::vector<Vec2> vertices;std::vector<double> sweeps;
        for(int visual=0;visual<rows->rowCount();++visual) {
            const auto row=rows->verticalHeader()->logicalIndex(visual);
            same_topology=same_topology && rows->cellWidget(row,0)->property("originalIndex").toInt()==visual;
            vertices.push_back({read_number(qobject_cast<QLineEdit*>(rows->cellWidget(row,0)),metric,true),read_number(qobject_cast<QLineEdit*>(rows->cellWidget(row,1)),metric,true)});
            sweeps.push_back(read_number(qobject_cast<QLineEdit*>(rows->cellWidget(row,2)),metric,false,true));
        }
        // Tolerance-connected endpoints can differ from the next vertex. Keep
        // their exact authored values for numeric-equivalent and sweep-only
        // edits. Moving a vertex changes only its two incident endpoints.
        if(same_topology) {
            auto result=original;
            for(std::size_t i=0;i<vertices.size();++i) {
                if(vertices[i].x!=original[i].start.x || vertices[i].y!=original[i].start.y) {
                    result[i].start=vertices[i];result[(i+vertices.size()-1)%vertices.size()].end=vertices[i];
                }
                if(sweeps[i]!=original[i].sweep_radians)result[i].sweep_radians=sweeps[i];
            }
            return result;
        }
        // A changed roster/order explicitly authors new closed connectivity.
        Boundary result;result.reserve(vertices.size());
        for(std::size_t i=0;i<vertices.size();++i)result.push_back({vertices[i],vertices[(i+1)%vertices.size()],sweeps[i]});
        return result;
    };
    auto profile_with=[&](const Boundary& boundary){auto result=retained;if(hole)result.holes.at(*hole)=boundary;else result.outer=boundary;return result;};
    QLabel* inline_error{};std::optional<Boundary> accepted;
    auto validate=[&] {
        auto boundary=read();const auto diagnostics=validate_boundary(boundary);
        if(!diagnostics.empty())throw std::invalid_argument("Segment "+std::to_string(diagnostics.front().segment_index+1)+": "+diagnostics.front().message);
        auto profile=profile_with(boundary);validate_profile_geometry(profile);return std::pair{std::move(boundary),std::move(profile)};
    };
    auto refresh=[&] {
        for(int visual=0;visual<rows->rowCount();++visual)rows->setVerticalHeaderItem(rows->verticalHeader()->logicalIndex(visual),new QTableWidgetItem(QString::number(visual+1)));
        try {
            auto [boundary,profile]=validate();(void)boundary;inline_error->clear();
            preview->show_profile(std::move(profile),hole,rows->currentRow()<0 ? -1 : rows->verticalHeader()->visualIndex(rows->currentRow()));
        } catch(const std::exception& e){inline_error->setText(q(e.what()));preview->show_error("Invalid draft: "+q(e.what()));}
    };
    auto* timer=new QTimer(&dialog);timer->setSingleShot(true);timer->setInterval(250);
    QObject::connect(timer,&QTimer::timeout,&dialog,refresh);
    auto connect_inputs=[&](int row){for(int column=0;column<3;++column)QObject::connect(qobject_cast<QLineEdit*>(rows->cellWidget(row,column)),&QLineEdit::textChanged,&dialog,[timer]{timer->start();});};
    for(int row=0;row<rows->rowCount();++row)connect_inputs(row);
    auto* controls=new QHBoxLayout;
    auto* add=button(editor,controls,"assemblyProfileVertexAdd","Insert vertex");
    add->setToolTip("Split the selected outgoing line or arc at its midpoint, retaining its circular sweep.");
    QObject::connect(add,&QPushButton::clicked,&dialog,[&]{
        try {
            require(static_cast<std::size_t>(rows->rowCount())<1024-other_segments,"Profile outer and cutouts exceed the segment limit");
            const auto boundary=read();const auto selected=rows->currentRow()<0 ? rows->rowCount()-1 : rows->verticalHeader()->visualIndex(rows->currentRow());
            const auto& edge=boundary.at(selected);Vec2 point{std::midpoint(edge.start.x,edge.end.x),std::midpoint(edge.start.y,edge.end.y)};
            if(edge.sweep_radians!=0) {
                (void)arc_from_chord_angle(edge.start,edge.end,edge.sweep_radians);
                const auto dx=edge.end.x-edge.start.x,dy=edge.end.y-edge.start.y;const auto chord=std::hypot(dx,dy);
                const auto sagitta=chord*.5*std::tan(edge.sweep_radians*.25);point.x+=dy/chord*sagitta;point.y-=dx/chord*sagitta;
            }
            require(std::isfinite(point.x) && std::isfinite(point.y),"Inserted vertex is not representable");
            auto* sweep=qobject_cast<QLineEdit*>(rows->cellWidget(rows->verticalHeader()->logicalIndex(selected),2));
            const auto new_row=add_vertex(point,edge.sweep_radians*.5,-1);connect_inputs(new_row);
            const auto sweep_text=exact(edge.sweep_radians*.5*180/std::numbers::pi);
            sweep->setText(sweep_text);sweep->setProperty("initialText",sweep_text);sweep->setProperty("initialValue",edge.sweep_radians*.5);
            auto* header=rows->verticalHeader();header->moveSection(header->visualIndex(new_row),selected+1);rows->setCurrentCell(new_row,0);refresh();
        } catch(const std::exception& e){inline_error->setText(q(e.what()));}
    });
    auto* remove=button(editor,controls,"assemblyProfileVertexRemove","Remove selected");
    QObject::connect(remove,&QPushButton::clicked,&dialog,[&]{
        if(rows->currentRow()<0)return;
        if(rows->rowCount()<=2){inline_error->setText("Keep at least two vertices; a circle can use two semicircular segments.");return;}
        rows->removeRow(rows->currentRow());refresh();
    });
    for(const auto& direction : {std::pair{"Up",-1},std::pair{"Down",1}}) {
        auto* move=button(editor,controls,QString("assemblyProfileVertex")+direction.first,direction.second<0 ? "Move up" : "Move down");
        QObject::connect(move,&QPushButton::clicked,&dialog,[&,step=direction.second]{
            if(rows->currentRow()<0)return;auto* header=rows->verticalHeader();const auto from=header->visualIndex(rows->currentRow()),to=from+step;
            if(to>=0 && to<rows->rowCount()){header->moveSection(from,to);refresh();}
        });
    }
    editor_layout->addLayout(controls);
    QObject::connect(rows,&QTableWidget::currentCellChanged,&dialog,[timer]{timer->start();});
    QObject::connect(rows->verticalHeader(),&QHeaderView::sectionMoved,&dialog,[timer]{timer->start();});
    finish_dialog(&dialog,layout,inline_error,[&]{
        try {auto result=validate();accepted=std::move(result.first);dialog.accept();}
        catch(const std::exception& e){inline_error->setText(q(e.what()));accepted.reset();}
    });
    rows->setCurrentCell(0,0);refresh();
    if(dialog.exec()==QDialog::Accepted)return accepted;
    return std::nullopt;
}
}

class AssemblyTypeDialog::Impl final {
public:
    AssemblyTypeDialog* owner;
    DocumentSnapshot source;
    std::string catalog_id;
    AssemblyModel model;
    AssemblyType original;
    bool editing{},metric{};
    QLineEdit* name{};
    QTableWidget *profiles{},*parts{},*impacts{};
    QLabel* error{};
    std::unique_ptr<NamedValues> defaults;
    std::map<std::string,Boundary> outlines;
    std::map<std::string,std::vector<Boundary>> outline_holes;
    std::map<std::string,AssemblyProfile> profile_drafts;
    std::map<std::string,AssemblyPart> part_drafts;
    std::vector<std::pair<QString,std::string>> outline_options;
    std::optional<AssemblyTypeDraft> preview,candidate;

    Impl(AssemblyTypeDialog* dialog, DocumentSnapshot captured, std::string catalog,
         std::optional<AssemblyType> expected, bool metric_units)
        : owner(dialog),source(std::move(captured)),catalog_id(std::move(catalog)),model(model_for(source,catalog_id)),
          original(expected.value_or(AssemblyType{})),editing(expected.has_value()),metric(metric_units) {
        require(source.is_editable(),"Assembly source is read-only");
        if(editing)require(find_type(model,original.id)==original,"Expected assembly type differs from captured source");
        else original.id=make_stable_id();
        owner->setObjectName("assemblyTypeAuthoringDialog"); owner->setWindowTitle(editing ? "Edit assembly type" : "Create assembly type"); owner->resize(940,780);
        auto* layout=new QVBoxLayout(owner); auto* heading=new QFormLayout;
        name=field(owner,"assemblyTypeName",q(original.name)); heading->addRow("Type name",name); layout->addLayout(heading);
        auto* units=new QLabel(metric ? "Lengths accept m, mm, cm, ft and in; bare values use metres." : "Lengths accept m, mm, cm, ft and in; bare values use feet.",owner);
        layout->addWidget(units);
        auto* tabs=new QTabWidget(owner); layout->addWidget(tabs,1);
        defaults=std::make_unique<NamedValues>(owner,"assemblyType",model); defaults->set(original.properties,original.materials,original.quantities); tabs->addTab(defaults->tabs,"Defaults");
        for(const auto& [id,entity]:source.entities()) {
            if(!can_recognize_boundary_entity_type(entity.type))continue;
            try {auto b=captured_boundary(entity);auto holes=captured_holes(entity,source,b);
                outlines.emplace(id,b);outline_holes.emplace(id,std::move(holes));outline_options.push_back({label(entity,QString("Closed outline %1").arg(outline_options.size()+1)),id});
            } catch(const std::exception&) { /* Unsupported or invalid outlines cannot lend geometry. */ }
        }
        auto* profile_page=new QWidget(tabs); auto* profile_layout=new QVBoxLayout(profile_page);
        profiles=table(profile_page,"assemblyProfiles",{"Closed outline","Height","Base elevation","Material slot"}); profile_layout->addWidget(profiles);
        row_controls(profile_page,profile_layout,profiles,"assemblyProfile",[this]{guard([&]{add_profile(std::nullopt);});},true);
        profile_page->findChild<QPushButton*>("assemblyProfileAdd")->setObjectName("assemblyAddProfile");
        auto* profile_actions=new QHBoxLayout;
        auto* outline=button(profile_page,profile_actions,"assemblyProfileEditOutline","Edit outline…");
        QObject::connect(outline,&QPushButton::clicked,owner,[this]{guard([&]{edit_outline();});});
        auto* cutouts=button(profile_page,profile_actions,"assemblyProfileCutouts","Cutouts…");
        QObject::connect(cutouts,&QPushButton::clicked,owner,[this]{guard([&]{edit_cutouts();});});
        auto* reimport=button(profile_page,profile_actions,"assemblyProfileReimport","Reimport selected boundary");
        QObject::connect(reimport,&QPushButton::clicked,owner,[this]{guard([&]{
            require(profiles->currentRow()>=0,"Select a profile first");auto* choice=qobject_cast<QComboBox*>(profiles->cellWidget(profiles->currentRow(),0));import_profile(choice);
        });});
        profile_layout->addLayout(profile_actions);
        tabs->addTab(profile_page,"Solid profiles");
        for(const auto& p:original.profiles)add_profile(p);
        auto* parts_page=new QWidget(tabs); auto* parts_layout=new QVBoxLayout(parts_page);
        parts=table(parts_page,"assemblyParts",{"Child type","X","Y","Z","Yaw (degrees)","Width/depth scale","Height scale"}); parts_layout->addWidget(parts);
        row_controls(parts_page,parts_layout,parts,"assemblyPart",[this]{guard([&]{add_part(std::nullopt);});},true);
        parts_page->findChild<QPushButton*>("assemblyPartAdd")->setObjectName("assemblyAddPart"); tabs->addTab(parts_page,"Nested parts");
        auto* part_action=new QPushButton("Part overrides…",parts_page);part_action->setObjectName("assemblyPartOverrides");parts_layout->addWidget(part_action);
        QObject::connect(part_action,&QPushButton::clicked,owner,[this]{guard([&]{edit_part_overrides();});});
        for(const auto& p:original.parts)add_part(p);
        auto* impact_page=new QWidget(tabs); auto* impact_layout=new QVBoxLayout(impact_page);
        auto* note=new QLabel("Preview lists every directly or transitively affected instance and its retained explicit overrides. Review it before saving a type edit.",impact_page); note->setWordWrap(true); impact_layout->addWidget(note);
        impacts=table(impact_page,"assemblyUpdateImpacts",{"Affected instance","Geometry before / after","Retained explicit overrides","Changed resolved values"}); impacts->setEditTriggers(QAbstractItemView::NoEditTriggers); impact_layout->addWidget(impacts);
        auto* preview_button=new QPushButton("Preview type update",impact_page); preview_button->setObjectName("assemblyPreviewTypeUpdate"); impact_layout->addWidget(preview_button);
        QObject::connect(preview_button,&QPushButton::clicked,owner,[this,tabs,impact_page]{if(owner->previewUpdate())tabs->setCurrentWidget(impact_page);});
        tabs->addTab(impact_page,"Update impact");
        finish_dialog(owner,layout,error,[this]{(void)owner->submit();});
    }
    template<class F> bool guard(F action) {
        try {action();error->clear();return true;} catch(const std::exception& e){error->setText(q(e.what()));candidate.reset();return false;}
    }
    void add_profile(std::optional<AssemblyProfile> value) {
        if(!value)require(!outline_options.empty(),"Create a valid closed boundary before adding an assembly profile");
        AssemblyProfile profile=value.value_or(AssemblyProfile{}); if(!value){profile.id=make_stable_id();profile.height_m=1;}
        auto row=append_row(profiles); auto* choice=new QComboBox(profiles);
        for(const auto& [text,id]:outline_options)choice->addItem(text,q(id));
        if(value) {
            auto key="stored:"+profile.id;while(outlines.contains(key))key+=':';outlines.emplace(key,profile.outer);
            outline_holes.emplace(key,profile.holes);
            choice->addItem(QString("Stored profile %1 (%2 edges)").arg(row+1).arg(profile.outer.size()),q(key)); choice->setCurrentIndex(choice->count()-1);
        }
        if(!value){profile.outer=outlines.at(choice->currentData().toString().toStdString());profile.holes=outline_holes.at(choice->currentData().toString().toStdString());}
        profile_drafts.emplace(profile.id,profile);
        choice->setProperty("stableId",q(profile.id)); profiles->setCellWidget(row,0,choice);
        profiles->setCellWidget(row,1,number(profiles,{},profile.height_m,true)); profiles->setCellWidget(row,2,number(profiles,{},profile.elevation_m,true));
        profiles->setCellWidget(row,3,field(profiles,{},profile.material_slot ? q(*profile.material_slot) : QString{}));
        QObject::connect(choice,&QComboBox::currentIndexChanged,owner,[this,choice]{guard([&]{import_profile(choice);});});
    }
    void add_part(std::optional<AssemblyPart> value) {
        AssemblyPart part=value.value_or(AssemblyPart{}); if(!value)part.id=make_stable_id();
        auto row=append_row(parts); auto* choice=new QComboBox(parts);
        for(const auto& type:model.types())choice->addItem(q(type.name),q(type.id));
        // Self is offered to make refusal explicit; shared graph validation owns cycles.
        if(choice->findData(q(original.id))<0)choice->addItem("This type",q(original.id));
        if(value)choice->setCurrentIndex(choice->findData(q(part.type_id))); choice->setProperty("stableId",q(part.id));
        if(!value)part.type_id=choice->currentData().toString().toStdString();part_drafts.emplace(part.id,part);
        parts->setCellWidget(row,0,choice);
        const std::array<double,6> values{part.transform.translation_m.x,part.transform.translation_m.y,part.transform.translation_m.z,part.transform.rotation_radians,part.transform.scale,part.transform.scale*part.transform.vertical_scale};
        for(int i=0;i<6;++i)parts->setCellWidget(row,i+1,number(parts,{},values[i],i<3,i==3));
    }
    void import_profile(QComboBox* choice) {
        require(choice && choice->currentIndex()>=0,"Select a captured boundary to import");
        auto& draft=profile_drafts.at(choice->property("stableId").toString().toStdString());const auto key=choice->currentData().toString().toStdString();
        draft.outer=outlines.at(key);draft.holes=outline_holes.at(key);
        choice->setToolTip(QString("%1 edges; %2 local cutouts").arg(draft.outer.size()).arg(draft.holes.size()));
        invalidate_geometry_preview();
    }
    void invalidate_geometry_preview() {
        preview.reset();candidate.reset();impacts->setRowCount(0);
    }
    void edit_outline() {
        require(profiles->currentRow()>=0,"Select a profile first");
        auto* choice=qobject_cast<QComboBox*>(profiles->cellWidget(profiles->currentRow(),0));
        require(choice && choice->currentIndex()>=0,"Choose a profile first");
        auto& profile=profile_drafts.at(choice->property("stableId").toString().toStdString());
        const auto retained=profile;
        if(auto boundary=edit_profile_boundary(owner,retained,std::nullopt,metric)) {
            profile.outer=std::move(*boundary);
            if(profile!=retained) {
                invalidate_geometry_preview();
                choice->setToolTip(QString("Edited local geometry: %1 edges; %2 cutouts. Reimport replaces local edits with the selected boundary.").arg(profile.outer.size()).arg(profile.holes.size()));
            }
        }
    }
    void edit_part_overrides() {
        require(parts->currentRow()>=0,"Select a child part first");
        auto* choice=qobject_cast<QComboBox*>(parts->cellWidget(parts->currentRow(),0));require(choice && choice->currentIndex()>=0,"Choose a child type first");
        const auto id=choice->property("stableId").toString().toStdString();const auto type_id=choice->currentData().toString().toStdString();
        const auto& child=find_type(model,type_id);const auto retained=part_drafts.at(id);
        QDialog dialog(owner);dialog.setObjectName("assemblyPartOverridesDialog");dialog.setWindowTitle("Overrides for "+q(child.name));dialog.resize(760,510);
        auto* layout=new QVBoxLayout(&dialog);auto* inherited=new QLabel(&dialog);inherited->setWordWrap(true);inherited->setObjectName("assemblyPartOverrideDefaults");
        AssemblyInstance defaults_view;defaults_view.type_id=child.id;defaults_view.property_overrides=child.properties;defaults_view.material_overrides=child.materials;defaults_view.quantity_overrides=child.quantities;
        inherited->setText("Child type defaults\n"+overrides(defaults_view,model));layout->addWidget(inherited);
        NamedValues values(&dialog,"assemblyPartOverride",model);values.set(retained.property_overrides,retained.material_overrides,retained.quantity_overrides);layout->addWidget(values.tabs,1);
        auto* clear=new QPushButton("Clear all explicit overrides",&dialog);clear->setObjectName("assemblyClearPartOverrides");layout->addWidget(clear);
        QObject::connect(clear,&QPushButton::clicked,&dialog,[&]{values.set({},{},{});});
        QLabel* inline_error{};std::optional<AssemblyPart> accepted;
        finish_dialog(&dialog,layout,inline_error,[&]{
            try {
                auto candidate=retained;candidate.type_id=type_id;candidate.property_overrides=values.read_properties();candidate.material_overrides=values.read_materials();candidate.quantity_overrides=values.read_quantities();
                AssemblyInstance probe;probe.id="part-override-validation";probe.type_id=type_id;probe.property_overrides=candidate.property_overrides;probe.material_overrides=candidate.material_overrides;probe.quantity_overrides=candidate.quantity_overrides;
                AssemblyExpansionBudget budget;(void)model.expand(probe,budget);accepted=std::move(candidate);dialog.accept();
            } catch(const std::exception& e){inline_error->setText(q(e.what()));accepted.reset();}
        });
        if(dialog.exec()==QDialog::Accepted && accepted) {
            part_drafts.insert_or_assign(id,*accepted);AssemblyInstance summary;summary.type_id=type_id;summary.property_overrides=accepted->property_overrides;summary.material_overrides=accepted->material_overrides;summary.quantity_overrides=accepted->quantity_overrides;
            choice->setToolTip(overrides(summary,model));
        }
    }
    void edit_cutouts() {
        require(profiles->currentRow()>=0,"Select a profile first");auto* profile_choice=qobject_cast<QComboBox*>(profiles->cellWidget(profiles->currentRow(),0));
        require(profile_choice && profile_choice->currentIndex()>=0,"Choose a profile first");
        const auto id=profile_choice->property("stableId").toString().toStdString();const auto retained=profile_drafts.at(id);
        QDialog dialog(owner);dialog.setObjectName("assemblyProfileCutoutsDialog");dialog.setWindowTitle("Profile cutouts");dialog.resize(740,470);
        auto* layout=new QVBoxLayout(&dialog);auto* note=new QLabel("Choose or edit closed cutouts in profile-local coordinates. Cutouts must lie strictly inside the outer boundary and remain disjoint. Save retains the roster and local geometry; Cancel preserves the profile.",&dialog);note->setWordWrap(true);layout->addWidget(note);
        auto* rows=table(&dialog,"assemblyProfileCutoutRows",{"Closed cutout boundary","Local geometry"});layout->addWidget(rows,1);
        std::map<std::string,Boundary> choices=outlines;
        QLabel* inline_error{};std::optional<std::vector<Boundary>> accepted;
        auto read_holes=[&] {
            std::vector<Boundary> result;std::size_t segments=retained.outer.size();
            require(segments<=1024,"Profile outer and cutouts exceed the segment limit");
            for(int visual=0;visual<rows->rowCount();++visual) {
                const auto row=rows->verticalHeader()->logicalIndex(visual);auto* choice=qobject_cast<QComboBox*>(rows->cellWidget(row,0));
                require(choice && choice->currentIndex()>=0,"Choose a cutout boundary");const auto& boundary=choices.at(choice->currentData().toString().toStdString());
                require(boundary.size()<=1024-segments,"Profile outer and cutouts exceed the segment limit");segments+=boundary.size();result.push_back(boundary);
            }
            return result;
        };
        auto add=[&](std::optional<Boundary> stored){
            if(!stored)require(!outline_options.empty(),"Create a valid closed boundary before adding a cutout");
            const auto row=append_row(rows);auto* choice=new QComboBox(rows);for(const auto& [name,key]:outline_options)choice->addItem(name,q(key));
            if(stored){auto key="cutout:"+std::to_string(choices.size());while(choices.contains(key))key+=':';choices.emplace(key,*stored);choice->addItem(QString("Stored cutout %1 (%2 edges)").arg(row+1).arg(stored->size()),q(key));choice->setCurrentIndex(choice->count()-1);}
            rows->setCellWidget(row,0,choice);
            auto* edit=new QPushButton("Edit cutout…",rows);edit->setObjectName("assemblyProfileEditCutout");rows->setCellWidget(row,1,edit);
            QObject::connect(edit,&QPushButton::clicked,&dialog,[&,choice]{
                try {
                    int selected=-1;
                    for(int logical=0;logical<rows->rowCount();++logical)if(rows->cellWidget(logical,0)==choice){selected=rows->verticalHeader()->visualIndex(logical);break;}
                    require(selected>=0,"Select a cutout first");auto profile=retained;profile.holes=read_holes();
                    if(auto geometry=edit_profile_boundary(&dialog,profile,static_cast<std::size_t>(selected),metric)) {
                        // A private row choice keeps other rows using the same
                        // imported boundary independent, even after reordering.
                        auto key="edited-cutout:"+std::to_string(choices.size());while(choices.contains(key))key+=':';
                        choices.emplace(key,std::move(*geometry));choice->addItem(QString("Edited local cutout (%1 edges)").arg(choices.at(key).size()),q(key));choice->setCurrentIndex(choice->count()-1);
                        inline_error->clear();
                    }
                } catch(const std::exception& e){inline_error->setText(q(e.what()));}
            });
        };
        row_controls(&dialog,layout,rows,"assemblyProfileCutoutRows",[&]{add(std::nullopt);},true);
        for(const auto& hole:retained.holes)add(hole);
        finish_dialog(&dialog,layout,inline_error,[&]{
            try {
                auto profile=retained;profile.holes=read_holes();validate_profile_geometry(profile);
                accepted=std::move(profile.holes);dialog.accept();
            } catch(const std::exception& e){inline_error->setText(q(e.what()));accepted.reset();}
        });
        if(dialog.exec()==QDialog::Accepted && accepted) {
            auto& profile=profile_drafts.at(id);profile.holes=std::move(*accepted);
            if(profile!=retained)invalidate_geometry_preview();
            profile_choice->setToolTip(QString("%1 edges; %2 local cutouts. Reimport replaces local edits with the selected boundary.").arg(profile.outer.size()).arg(profile.holes.size()));
        }
    }
    AssemblyType read() const {
        auto result=original; result.name=name->text().trimmed().toStdString(); require(!result.name.empty(),"Enter an assembly type name");
        result.properties=defaults->read_properties(); result.materials=defaults->read_materials(); result.quantities=defaults->read_quantities(); result.profiles.clear(); result.parts.clear();
        for(int visual=0;visual<profiles->rowCount();++visual) {
            const auto row=profiles->verticalHeader()->logicalIndex(visual); auto* choice=qobject_cast<QComboBox*>(profiles->cellWidget(row,0));
            require(choice && choice->currentIndex()>=0,"Choose a closed profile outline"); auto id=choice->property("stableId").toString().toStdString();
            auto profile=profile_drafts.at(id);profile.id=id;
            profile.height_m=read_number(qobject_cast<QLineEdit*>(profiles->cellWidget(row,1)),metric,true);
            profile.elevation_m=read_number(qobject_cast<QLineEdit*>(profiles->cellWidget(row,2)),metric,true);
            const auto slot=qobject_cast<QLineEdit*>(profiles->cellWidget(row,3))->text().trimmed().toStdString(); profile.material_slot=slot.empty() ? std::nullopt : std::optional{slot};
            result.profiles.push_back(std::move(profile));
        }
        for(int visual=0;visual<parts->rowCount();++visual) {
            const auto row=parts->verticalHeader()->logicalIndex(visual); auto* choice=qobject_cast<QComboBox*>(parts->cellWidget(row,0));
            require(choice && choice->currentIndex()>=0,"Choose an available child type"); auto id=choice->property("stableId").toString().toStdString();
            auto part=part_drafts.at(id);part.id=id;part.type_id=choice->currentData().toString().toStdString();
            TransformFields transform; transform.mirrored_y=part.transform.mirrored_y; transform.initial_transform=part.transform; for(int i=0;i<6;++i)transform.fields[i]=qobject_cast<QLineEdit*>(parts->cellWidget(row,i+1)); part.transform=transform.read(metric); result.parts.push_back(std::move(part));
        }
        require(!result.profiles.empty() || !result.parts.empty(),"Add a solid profile or a nested part"); return result;
    }
    AssemblyTypeDraft validated(AssemblyType replacement) const {
        auto updated=[&]{
            if(editing)return model.with_type(replacement);
            auto types=model.types();types.push_back(replacement);return AssemblyModel::create(model.materials(),std::move(types),model.instances());
        }();
        AssemblyInstance probe; probe.id="authoring-preview";probe.type_id=replacement.id;
        AssemblyExpansionBudget budget;require(!updated.expand(probe,budget).profiles.empty(),"Assembly type must contain usable solid geometry");
        AssemblyTypeDraft result{source.revision(),source.entities().at(catalog_id),replacement,{}, {}};
        if(editing) {result.catalog_impacts=model.preview_type_update(replacement);result.document_impacts=preview_document_assembly_type_update(source.entities(),catalog_id,replacement);}
        // Includes every catalog/independent instance in one bounded validation.
        auto entities=source.entities();entities.at(catalog_id).properties["model"]=updated.to_json();validate_document_assembly_instances(entities); return result;
    }
    void show_impacts(const AssemblyTypeDraft& draft) {
        impacts->setRowCount(0);
        // A new type has no impacted instances. Edited paths are labeled using
        // the replacement graph, while removed paths keep their old names.
        const auto updated=editing ? model.with_type(draft.replacement) : model;
        auto add=[&](QString name,const AssemblyExpansion& before,const AssemblyExpansion& after,const AssemblyInstance& retained) {
            auto row=append_row(impacts);impacts->setItem(row,0,new QTableWidgetItem(name));
            impacts->setItem(row,1,new QTableWidgetItem(QString("%1 → %2 solids\n%3 → %4 m³").arg(before.profiles.size()).arg(after.profiles.size()).arg(exact(before.volume_m3),exact(after.volume_m3))));
            impacts->setItem(row,2,new QTableWidgetItem(overrides(retained,model)));impacts->item(row,2)->setToolTip(overrides(retained,model));
            const auto changes=resolved_changes(before,after,model,updated);impacts->setItem(row,3,new QTableWidgetItem(changes));impacts->item(row,3)->setToolTip(changes);
        };
        int ordinal=0; for(const auto& impact:draft.catalog_impacts)add(QString("Catalog instance %1").arg(++ordinal),impact.before_expansion,impact.after_expansion,impact.retained_overrides);
        ordinal=0;for(const auto& impact:draft.document_impacts)add(label(source.entities().at(impact.entity_id),QString("Placed assembly %1").arg(++ordinal)),impact.before,impact.after,impact.retained_overrides.instance);
        impacts->resizeRowsToContents();
    }
};

AssemblyTypeDialog::AssemblyTypeDialog(DocumentSnapshot source,std::string catalog_id,std::optional<AssemblyType> expected_type,bool metric_units,QWidget* parent)
    : QDialog(parent),m_impl(std::make_unique<Impl>(this,std::move(source),std::move(catalog_id),std::move(expected_type),metric_units)) {}
AssemblyTypeDialog::~AssemblyTypeDialog()=default;
bool AssemblyTypeDialog::previewUpdate() { return m_impl->guard([&]{m_impl->preview=m_impl->validated(m_impl->read());m_impl->show_impacts(*m_impl->preview);}); }
bool AssemblyTypeDialog::submit() {
    return m_impl->guard([&]{
        auto draft=m_impl->validated(m_impl->read());
        if(m_impl->editing && draft.replacement!=m_impl->original &&
           (!m_impl->preview || m_impl->preview->replacement!=draft.replacement)) {
            m_impl->preview=std::move(draft);m_impl->show_impacts(*m_impl->preview);
            auto* tabs=m_impl->impacts->parentWidget();auto* outer=qobject_cast<QTabWidget*>(tabs->parentWidget()->parentWidget());
            if(outer)outer->setCurrentWidget(tabs);
            throw std::invalid_argument("Review the updated impact preview, then Save again");
        }
        m_impl->candidate=std::move(draft);accept();
    });
}
std::optional<AssemblyTypeDraft> AssemblyTypeDialog::candidate() const {return result()==QDialog::Accepted ? m_impl->candidate : std::nullopt;}
QString AssemblyTypeDialog::lastError() const {return m_impl->error->text();}

class AssemblyInstanceDialog::Impl final {
public:
    AssemblyInstanceDialog* owner;
    DocumentSnapshot source;
    std::optional<Entity> original;
    std::map<std::string,AssemblyModel> catalogs;
    QComboBox *catalog{},*type{},*nested_choice{};
    QLineEdit* name{};
    QLabel *error{},*nested_defaults{};
    QCheckBox* nested_transform_enabled{};
    TransformFields root_transform,nested_transform;
    std::unique_ptr<NamedValues> root_values,nested_values;
    QWidget *root_values_host{},*nested_values_host{};
    AssemblyInstance instance;
    std::vector<std::vector<std::string>> paths;
    int selected_path{-1};
    bool metric{},loading{};
    std::optional<AssemblyInstanceDraft> candidate;

    Impl(AssemblyInstanceDialog* dialog,DocumentSnapshot captured,std::optional<Entity> expected,std::string preferred,bool metric_units)
        :owner(dialog),source(std::move(captured)),original(std::move(expected)),metric(metric_units) {
        require(source.is_editable(),"Assembly source is read-only");
        if(original) {
            auto found=source.entities().find(original->id);require(found!=source.entities().end() && found->second==*original,"Expected assembly instance differs from captured source");
            auto value=decode_document_assembly_instance(*original);instance=value.instance;preferred=value.assembly_catalog_id;
        } else {instance.id=make_stable_id();instance.root_transform=AssemblyTransform{};}
        for(const auto& [id,entity]:source.entities())if(entity.type=="assembly_model")catalogs.emplace(id,model_for(source,id));
        require(!catalogs.empty(),"Create an assembly catalog with solid geometry first");
        owner->setObjectName("assemblyInstanceAuthoringDialog");owner->setWindowTitle(original ? "Edit placed assembly" : "Place assembly");owner->resize(1000,820);
        auto* layout=new QVBoxLayout(owner);auto* form=new QFormLayout;
        catalog=new QComboBox(owner);catalog->setObjectName("assemblyInstanceCatalog");int ordinal=0;
        for(const auto& [id,model]:catalogs){(void)model;catalog->addItem(label(source.entities().at(id),QString("Assembly catalog %1").arg(++ordinal)),q(id));}
        if(!preferred.empty()){const auto index=catalog->findData(q(preferred));require(index>=0,"Preferred assembly catalog is unavailable");catalog->setCurrentIndex(index);}
        type=new QComboBox(owner);type->setObjectName("assemblyInstanceType");
        name=field(owner,"assemblyInstanceName",original ? label(*original,{}) : QString{});
        form->addRow("Catalog",catalog);form->addRow("Type",type);form->addRow("Instance name",name);layout->addLayout(form);
        auto* units=new QLabel(metric ? "Bare lengths use metres; unit suffixes and fractional feet/inches are supported." : "Bare lengths use feet; unit suffixes and fractional feet/inches are supported.",owner);layout->addWidget(units);
        auto* tabs=new QTabWidget(owner);layout->addWidget(tabs,1);
        auto* root_page=new QWidget(tabs);auto* root_layout=new QVBoxLayout(root_page);auto* transform_form=new QFormLayout;
        root_transform=TransformFields::form(root_page,transform_form,"assemblyRoot",*instance.root_transform);root_layout->addLayout(transform_form);
        root_values_host=new QWidget(root_page);root_values_host->setLayout(new QVBoxLayout);root_layout->addWidget(root_values_host,1);tabs->addTab(root_page,"World placement and overrides");
        auto* nested_page=new QWidget(tabs);auto* nested_layout=new QVBoxLayout(nested_page);
        nested_choice=new QComboBox(nested_page);nested_choice->setObjectName("assemblyNestedPart");nested_layout->addWidget(new QLabel("Select a nested part to author explicit overrides.",nested_page));nested_layout->addWidget(nested_choice);
        nested_defaults=new QLabel(nested_page);nested_defaults->setWordWrap(true);nested_defaults->setObjectName("assemblyNestedInheritedValues");nested_layout->addWidget(nested_defaults);
        nested_transform_enabled=new QCheckBox("Override local part placement",nested_page);nested_transform_enabled->setObjectName("assemblyNestedTransformEnabled");nested_layout->addWidget(nested_transform_enabled);
        auto* nested_form=new QFormLayout;nested_transform=TransformFields::form(nested_page,nested_form,"assemblyNested",{});nested_layout->addLayout(nested_form);
        QObject::connect(nested_transform_enabled,&QCheckBox::toggled,owner,[this](bool enabled){for(auto* input:nested_transform.fields)input->setEnabled(enabled);});
        nested_values_host=new QWidget(nested_page);nested_values_host->setLayout(new QVBoxLayout);nested_layout->addWidget(nested_values_host,1);
        auto* remove=new QPushButton("Remove all overrides on selected part",nested_page);remove->setObjectName("assemblyRemoveNestedOverrides");nested_layout->addWidget(remove);
        QObject::connect(remove,&QPushButton::clicked,owner,[this]{if(selected_path<0)return;const auto path=paths.at(selected_path);std::erase_if(instance.nested_overrides,[&](const auto& entry){return entry.part_path==path;});load_nested();});
        tabs->addTab(nested_page,"Nested part overrides");
        finish_dialog(owner,layout,error,[this]{(void)owner->submit();});
        loading=true;load_catalog();loading=false;
        QObject::connect(catalog,&QComboBox::currentIndexChanged,owner,[this]{if(loading)return;guard([&]{store_nested();instance.property_overrides=root_values->read_properties();instance.material_overrides=root_values->read_materials();instance.quantity_overrides=root_values->read_quantities();load_catalog();});});
        QObject::connect(type,&QComboBox::currentIndexChanged,owner,[this]{if(loading)return;guard([&]{store_nested();instance.type_id=type->currentData().toString().toStdString();load_paths();});});
        QObject::connect(nested_choice,&QComboBox::currentIndexChanged,owner,[this](int index){
            if(loading)return; const auto previous=selected_path;
            if(!guard([&]{store_nested();selected_path=index;load_nested();})){selected_path=previous;QSignalBlocker block(nested_choice);nested_choice->setCurrentIndex(previous);}
        });
    }
    template<class F> bool guard(F action) {
        try {action();error->clear();return true;}catch(const std::exception& e){error->setText(q(e.what()));candidate.reset();return false;}
    }
    const AssemblyModel& model() const {return catalogs.at(catalog->currentData().toString().toStdString());}
    void reset_values(std::unique_ptr<NamedValues>& values,QWidget* host,const QString& prefix) {
        if(values){delete values->tabs;values.reset();}values=std::make_unique<NamedValues>(host,prefix,model());host->layout()->addWidget(values->tabs);
    }
    void load_catalog() {
        QSignalBlocker block(type);type->clear();for(const auto& item:model().types())type->addItem(q(item.name),q(item.id));
        auto desired=type->findData(q(instance.type_id));if(desired>=0)type->setCurrentIndex(desired);
        instance.type_id=type->currentData().toString().toStdString();
        reset_values(root_values,root_values_host,"assemblyRoot");root_values->set(instance.property_overrides,instance.material_overrides,instance.quantity_overrides);
        reset_values(nested_values,nested_values_host,"assemblyNested");load_paths();
    }
    QString path_label(const std::vector<std::string>& path) const {
        return readable_path(model(),instance.type_id,path);
    }
    void load_paths() {
        QSignalBlocker block(nested_choice);nested_choice->clear();paths.clear();selected_path=-1;
        if(!instance.type_id.empty()) {
            AssemblyInstance probe;probe.id=instance.id;probe.type_id=instance.type_id;probe.root_transform=AssemblyTransform{};
            AssemblyExpansionBudget budget;auto expanded=model().expand(probe,budget);
            for(const auto& node:expanded.nodes)if(!node.part_path.empty())paths.push_back(node.part_path);
        }
        for(const auto& entry:instance.nested_overrides)if(std::find(paths.begin(),paths.end(),entry.part_path)==paths.end())paths.push_back(entry.part_path);
        for(const auto& path:paths){nested_choice->addItem(path_label(path));QStringList ids;for(const auto& id:path)ids<<q(id);nested_choice->setItemData(nested_choice->count()-1,ids.join(" / "),Qt::ToolTipRole);}
        selected_path=nested_choice->currentIndex();load_nested();
    }
    void load_nested() {
        const bool enabled=selected_path>=0;nested_values->tabs->setEnabled(enabled);nested_transform_enabled->setEnabled(enabled);
        AssemblyPathOverride current;AssemblyTransform inherited;
        nested_defaults->clear();
        if(enabled) {
            current.part_path=paths.at(selected_path);
            auto found=std::find_if(instance.nested_overrides.begin(),instance.nested_overrides.end(),[&](const auto& entry){return entry.part_path==current.part_path;});
            if(found!=instance.nested_overrides.end())current=*found;
            const AssemblyType* child=&find_type(model(),instance.type_id);
            for(const auto& id:current.part_path) {
                auto part=std::find_if(child->parts.begin(),child->parts.end(),[&](const auto& entry){return entry.id==id;});
                if(part==child->parts.end()){child=nullptr;break;} inherited=part->transform;child=&find_type(model(),part->type_id);
            }
            if(child) {
                QStringList hints;for(const auto& [key,value]:child->properties){(void)value;hints<<"Property: "+q(key);}
                for(const auto& [key,value]:child->materials){(void)value;hints<<"Material slot: "+q(key);}
                for(const auto& [key,value]:child->quantities){(void)value;hints<<"Quantity: "+q(key);}
                nested_defaults->setText("Inherited names: "+hints.join("; "));
            }
        }
        nested_values->set(current.property_overrides,current.material_overrides,current.quantity_overrides);
        nested_transform_enabled->setChecked(current.transform.has_value());nested_transform.set(current.transform.value_or(inherited));
        for(auto* input:nested_transform.fields)input->setEnabled(enabled && current.transform.has_value());
    }
    void store_nested() {
        if(selected_path<0)return;AssemblyPathOverride current;current.part_path=paths.at(selected_path);
        if(nested_transform_enabled->isChecked())current.transform=nested_transform.read(metric);
        current.property_overrides=nested_values->read_properties();current.material_overrides=nested_values->read_materials();current.quantity_overrides=nested_values->read_quantities();
        auto found=std::find_if(instance.nested_overrides.begin(),instance.nested_overrides.end(),[&](const auto& entry){return entry.part_path==current.part_path;});
        const auto nonempty=current.transform || !current.property_overrides.empty() || !current.material_overrides.empty() || !current.quantity_overrides.empty();
        if(found!=instance.nested_overrides.end())*found=std::move(current);else if(nonempty)instance.nested_overrides.push_back(std::move(current));
    }
    AssemblyInstanceDraft read() {
        store_nested();auto value=instance;value.type_id=type->currentData().toString().toStdString();value.placement.reset();value.root_transform=root_transform.read(metric);
        value.property_overrides=root_values->read_properties();value.material_overrides=root_values->read_materials();value.quantity_overrides=root_values->read_quantities();
        const auto entered_name=name->text().trimmed().toStdString();require(!entered_name.empty(),"Enter a name for this placed assembly");
        AssemblyExpansionBudget budget;require(!model().expand(value,budget).profiles.empty(),"Choose an assembly type with usable solid geometry");
        AssemblyDocumentInstance authored{catalog->currentData().toString().toStdString(),value};
        Entity entity=original.value_or(Entity{value.id,"assembly_instance"});entity.properties["name"]=entered_name;
        auto candidate_entities=source.entities();candidate_entities.insert_or_assign(entity.id,encode_document_assembly_instance(entity,authored));validate_document_assembly_instances(candidate_entities);
        return {source.revision(),original,entered_name,std::move(authored)};
    }
};
AssemblyInstanceDialog::AssemblyInstanceDialog(DocumentSnapshot source,std::optional<Entity> expected_instance,std::string preferred_catalog_id,bool metric_units,QWidget* parent)
    : QDialog(parent),m_impl(std::make_unique<Impl>(this,std::move(source),std::move(expected_instance),std::move(preferred_catalog_id),metric_units)) {}
AssemblyInstanceDialog::~AssemblyInstanceDialog()=default;
bool AssemblyInstanceDialog::submit() {return m_impl->guard([&]{m_impl->candidate=m_impl->read();accept();});}
std::optional<AssemblyInstanceDraft> AssemblyInstanceDialog::candidate() const {return result()==QDialog::Accepted ? m_impl->candidate : std::nullopt;}
QString AssemblyInstanceDialog::lastError() const {return m_impl->error->text();}
} // namespace sketch::desktop

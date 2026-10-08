#include "sketch/phase_wall_canvas_projection.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/area_type_presets.hpp"
#include "sketch/architecture.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/document_wall_plan.hpp"
#include "sketch/hosted_opening_plan.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_measurement.hpp"

#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace sketch::desktop {
namespace {
using Entities = PhaseWallReplacementEntities;
using Identity = std::tuple<QString, QString, QString>;

bool horizontal(const BuildingViewFrame& frame) {
    return std::abs(frame.direction.x)<=1e-12 && std::abs(frame.direction.y)<=1e-12 &&
        std::abs(std::abs(frame.direction.z)-1.0)<=1e-12 && std::abs(frame.up.z)<=1e-12;
}
Vec2 right_axis(const BuildingViewFrame& frame) {
    const auto length=std::hypot(frame.up.x,frame.up.y);
    if (!(length>0) || !std::isfinite(length)) throw std::invalid_argument("Phase wall preview has an invalid plan frame.");
    const auto sign=frame.direction.z<0 ? 1.0 : -1.0;
    return {sign*frame.up.y/length,-sign*frame.up.x/length};
}
Vec2 up_axis(const BuildingViewFrame& frame) {
    const auto length=std::hypot(frame.up.x,frame.up.y);
    if (!(length>0) || !std::isfinite(length)) throw std::invalid_argument("Phase wall preview has an invalid plan frame.");
    return {frame.up.x/length,frame.up.y/length};
}
Vec2 project_point(Vec2 point,const BuildingViewFrame& frame) {
    const auto right=right_axis(frame),up=up_axis(frame);
    const auto x=point.x-frame.origin.x,y=point.y-frame.origin.y;
    return {x*right.x+y*right.y,x*up.x+y*up.y};
}
Boundary project_path(Boundary path,const BuildingViewFrame& frame) {
    const auto right=right_axis(frame),up=up_axis(frame);
    for (auto& edge:path) {
        edge.start=project_point(edge.start,frame);edge.end=project_point(edge.end,frame);
        edge.sweep_radians*=right.x*up.y-right.y*up.x;
    }
    return path;
}
double project_angle(double angle,const BuildingViewFrame& frame) {
    const auto right=right_axis(frame),up=up_axis(frame);
    return std::atan2(up.x*std::cos(angle)+up.y*std::sin(angle),
        right.x*std::cos(angle)+right.y*std::sin(angle));
}
bool same_path(const Boundary& a,const Boundary& b) {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0;i<a.size();++i)
        if (a[i].start.x!=b[i].start.x || a[i].start.y!=b[i].start.y ||
            a[i].end.x!=b[i].end.x || a[i].end.y!=b[i].end.y || a[i].sweep_radians!=b[i].sweep_radians) return false;
    return true;
}
Bounds2 crop_bounds(const BuildingViewCrop& crop) {
    return {{crop.min_horizontal_m,crop.min_vertical_m},{crop.max_horizontal_m,crop.max_vertical_m}};
}
bool inside(Vec2 point,const Bounds2& bounds) {
    return std::isfinite(point.x) && std::isfinite(point.y) && point.x>=bounds.minimum.x &&
        point.x<=bounds.maximum.x && point.y>=bounds.minimum.y && point.y<=bounds.maximum.y;
}
void clear_geometry(CanvasEntity& item) {
    item.segments.clear();item.holes.clear();item.stroke_segments.reset();item.hit_segments.clear();
    item.snap_segments.clear();item.snap_points.clear();item.drawing_alignment_segments.clear();
    item.vertex_handles.clear();item.resize_frame.reset();item.opening_width_controls.reset();
    item.endpoint_baseline.reset();item.svg_symbol.reset();
}
void project_entity(CanvasEntity& item,const ArchitecturalViewContext& context,bool dimension=false) {
    item.segments=project_path(std::move(item.segments),context.frame);
    if (item.stroke_segments) *item.stroke_segments=project_path(std::move(*item.stroke_segments),context.frame);
    item.hit_segments=project_path(std::move(item.hit_segments),context.frame);
    item.snap_segments=project_path(std::move(item.snap_segments),context.frame);
    item.drawing_alignment_segments=project_path(std::move(item.drawing_alignment_segments),context.frame);
    for (auto& hole:item.holes) hole=project_path(std::move(hole),context.frame);
    for (auto& point:item.snap_points) point=project_point(point,context.frame);
    for (auto& handle:item.vertex_handles) handle.position=project_point(handle.position,context.frame);
    if (item.endpoint_baseline) item.endpoint_baseline=project_path({*item.endpoint_baseline},context.frame).front();
    if (item.resize_frame) {
        item.resize_frame->center=project_point(item.resize_frame->center,context.frame);
        item.resize_frame->rotation_radians=project_angle(item.resize_frame->rotation_radians,context.frame);
    }
    if (item.opening_width_controls) {
        auto& controls=*item.opening_width_controls;
        controls.start_jamb=project_point(controls.start_jamb,context.frame);
        controls.end_jamb=project_point(controls.end_jamb,context.frame);
        if (controls.host_baseline) controls.host_baseline=project_path({*controls.host_baseline},context.frame).front();
    }
    if (!context.crop || dimension) return;
    const auto bounds=crop_bounds(*context.crop);
    item.segments=clip_boundary_to_bounds(item.segments,bounds);
    if (item.stroke_segments) *item.stroke_segments=clip_boundary_to_bounds(*item.stroke_segments,bounds);
    item.hit_segments=clip_boundary_to_bounds(item.hit_segments,bounds);
    item.snap_segments=clip_boundary_to_bounds(item.snap_segments,bounds);
    item.drawing_alignment_segments=clip_boundary_to_bounds(item.drawing_alignment_segments,bounds);
    for (auto& hole:item.holes) hole=clip_boundary_to_bounds(hole,bounds);
    std::erase_if(item.holes,[](const auto& value){return value.empty();});
    std::erase_if(item.snap_points,[&](Vec2 point){return !inside(point,bounds);});
    std::erase_if(item.vertex_handles,[&](const auto& handle){return !inside(handle.position,bounds);});
    if (item.opening_width_controls && (!inside(item.opening_width_controls->start_jamb,bounds) ||
        !inside(item.opening_width_controls->end_jamb,bounds))) item.opening_width_controls.reset();
    if (item.segments.empty()) item.resize_frame.reset();
}
Boundary project_solid(const TopoDS_Shape& shape,const ArchitecturalViewContext& context) {
    if (!shape_intersects_view_depth(shape,context.depth) ||
        (context.crop && !shape_intersects_view_crop(shape,*context.crop))) return {};
    auto clipped=clip_shape_to_view_depth(shape,context.depth);
    if (!clipped.IsNull() && context.crop) clipped=clip_shape_to_view_crop(clipped,*context.crop);
    return clipped.IsNull() ? Boundary{} : project_shape_view(clipped,BuildingViewKind::plan,context.frame);
}
TopoDS_Shape opening_solid(const Entity& entity,const Wall& wall,const HostedOpening& opening) {
    const auto kind=entity.properties.at("opening_kind").get<std::string>();
    if (kind=="opening") return make_wall(Wall{entity.id,
        hosted_opening_span(wall.baseline,opening.offset,opening.width),wall.thickness,
        opening.height,wall.elevation+opening.sill,{}});
    const auto parsed=parse_opening_assembly_kind(kind);
    if (!parsed) throw std::invalid_argument("Phase wall preview has an unsupported opening kind.");
    const auto assembly=entity.properties.contains("opening_assembly") ?
        parse_opening_assembly(entity.properties.at("opening_assembly")) : default_opening_assembly(*parsed);
    if (opening_assembly_kind_name(assembly.kind)!=kind) throw std::invalid_argument("Phase wall preview opening assembly kind differs from its owner.");
    std::optional<DoorOperation> operation;
    if (kind=="door" && entity.properties.contains("door_operation")) operation=decode_door_operation(entity.properties.at("door_operation"));
    return make_opening_assembly(wall,opening,assembly,operation);
}
bool full_opening_depth(const Wall& wall,const HostedOpening& opening,const BuildingViewDepth& depth) {
    if (std::isinf(depth.far_depth_m) && depth.far_depth_m>0) return true;
    if (!std::isfinite(depth.far_depth_m) || depth.direction.x!=0 || depth.direction.y!=0 || std::abs(depth.direction.z)!=1) return false;
    const auto bottom=(wall.elevation+opening.sill-depth.origin.z)*depth.direction.z;
    const auto top=(wall.elevation+opening.sill+opening.height-depth.origin.z)*depth.direction.z;
    return std::isfinite(bottom) && std::isfinite(top) && std::max(bottom,top)<=depth.far_depth_m;
}
QString length_text(double metres,bool metric,bool ansi=false) {
    if (!ansi) return PlanCanvas::drawingLengthText(metres,metric);
    auto text=QStringLiteral("%1 ft").arg(QString::number(std::round(metres/.3048*10)/10,'f',1));
    if (metric) text+=QStringLiteral(" (%1 m)").arg(QString::number(metres,'f',3));
    return text;
}
QString area_text(double metres,bool metric,bool ansi=false) {
    if (ansi) {
        auto text=QStringLiteral("%1 sq ft").arg(QString::number(std::round(metres/.09290304),'f',0));
        if (metric) text+=QStringLiteral(" (%1 m²)").arg(QString::number(metres,'f',2));
        return text;
    }
    return metric ? QStringLiteral("%1 m²").arg(metres,0,'f',2) : QStringLiteral("%1 ft²").arg(metres/.09290304,0,'f',1);
}
bool ansi_dimensions(const Entities& entities,const Entity& owner) {
    const auto string_property=[](const Entity& entity,const char* key) -> std::string {
        const auto found=entity.properties.find(key);
        return found!=entity.properties.end() && found->is_string() ? found->get<std::string>() : std::string{};
    };
    auto property=string_property(owner,"property_id"),building=string_property(owner,"building_id");
    const auto floor=string_property(owner,"floor_id");
    if (entities.contains(floor)) building=string_property(entities.at(floor),"building_id");
    if (entities.contains(building)) property=string_property(entities.at(building),"property_id");
    if (!entities.contains(property)) return false;
    const auto& props=entities.at(property).properties;
    const auto policy=props.find("appraisal_policy");
    return props.value("calculation_workflow",nlohmann::json{})=="appraisal" && policy!=props.end() &&
        policy->is_object() && policy->value("policy_kind",nlohmann::json{})=="ansi_z765_2021";
}
Boundary dimension_overlay(const Segment& segment,Vec2 text) {
    const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y;
    const auto chord=std::hypot(dx,dy);
    if (!(chord>1e-7) || !std::isfinite(chord)) throw std::invalid_argument("Phase wall dimension has no representable witness geometry.");
    const Vec2 middle{std::midpoint(segment.start.x,segment.end.x),std::midpoint(segment.start.y,segment.end.y)};
    if (segment.sweep_radians==0) {
        const Vec2 normal{-dy/chord,dx/chord};
        const auto offset=(text.x-middle.x)*normal.x+(text.y-middle.y)*normal.y;
        const Vec2 start{segment.start.x+normal.x*offset,segment.start.y+normal.y*offset};
        const Vec2 end{segment.end.x+normal.x*offset,segment.end.y+normal.y*offset};
        return {{segment.start,start,0},{segment.end,end,0},{start,end,0}};
    }
    const auto tangent=std::tan(segment.sweep_radians*.5);
    if (!std::isfinite(tangent) || tangent==0) throw std::invalid_argument("Phase wall dimension arc is unrepresentable.");
    const Vec2 center{middle.x-dy/(2*tangent),middle.y+dx/(2*tangent)};
    const auto original_radius=std::hypot(segment.start.x-center.x,segment.start.y-center.y);
    const auto requested=std::hypot(text.x-center.x,text.y-center.y);
    const auto radius=requested>1e-7 ? requested : original_radius+std::max(.25,original_radius*.1);
    const auto angle=std::atan2(segment.start.y-center.y,segment.start.x-center.x);
    const Vec2 start{center.x+radius*std::cos(angle),center.y+radius*std::sin(angle)};
    const Vec2 end{center.x+radius*std::cos(angle+segment.sweep_radians),center.y+radius*std::sin(angle+segment.sweep_radians)};
    return {{segment.start,start,0},{segment.end,end,0},{start,end,segment.sweep_radians}};
}
Vec2 tangent_at(const IdentifiedSegment& edge,const std::string& vertex) {
    const auto start=edge.start_vertex_id==vertex,end=edge.end_vertex_id==vertex;
    if (start==end) throw std::invalid_argument("Phase wall angle dimension has ambiguous vertex support.");
    auto direction=Vec2{edge.segment.end.x-edge.segment.start.x,edge.segment.end.y-edge.segment.start.y};
    if (edge.segment.sweep_radians!=0) {
        const auto tangent=std::tan(edge.segment.sweep_radians*.5);
        if (!std::isfinite(tangent) || std::abs(tangent)<=1e-12) throw std::invalid_argument("Phase wall angle dimension arc is unrepresentable.");
        const Vec2 center{std::midpoint(edge.segment.start.x,edge.segment.end.x)-direction.y/(2*tangent),
            std::midpoint(edge.segment.start.y,edge.segment.end.y)+direction.x/(2*tangent)};
        const auto point=start ? edge.segment.start : edge.segment.end;
        const Vec2 radial{point.x-center.x,point.y-center.y};
        direction=edge.segment.sweep_radians>0 ? Vec2{-radial.y,radial.x} : Vec2{radial.y,-radial.x};
    }
    if (end) direction={-direction.x,-direction.y};
    const auto magnitude=std::hypot(direction.x,direction.y);
    if (!(magnitude>1e-7) || !std::isfinite(magnitude)) throw std::invalid_argument("Phase wall angle dimension tangent is unrepresentable.");
    return {direction.x/magnitude,direction.y/magnitude};
}
Boundary angle_overlay(const Entity& owner,const BoundaryDimension& dimension) {
    const auto geometry=resolve_dimension_geometry_owner(owner);
    const auto edge=[&](const std::string& id) -> const IdentifiedSegment& {
        const auto found=std::find_if(geometry.segments.begin(),geometry.segments.end(),[&](const auto& value){return value.segment_id==id;});
        if (found==geometry.segments.end()) throw std::invalid_argument("Phase wall angle dimension lost its stable segment.");
        return *found;
    };
    const auto& first=edge(dimension.segment_id);const auto& second=edge(dimension.secondary_segment_id);
    const auto a=tangent_at(first,dimension.vertex_id),b=tangent_at(second,dimension.vertex_id);
    const auto sweep=std::atan2(a.x*b.y-a.y*b.x,a.x*b.x+a.y*b.y);
    if (!std::isfinite(sweep) || std::abs(sweep)<=1e-7) throw std::invalid_argument("Phase wall angle dimension has no representable arc.");
    const auto vertex=first.start_vertex_id==dimension.vertex_id ? first.segment.start : first.segment.end;
    const auto requested=std::hypot(dimension.text_position.x-vertex.x,dimension.text_position.y-vertex.y);
    const auto radius=requested>1e-7 ? requested : std::max(.25,std::min(segment_length(first.segment),segment_length(second.segment))*.25);
    const auto angle=std::atan2(a.y,a.x);
    const Vec2 start{vertex.x+radius*std::cos(angle),vertex.y+radius*std::sin(angle)};
    const Vec2 end{vertex.x+radius*std::cos(angle+sweep),vertex.y+radius*std::sin(angle+sweep)};
    return {{vertex,start,0},{start,end,sweep},{end,vertex,0}};
}
void project_label(CanvasLabel& label,const BuildingViewFrame& frame,bool rotate) {
    label.position=project_point(label.position,frame);
    if (label.leader_start) label.leader_start=project_point(*label.leader_start,frame);
    if (rotate) {
        label.rotation_radians=project_angle(label.rotation_radians,frame);
        if (!label.derived_label_manual_rotation) {
            while (label.rotation_radians>std::numbers::pi/2) label.rotation_radians-=std::numbers::pi;
            while (label.rotation_radians<=-std::numbers::pi/2) label.rotation_radians+=std::numbers::pi;
        }
    }
    if (label.automatic_linear_placement) {
        auto& value=*label.automatic_linear_placement;
        value.anchor=project_path({value.anchor},frame).front();
        const auto right=right_axis(frame),up=up_axis(frame),normal=value.outward_normal;
        value.outward_normal={right.x*normal.x+right.y*normal.y,up.x*normal.x+up.y*normal.y};
    }
}

std::map<std::string,PresentationOverride,std::less<>> wall_presentations(const Entities& entities) {
    std::map<std::string,PresentationOverride,std::less<>> result;
    for (const auto& [id,entity]:entities) {
        (void)id;
        if (entity.type!=kAnnotationEntityType) continue;
        for (const auto& value:decode_annotation_entity(entity).overrides) {
            if (value.target_kind!="wall_dimension") continue;
            if (!result.emplace(value.target_id,value).second) throw std::invalid_argument("Phase wall preview has conflicting wall measurement presentation records.");
        }
    }
    return result;
}
std::map<std::string,QPainterPath,std::less<>> exterior_regions(const Entities& entities) {
    std::map<std::string,QPainterPath,std::less<>> result;
    for (const auto& [id,entity]:entities) {
        (void)id;
        if (!entity.properties.contains("wall_measurement_source") || !wall_measurement_source_current(entities,entity)) continue;
        const auto boundary=boundary_geometry(decode_identified_boundary_entity(entity));
        if (boundary.empty()) continue;
        QPainterPath path;path.moveTo(boundary.front().start.x,boundary.front().start.y);
        for (const auto& segment:boundary) {
            const auto steps=std::max(1,static_cast<int>(std::ceil(std::abs(segment.sweep_radians)/.05)));
            for (int i=1;i<=steps;++i) {
                const auto point=point_at_segment(segment,static_cast<double>(i)/steps);
                if (!point) throw std::invalid_argument("Phase wall preview exterior label region is unrepresentable.");
                path.lineTo(point->x,point->y);
            }
        }
        path.closeSubpath();
        for (const auto& wall:entity.properties.at("wall_measurement_source").at("walls"))
            result.try_emplace(wall.at("id").get<std::string>(),path);
    }
    return result;
}
CanvasLabel linear_label(const CanvasLabel& prototype,const Segment& segment,double thickness,bool metric,bool ansi,
    const PresentationOverride* presentation,const QPainterPath* exterior,bool right_side=false) {
    auto result=prototype;
    const auto middle=point_at_segment(segment,.5);
    const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y,chord=std::hypot(dx,dy);
    if (!middle || !(chord>1e-9)) throw std::invalid_argument("Phase wall preview measurement has no midpoint.");
    Vec2 normal{-dy/chord,dx/chord};const auto clearance=thickness*.5+.14;
    if (right_side) normal={-normal.x,-normal.y};
    if (exterior && exterior->contains(QPointF(middle->x+normal.x*clearance,middle->y+normal.y*clearance)) &&
        !exterior->contains(QPointF(middle->x-normal.x*clearance,middle->y-normal.y*clearance))) normal={-normal.x,-normal.y};
    result.position={middle->x+normal.x*clearance,middle->y+normal.y*clearance};
    result.text=length_text(segment_length(segment),metric,ansi);
    result.rotation_radians=std::atan2(dy,dx);result.derived_label_manual_rotation=false;result.leader_start.reset();
    while (result.rotation_radians>std::numbers::pi/2) result.rotation_radians-=std::numbers::pi;
    while (result.rotation_radians<=-std::numbers::pi/2) result.rotation_radians+=std::numbers::pi;
    result.automatic_linear_placement=CanvasLinearLabelPlacement{segment,normal,clearance};
    if (presentation) {
        if (!presentation->visible) result.text.clear();
        if (presentation->plan_label_rotation_radians) {
            result.rotation_radians=*presentation->plan_label_rotation_radians;result.derived_label_manual_rotation=true;
        }
        if (presentation->plan_label_offset) {
            result.position={middle->x+presentation->plan_label_offset->x,middle->y+presentation->plan_label_offset->y};
            result.automatic_linear_placement.reset();result.leader_start=*middle;
        }
    }
    return result;
}

std::vector<std::string> deduction_ids(const Entity& entity) {
    std::vector<std::string> result;
    if (!entity.properties.contains("deduction_ids")) return result;
    const auto& ids=entity.properties.at("deduction_ids");
    if (!ids.is_array()) throw std::invalid_argument("Phase wall preview deduction IDs must be an array.");
    std::set<std::string,std::less<>> unique;
    for (const auto& value:ids) {
        if (!value.is_string()) throw std::invalid_argument("Phase wall preview deduction ID is invalid.");
        const auto id=value.get<std::string>();
        if (id.empty() || !unique.insert(id).second) throw std::invalid_argument("Phase wall preview deduction IDs must be unique.");
        result.push_back(id);
    }
    return result;
}
ExactRational area_factor(const Entity& entity) {
    const auto& props=entity.properties;
    if (props.contains("factor_numerator") || props.contains("factor_denominator")) {
        if (!props.contains("factor_numerator") || !props.contains("factor_denominator") ||
            !props.at("factor_numerator").is_number_integer() || !props.at("factor_denominator").is_number_integer())
            throw std::invalid_argument("Phase wall preview area factor requires exact integers.");
        const auto numerator=props.at("factor_numerator").get<std::int64_t>(),denominator=props.at("factor_denominator").get<std::int64_t>();
        if (numerator<0 || denominator<=0) throw std::invalid_argument("Phase wall preview area factor is invalid.");
        const auto divisor=std::gcd(numerator,denominator);
        return {numerator/divisor,denominator/divisor};
    }
    if (!props.contains("factor")) return {1,1};
    if (!props.at("factor").is_number()) throw std::invalid_argument("Phase wall preview area factor is invalid.");
    const auto factor=props.at("factor").get<double>();
    if (!std::isfinite(factor) || factor<0) throw std::invalid_argument("Phase wall preview area factor is invalid.");
    std::ostringstream encoded;encoded.imbue(std::locale::classic());
    encoded<<std::setprecision(std::numeric_limits<double>::max_digits10)<<factor;
    return parse_quantity(encoded.str(),Unit::metre).exact_metres;
}
CalculationProfile measurement_profile(const Entity& property,bool metric) {
    CalculationProfile result{"vertex-default",2,metric ? AreaUnit::square_metre : AreaUnit::square_foot,2,
        {{"measurement",{true,false}},{"survey",{false,false}},{"room",{true,true}},
         {"living",{true,true}},{"interior",{false,false}},{"exterior",{true,false}},{"party",{true,false}}}};
    for (const auto& preset:area_type_presets) if (!preset.classification.empty())
        result.classifications.emplace(std::string(preset.classification),ClassificationRule{preset.building_total,preset.living_total});
    const auto& props=property.properties;
    if (!props.contains("calculation_profile")) return result;
    const auto& saved=props.at("calculation_profile");
    if (!saved.is_object()) throw std::invalid_argument("Phase wall preview calculation profile is invalid.");
    result.id=saved.value("id",result.id);
    if (saved.contains("version")) {
        const auto& version=saved.at("version");
        if (!version.is_number_unsigned() || version.get<std::uint64_t>()>std::numeric_limits<unsigned>::max())
            throw std::invalid_argument("Phase wall preview calculation profile version is invalid.");
        result.version=version.get<unsigned>();
    }
    if (result.id.empty() || result.version==0) throw std::invalid_argument("Phase wall preview calculation profile identity is invalid.");
    result.decimal_places=appraisal_display_profile(props).decimal_places;
    if (saved.contains("classifications")) {
        if (!saved.at("classifications").is_object()) throw std::invalid_argument("Phase wall preview classification rules are invalid.");
        result.classifications.clear();
        for (const auto& [name,rule]:saved.at("classifications").items()) {
            if (!rule.is_object() || !rule.contains("building_total") || !rule.contains("living_total") ||
                !rule.at("building_total").is_boolean() || !rule.at("living_total").is_boolean())
                throw std::invalid_argument("Phase wall preview classification rules need boolean totals.");
            auto category=AppraisalAreaCategory::none;
            if (rule.contains("appraisal_category")) {
                if (!rule.at("appraisal_category").is_string()) throw std::invalid_argument("Phase wall preview appraisal category is invalid.");
                const auto parsed=parse_appraisal_category(rule.at("appraisal_category").get<std::string>());
                if (!parsed) throw std::invalid_argument("Phase wall preview appraisal category is unknown.");
                category=*parsed;
            }
            result.classifications.emplace(name,ClassificationRule{rule.at("building_total").get<bool>(),rule.at("living_total").get<bool>(),category});
        }
    }
    return result;
}
QString measurement_area_text(const Entities& stage,const ProjectOrganization& organization,
    const std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>& checks,const Entity& entity,bool metric) {
    const auto context=organization.drawing_context(entity.id);
    if (!context || !stage.contains(context->property_id))
        throw std::invalid_argument("Phase wall preview affected area has no qualified drawing context: "+entity.id);
    const auto& property=stage.at(context->property_id);
    if (property.type!="property") throw std::invalid_argument("Phase wall preview area property is invalid: "+entity.id);
    // The physical stage deliberately has no proposed room/appraisal facts.
    // An empty value is an explicit override of the captured numeric callout.
    if (property.properties.value("calculation_workflow",std::string{"measurement"})!="measurement") return {};
    const auto classification=entity.properties.value("measurement_classification",std::string{}).empty() ?
        entity.properties.value("classification",std::string{}) : entity.properties.at("measurement_classification").get<std::string>();
    if (classification.empty() || classification=="non_calculated") return {};
    if (!wall_measurement_source_current(stage,entity) || !measurement_linework_source_current(checks,entity)) return {};
    std::vector<AreaDeduction> deductions;
    for (const auto& id:deduction_ids(entity)) {
        const auto& child=stage.at(id);const auto child_context=organization.drawing_context(id);
        if (id==entity.id || is_physical_wall_room(child) || !can_recognize_boundary_entity_type(child.type) ||
            !child_context || child_context->property_id!=context->property_id || child_context->building_id!=context->building_id ||
            child_context->floor_id!=context->floor_id || !deduction_ids(child).empty() ||
            !wall_measurement_source_current(stage,child) || !measurement_linework_source_current(checks,child))
            throw std::invalid_argument("Phase wall canvas preview area deduction is unavailable or stale: "+id);
        deductions.push_back({id,boundary_geometry(decode_identified_boundary_entity(child))});
    }
    const auto scope=entity.properties.value("calculation_scope",classification=="survey" ? std::string{"site"} : std::string{"building"});
    if (scope!="site" && scope!="building") throw std::invalid_argument("Phase wall preview calculation scope is invalid.");
    const auto calculation=calculate_area(MeasurementArea{entity.id,context->building_id,context->floor_id,classification,
        boundary_geometry(decode_identified_boundary_entity(entity)),std::move(deductions),area_factor(entity),
        scope=="site" ? AreaScope::site : AreaScope::building},measurement_profile(property,metric));
    return QString::fromStdString(calculation.display.text)+(metric ? QStringLiteral(" m²") : QStringLiteral(" ft²"));
}
} // namespace

bool analytical_canvas_plan_context(BuildingViewKind kind, const ArchitecturalViewContext& context) noexcept {
    const auto& frame = context.frame;
    return kind == BuildingViewKind::plan &&
        frame.origin.x == 0.0 && frame.origin.y == 0.0 && frame.origin.z == 0.0 &&
        frame.direction.x == 0.0 && frame.direction.y == 0.0 && frame.direction.z == -1.0 &&
        frame.up.x == 0.0 && frame.up.y == 1.0 && frame.up.z == 0.0 &&
        (context.depth.far_depth_m == ViewPresentation{}.far_depth_m ||
         std::isinf(context.depth.far_depth_m));
}

PhaseWallCanvasProjection project_phase_wall_canvas(const DocumentSnapshot& source,
    const PhaseWallReplacementAuthoringPreview& physical,const std::vector<CanvasEntity>& retained,
    const std::vector<CanvasEntity>& eligible,const std::vector<CanvasLabel>& labels,bool metric_units,
    const std::optional<ArchitecturalViewContext>& view_context) {
    const auto& stage=physical.edited_entities;
    const auto& aliases=physical.replacement.original_to_proposed;
    if (stage.empty() || aliases.empty()) throw std::invalid_argument("Phase wall canvas preview requires a complete qualified physical stage.");
    std::set<std::string,std::less<>> fresh;
    for (const auto& [original,proposed]:aliases) {
        if (original==proposed || source.entities().contains(proposed) || !fresh.insert(proposed).second)
            throw std::invalid_argument("Phase wall canvas preview requires injective fresh physical identities.");
        if (const auto before=source.entities().find(original);before!=source.entities().end()) {
            const auto after=stage.find(proposed);
            if (after==stage.end() || after->second.id!=proposed || after->second.type!=before->second.type)
                throw std::invalid_argument("Phase wall canvas preview lost a qualified replacement owner: "+original);
        }
    }
    const auto proposed_id=[&](const std::string& id) {
        const auto found=aliases.find(id);return found==aliases.end() ? id : found->second;
    };
    std::map<std::string,Wall,std::less<>> walls;
    for (const auto& [original,proposed]:aliases) {
        if (!source.entities().contains(original) || source.entities().at(original).type!="wall") continue;
        std::vector<const Entity*> children;
        for (const auto& [id,entity]:stage) {
            (void)id;
            if (entity.type=="opening" && entity.properties.value("wall_id",std::string{})==proposed) children.push_back(&entity);
        }
        Wall wall;std::string error;
        if (!read_document_wall(resolve_vertical_placement(stage,stage.at(proposed)),children,wall,error))
            throw std::invalid_argument("Phase wall canvas preview wall "+original+": "+error);
        walls.emplace(proposed,std::move(wall));
    }
    // Phase replacement retains its originals as recovery/design evidence.
    // Only the actual saved active semantic roster participates in junctions;
    // duplicate retired originals must not create false caps or T/X seams.
    const auto stage_scope=constraint_phase_scope(stage);
    std::set<std::string,std::less<>> semantic_visible;
    for (const auto& [id,entity]:stage) if (!stage_scope.inactive_owner_ids.contains(id)) {
        (void)entity;
        semantic_visible.insert(id);
    }
    const auto plans=document_wall_plan_geometry(stage,walls);
    const auto old_plans=document_wall_plan_geometry(source.entities());
    const auto presentations=wall_presentations(source.entities());
    const auto regions=exterior_regions(stage);
    const bool horizontal_plan=!view_context || horizontal(view_context->frame);
    const bool shape_projection=view_context && !analytical_canvas_plan_context(BuildingViewKind::plan, *view_context) &&
        !(horizontal_plan && std::isinf(view_context->depth.far_depth_m));
    std::map<Identity,CanvasEntity> prototypes;
    std::set<Identity> captured;
    const auto identity=[](const CanvasEntity& value){return Identity{value.id,value.presentation_key,value.type};};
    for (const auto& value:eligible) prototypes.emplace(identity(value),value);
    for (const auto& value:retained) {prototypes.insert_or_assign(identity(value),value);captured.insert(identity(value));}
    PhaseWallCanvasProjection result;
    std::set<std::string,std::less<>> affected;
    for (const auto& [original,proposed]:aliases)
        if (source.entities().contains(original)) affected.insert(original);
    for (const auto& [id,plan]:plans) {
        if (fresh.contains(id) || aliases.contains(id) || !source.entities().contains(id)) continue;
        const auto old=old_plans.find(id);
        if (old!=old_plans.end() && (!same_path(plan.footprint,old->second.footprint) || !same_path(plan.strokes,old->second.strokes))) affected.insert(id);
    }
    std::map<std::string,MeasurementLineworkReplay,std::less<>> strokes;
    const auto stroke=[&](const std::string& id) -> const MeasurementLineworkReplay& {
        if (!strokes.contains(id)) {
            const auto decoded=decode_measurement_linework_model(stage.at(id).properties.at("model"));
            if (!decoded.supported()) throw std::invalid_argument("Phase wall canvas preview measured stroke: "+decoded.diagnostic);
            strokes.emplace(id,replay_measurement_linework(*decoded.model));
        }
        return strokes.at(id);
    };
    const auto dimension=[&](const Entity& entity) {
        const auto decoded=decode_boundary_dimension_entity(entity);
        if (!decoded.supported()) throw std::invalid_argument("Phase wall canvas preview dimension: "+decoded.unsupported_reason);
        return *decoded.dimension;
    };
    for (const auto& [key,prototype]:prototypes) {
        const auto id=prototype.id.toStdString();
        if (!affected.contains(id)) continue;
        const auto target=proposed_id(id);
        const auto found=stage.find(target);
        if (found==stage.end()) throw std::invalid_argument("Phase wall canvas preview affected presentation owner is missing: "+id);
        const auto& entity=found->second;
        if (is_physical_wall_room(entity)) continue;
        auto projected=prototype;clear_geometry(projected);
        bool world=true;
        if (entity.type=="wall") {
            if (!walls.contains(target)) {
                std::vector<const Entity*> children;
                for (const auto& [child_id,child]:stage) {
                    (void)child_id;
                    if (child.type=="opening" && child.properties.value("wall_id",std::string{})==target) children.push_back(&child);
                }
                Wall wall;std::string error;
                if (!read_document_wall(resolve_vertical_placement(stage,entity),children,wall,error)) throw std::invalid_argument(error);
                walls.emplace(target,std::move(wall));
            }
            const auto& wall=walls.at(target);
            projected.thickness_metres=wall.thickness;
            if (shape_projection) {projected.segments=project_solid(make_wall(wall),*view_context);world=false;}
            else {
                const auto plan=plans.find(target);
                if (plan==plans.end()) throw std::invalid_argument("Phase wall canvas preview has no admitted wall plan: "+id);
                projected.segments=plan->second.footprint;projected.stroke_segments=plan->second.strokes;
            }
            if (horizontal_plan && !shape_projection) {
                // A visible selected wall can expose endpoint handles without
                // belonging to the active drawing context. Selection does not
                // grant snap authority which the captured scene withheld.
                if (!prototype.snap_points.empty()) projected.snap_points={wall.baseline.start,wall.baseline.end};
                if (!prototype.snap_segments.empty()) projected.snap_segments={wall.baseline};
                if (!prototype.drawing_alignment_segments.empty()) projected.drawing_alignment_segments={wall.baseline};
                if (prototype.endpoint_baseline) projected.endpoint_baseline=wall.baseline;
                for (const auto& handle:prototype.vertex_handles) {
                    if (handle.id==QStringLiteral("wall:start")) projected.vertex_handles.push_back({handle.id,wall.baseline.start,handle.source_revision});
                    else if (handle.id==QStringLiteral("wall:end")) projected.vertex_handles.push_back({handle.id,wall.baseline.end,handle.source_revision});
                    else throw std::invalid_argument("Phase wall canvas preview has an unknown wall handle.");
                }
                if (prototype.resize_frame && wall.baseline.sweep_radians==0) projected.resize_frame=CanvasSelectionFrame{
                    {std::midpoint(wall.baseline.start.x,wall.baseline.end.x),std::midpoint(wall.baseline.start.y,wall.baseline.end.y)},
                    std::atan2(wall.baseline.end.y-wall.baseline.start.y,wall.baseline.end.x-wall.baseline.start.x),segment_length(wall.baseline),wall.thickness};
            }
        } else if (entity.type=="opening") {
            const auto host=entity.properties.at("wall_id").get<std::string>();
            if (!walls.contains(host)) throw std::invalid_argument("Phase wall canvas preview opening has no qualified replacement host: "+id);
            const auto& wall=walls.at(host);
            const auto opening=std::find_if(wall.openings.begin(),wall.openings.end(),[&](const auto& child){return child.id==target;});
            if (opening==wall.openings.end()) throw std::invalid_argument("Phase wall canvas preview opening is missing from its physical host: "+id);
            if (view_context && (!horizontal_plan || !full_opening_depth(wall,*opening,view_context->depth))) {
                projected.segments=project_solid(opening_solid(entity,wall,*opening),*view_context);world=false;
                projected.filled=false;projected.hatch_pattern=QStringLiteral("none");
            } else {
                const auto kind=entity.properties.at("opening_kind").get<std::string>();
                if (entity.properties.contains("opening_assembly")) {
                    const auto assembly=parse_opening_assembly(entity.properties.at("opening_assembly"));
                    if (opening_assembly_kind_name(assembly.kind)!=kind) throw std::invalid_argument("Phase wall preview opening assembly kind differs from its owner.");
                    std::optional<DoorOperation> operation;
                    if (kind=="door" && entity.properties.contains("door_operation")) operation=decode_door_operation(entity.properties.at("door_operation"));
                    projected.segments=project_hosted_opening_plan(wall,*opening,assembly,operation);
                } else if (kind=="door" && entity.properties.contains("door_operation"))
                    projected.segments=door_plan_symbol(wall.baseline,opening->offset,opening->width,decode_door_operation(entity.properties.at("door_operation")));
                else if (kind=="window") projected.segments=window_plan_symbol(wall.baseline,opening->offset,opening->width,wall.thickness);
                else if (kind=="opening" || kind=="door") {
                    const auto span=hosted_opening_span(wall.baseline,opening->offset,opening->width);
                    const auto outline=wall_plan_footprint(span,{},wall.thickness);
                    projected.segments={outline.at(1),outline.at(3),span};
                } else throw std::invalid_argument("Phase wall canvas preview has an unsupported opening kind: "+id);
                const auto span=hosted_opening_span(wall.baseline,opening->offset,opening->width);
                projected.hit_segments={span};
                if (prototype.opening_width_controls) projected.opening_width_controls=CanvasOpeningWidthControls{
                    span.start,span.end,opening->width,opening->height,source.revision(),wall.baseline,opening->offset};
            }
        } else if (entity.type=="measurement_linework" || can_recognize_boundary_entity_type(entity.type)) {
            if (!horizontal_plan) throw std::invalid_argument("Phase wall canvas preview analytical owner requires a horizontal plan: "+id);
            std::map<std::string,Vec2,std::less<>> vertices;
            if (entity.type=="measurement_linework") {
                for (const auto& edge:stroke(target).edges) {
                    projected.segments.push_back(edge.segment);vertices.emplace(edge.start_vertex_id,edge.segment.start);vertices.emplace(edge.end_vertex_id,edge.segment.end);
                }
            } else {
                const auto boundary=decode_identified_boundary_entity(entity);
                projected.segments=boundary_geometry(boundary);
                for (const auto& edge:boundary.segments) {vertices.emplace(edge.start_vertex_id,edge.segment.start);vertices.emplace(edge.end_vertex_id,edge.segment.end);}
            }
            projected.snap_segments=projected.segments;
            for (const auto& [vertex,position]:vertices) { (void)vertex;projected.snap_points.push_back(position); }
            for (const auto& handle:prototype.vertex_handles) {
                const auto vertex=vertices.find(proposed_id(handle.id.toStdString()));
                if (vertex==vertices.end()) throw std::invalid_argument("Phase wall canvas preview lost a qualified analytical handle: "+id);
                projected.vertex_handles.push_back({handle.id,vertex->second,handle.source_revision});
            }
        } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=dimension(entity);
            const auto& owner=stage.at(decoded.boundary_id);
            if (is_physical_wall_room(owner) && physical.needs_room_review) {
                projected.dimension_end_ticks=false;
            } else {
                if (!horizontal_plan) throw std::invalid_argument("Phase wall canvas preview analytical dimension requires a horizontal plan: "+id);
                const auto resolved=decoded.resolve(stage);
                if (decoded.presentation && !decoded.presentation->visible) projected.segments.clear();
                else if (resolved.kind==BoundaryDimensionKind::segment_length || resolved.kind==BoundaryDimensionKind::wall_axis_length)
                    projected.segments=dimension_overlay(resolved.segment,decoded.text_position);
                else if (resolved.kind==BoundaryDimensionKind::angle) projected.segments=angle_overlay(owner,decoded);
                projected.dimension_end_ticks=resolved.kind==BoundaryDimensionKind::segment_length || resolved.kind==BoundaryDimensionKind::wall_axis_length;
            }
        } else throw std::invalid_argument("Phase wall canvas preview has unsupported affected geometry: "+id+" ("+entity.type+").");
        if (view_context && world) project_entity(projected,*view_context,prototype.type==QStringLiteral("dimension_line"));
        if (captured.contains(key) || !projected.segments.empty() || !projected.holes.empty()) result.entities.push_back(std::move(projected));
    }
    std::map<std::string,std::size_t,std::less<>> legacy_counts;
    for (const auto& label:labels) if (label.callout_role.isEmpty()) ++legacy_counts[label.id.toStdString()];
    std::optional<ProjectOrganization> area_organization;
    std::optional<std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>> area_source_checks;
    for (const auto& prototype:labels) {
        const auto id=prototype.id.toStdString();
        if (!affected.contains(id)) continue;
        const auto target=proposed_id(id);const auto& entity=stage.at(target);
        if (is_physical_wall_room(entity)) continue;
        auto projected=prototype;
        if (entity.type=="wall" || entity.type=="measurement_linework") {
            if (!horizontal_plan) {projected.text.clear();result.labels.push_back(std::move(projected));continue;}
            Segment baseline;double thickness{};bool right_side{};
            if (entity.type=="wall") {baseline=walls.at(target).baseline;thickness=walls.at(target).thickness;}
            else {
                std::string edge_id;
                if (prototype.callout_role.startsWith(QStringLiteral("measurement_edge:")))
                    edge_id=proposed_id(prototype.callout_role.mid(QStringLiteral("measurement_edge:").size()).toStdString());
                else if (prototype.callout_role.isEmpty() && prototype.automatic_linear_placement && legacy_counts.at(id)==1) {
                    const auto decoded=decode_measurement_linework_model(source.entities().at(id).properties.at("model"));
                    if (!decoded.supported()) throw std::invalid_argument("Phase wall preview source measurement label is unsupported.");
                    std::optional<std::string> matched;
                    for (const auto& edge:replay_measurement_linework(*decoded.model).edges) {
                        auto anchor=edge.segment;
                        if (view_context) anchor=project_path({anchor},view_context->frame).front();
                        if (!same_path({anchor},{prototype.automatic_linear_placement->anchor})) continue;
                        if (matched) throw std::invalid_argument("Phase wall preview legacy measurement label is ambiguous.");
                        matched=edge.segment_id;
                    }
                    if (matched) edge_id=proposed_id(*matched);
                }
                const auto& replay=stroke(target);
                const auto found=std::find_if(replay.edges.begin(),replay.edges.end(),[&](const auto& edge){return edge.segment_id==edge_id;});
                if (found==replay.edges.end()) throw std::invalid_argument("Phase wall preview cannot bind an affected measurement label to its typed edge: "+id);
                baseline=found->segment;Boundary boundary;
                for (const auto& edge:replay.edges) boundary.push_back(edge.segment);
                right_side=replay.closed && signed_area(boundary)>0;
            }
            const auto configured=presentations.find(id);
            const auto exterior=regions.find(target);
            projected=linear_label(prototype,baseline,thickness,metric_units,entity.type=="measurement_linework" && ansi_dimensions(stage,entity),
                configured==presentations.end() ? nullptr : &configured->second,exterior==regions.end() ? nullptr : &exterior->second,right_side);
            if (view_context) project_label(projected,view_context->frame,true);
        } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=dimension(entity);const auto& owner=stage.at(decoded.boundary_id);
            if (is_physical_wall_room(owner) && physical.needs_room_review) projected.text.clear();
            else {
                if (!horizontal_plan) throw std::invalid_argument("Phase wall preview analytical dimension label requires a horizontal plan: "+id);
                const auto resolved=decoded.resolve(stage);
                if (decoded.presentation && !decoded.presentation->visible) projected.text.clear();
                else if (resolved.kind==BoundaryDimensionKind::segment_length || resolved.kind==BoundaryDimensionKind::wall_axis_length)
                    projected.text=length_text(resolved.segment_length_metres,metric_units,ansi_dimensions(stage,owner));
                else if (resolved.kind==BoundaryDimensionKind::angle) projected.text=QStringLiteral("%1°").arg(resolved.angle_radians*180/std::numbers::pi,0,'f',1);
                else projected.text=area_text(resolved.area_square_metres,metric_units,ansi_dimensions(stage,owner));
                projected.position=decoded.text_position;
                if (decoded.presentation) projected.rotation_radians=decoded.presentation->rotation_radians;
                if (view_context) project_label(projected,view_context->frame,false);
            }
        } else if (can_recognize_boundary_entity_type(entity.type)) {
            const auto boundary=boundary_geometry(decode_identified_boundary_entity(entity));
            if (prototype.callout_role!=QStringLiteral("area_name")) {
                if (!area_organization) area_organization=organize_project(stage);
                if (!area_source_checks) area_source_checks=measurement_linework_source_checks(stage,&semantic_visible);
                const auto calculation=measurement_area_text(stage,*area_organization,*area_source_checks,entity,metric_units);
                if (prototype.callout_role==QStringLiteral("area_calculation")) projected.text=calculation;
                else if (prototype.callout_role.isEmpty()) {
                    // The captured combined label's name is presentation text;
                    // update only its quantity line, preserving its spelling.
                    const auto newline=prototype.text.lastIndexOf(QLatin1Char('\n'));
                    auto name=newline>=0 ? prototype.text.left(newline) : QString{};
                    if (newline<0 && !entity.properties.value("name",std::string{}).empty()) name=QString::fromStdString(entity.properties.at("name").get<std::string>());
                    projected.text=name;
                    if (!calculation.isEmpty()) {if (!projected.text.isEmpty()) projected.text+=QLatin1Char('\n');projected.text+=calculation;}
                } else throw std::invalid_argument("Phase wall preview has an unknown affected area callout role: "+id);
            }
            projected.position=area_label_anchor(boundary);
            if (projected.plan_label_offset) {projected.position.x+=projected.plan_label_offset->x;projected.position.y+=projected.plan_label_offset->y;}
            projected.leader_start.reset();
            if (view_context) project_label(projected,view_context->frame,projected.derived_label_manual_rotation);
        } else throw std::invalid_argument("Phase wall canvas preview has an unsupported affected label owner: "+id);
        result.labels.push_back(std::move(projected));
    }
    if (view_context) for (const auto& overlay:view_context->overlays) {
        if (overlay.kind!=SectionOverlayKind::dimension || !overlay.dimension_binding ||
            !affected.contains(overlay.dimension_binding->object_id) ||
            !section_overlay_visible(overlay,view_context->presentation.detail)) continue;
        const auto& binding=*overlay.dimension_binding;
        const auto target=proposed_id(binding.object_id);
        if (!stage.contains(target)) throw std::invalid_argument("Phase wall preview bound view dimension lost its owner.");
        const auto& owner=stage.at(target);
        Boundary support;
        if (owner.type=="wall" && walls.contains(target))
            support=project_shape_view(make_wall(walls.at(target)),BuildingViewKind::plan,view_context->frame);
        else if (owner.type=="opening") {
            const auto host=owner.properties.at("wall_id").get<std::string>();
            if (!walls.contains(host)) throw std::invalid_argument("Phase wall preview bound view dimension lost its opening host.");
            const auto& wall=walls.at(host);
            const auto opening=std::find_if(wall.openings.begin(),wall.openings.end(),[&](const auto& value){return value.id==target;});
            if (opening==wall.openings.end()) throw std::invalid_argument("Phase wall preview bound view dimension lost its opening.");
            support=project_shape_view(opening_solid(owner,wall,*opening),BuildingViewKind::plan,view_context->frame);
        } else if (horizontal_plan && can_recognize_boundary_entity_type(owner.type))
            support=project_path(boundary_geometry(decode_identified_boundary_entity(owner)),view_context->frame);
        else throw std::invalid_argument("Phase wall preview has an unsupported affected bound view dimension owner: "+binding.object_id);
        if (support.empty()) throw std::invalid_argument("Phase wall preview bound view dimension has no complete source silhouette.");
        const auto bounds=boundary_bounds(support);
        const bool along_horizontal=binding.axis==SectionDimensionAxis::horizontal;
        const auto coordinate=[&](Vec2 point){return along_horizontal ? point.x : point.y;};
        const auto perpendicular=[&](Vec2 point){return along_horizontal ? point.y : point.x;};
        std::optional<Vec2> first,last;
        // Match semantic silhouette handles: at each extreme take the maximum
        // perpendicular support, independent of crop/depth/visibility.
        const auto minimum=along_horizontal ? bounds.minimum.x : bounds.minimum.y;
        const auto maximum=along_horizontal ? bounds.maximum.x : bounds.maximum.y;
        const auto consider=[&](Vec2 point) {
            if (std::abs(coordinate(point)-minimum)<=default_geometry_tolerance_metres &&
                (!first || perpendicular(point)>perpendicular(*first))) first=point;
            if (std::abs(coordinate(point)-maximum)<=default_geometry_tolerance_metres &&
                (!last || perpendicular(point)>perpendicular(*last))) last=point;
        };
        for (const auto& edge:support) {
            consider(edge.start);consider(edge.end);
            if (edge.sweep_radians==0) continue;
            const auto tangent=std::tan(edge.sweep_radians*.5);
            if (!std::isfinite(tangent) || tangent==0)
                throw std::invalid_argument("Phase wall preview bound dimension arc support is unrepresentable.");
            const auto dx=edge.end.x-edge.start.x,dy=edge.end.y-edge.start.y;
            const Vec2 center{std::midpoint(edge.start.x,edge.end.x)-dy/(2*tangent),
                std::midpoint(edge.start.y,edge.end.y)+dx/(2*tangent)};
            const auto extent=segment_bounds(edge);
            const auto low=coordinate(extent.minimum),high=coordinate(extent.maximum);
            if (low<std::min(coordinate(edge.start),coordinate(edge.end)))
                consider(along_horizontal ? Vec2{low,center.y} : Vec2{center.x,low});
            if (high>std::max(coordinate(edge.start),coordinate(edge.end)))
                consider(along_horizontal ? Vec2{high,center.y} : Vec2{center.x,high});
        }
        if (!first || !last || !(maximum-minimum>default_geometry_tolerance_metres))
            throw std::invalid_argument("Phase wall preview bound view dimension silhouette extrema are unrepresentable.");
        const auto line_coordinate=(along_horizontal ? bounds.maximum.y : bounds.maximum.x)+binding.line_offset_m;
        const Vec2 line_start=along_horizontal ? Vec2{minimum,line_coordinate} : Vec2{line_coordinate,minimum};
        const Vec2 line_end=along_horizontal ? Vec2{maximum,line_coordinate} : Vec2{line_coordinate,maximum};
        const auto render_id=QString::fromStdString(view_context->view_id+"/overlay/"+overlay.id);
        const auto prototype=std::find_if(prototypes.begin(),prototypes.end(),[&](const auto& value){return value.second.id==render_id && value.second.type==QStringLiteral("section_overlay");});
        if (prototype!=prototypes.end()) {
            auto line=prototype->second;clear_geometry(line);
            if (first->x!=line_start.x || first->y!=line_start.y) line.segments.push_back({*first,line_start,0});
            if (last->x!=line_end.x || last->y!=line_end.y) line.segments.push_back({*last,line_end,0});
            line.segments.push_back({line_start,line_end,0});line.dimension_end_ticks=true;
            result.entities.push_back(std::move(line));
        }
        const auto label=std::find_if(labels.begin(),labels.end(),[&](const auto& value){return value.id==render_id;});
        if (label!=labels.end()) {
            auto projected=*label;projected.position={std::midpoint(line_start.x,line_end.x),std::midpoint(line_start.y,line_end.y)};
            projected.text=length_text(maximum-minimum,metric_units);result.labels.push_back(std::move(projected));
        }
    }
    return result;
}
} // namespace sketch::desktop

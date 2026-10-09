#include "sketch/assembly_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void identifier(const std::string& value) {
    require(!value.empty() && !std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); }), "assembly identifier/name must not be blank");
}
const char* unit_name(AssemblyQuantityUnit unit) {
    switch (unit) {
    case AssemblyQuantityUnit::count: return "count";
    case AssemblyQuantityUnit::metre: return "m";
    case AssemblyQuantityUnit::square_metre: return "m2";
    case AssemblyQuantityUnit::cubic_metre: return "m3";
    case AssemblyQuantityUnit::kilogram: return "kg";
    }
    throw std::invalid_argument("unknown assembly quantity unit");
}
void quantities(const std::map<std::string, AssemblyQuantityProperty>& values) {
    for (const auto& [key, property] : values) {
        identifier(key);
        (void)unit_name(property.unit);
        require(std::isfinite(property.value) && property.value >= 0, "assembly quantity must be finite and nonnegative");
        require(property.unit != AssemblyQuantityUnit::count || std::floor(property.value) == property.value,
            "assembly count must be integral");
    }
}
double z_scale(double scale, double vertical_scale) {
    require(std::isfinite(scale) && scale>0 && std::isfinite(vertical_scale) && vertical_scale>0,
        "assembly scales must be finite and positive");
    const auto result=scale*vertical_scale;
    require(std::isfinite(result) && result>0,"assembly Z scale overflow/underflow");
    return result;
}
// Combine positive factors without rejecting a representable result because an
// intermediate multiplication overflowed or underflowed.
double positive_product(std::initializer_list<double> factors) {
    double mantissa=1;
    int exponent=0;
    for(const auto factor:factors) {
        require(std::isfinite(factor) && factor>0,"invalid assembly product factor");
        int factor_exponent=0, adjustment=0;
        mantissa*=std::frexp(factor,&factor_exponent);
        mantissa=std::frexp(mantissa,&adjustment);
        exponent+=factor_exponent+adjustment;
    }
    const auto result=std::scalbn(mantissa,exponent);
    require(std::isfinite(result) && result>0,"assembly product overflow/underflow");
    return result;
}
void placement(const std::optional<AssemblyPlacement>& value) {
    if (!value) return;
    identifier(value->host_entity_id);
    require(std::isfinite(value->translation_m.x) && std::isfinite(value->translation_m.y) &&
            std::isfinite(value->translation_z_m),
            "assembly placement translation must be finite");
    require(std::isfinite(value->rotation_radians),
            "assembly placement rotation must be finite");
    require(std::isfinite(value->scale) && value->scale > 0.0,
            "assembly placement scale must be finite and positive");
    (void)z_scale(value->scale,value->vertical_scale);
}
void transform(const AssemblyTransform& value) {
    require(std::isfinite(value.translation_m.x) && std::isfinite(value.translation_m.y) &&
        std::isfinite(value.translation_m.z) && std::isfinite(value.rotation_radians), "assembly transform must be finite");
    require(std::isfinite(value.scale) && value.scale > 0, "assembly scale must be finite and positive");
    (void)z_scale(value.scale,value.vertical_scale);
}
bool boundary_equal(const Boundary& a, const Boundary& b) {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0;i<a.size();++i) if (a[i].start.x!=b[i].start.x || a[i].start.y!=b[i].start.y ||
        a[i].end.x!=b[i].end.x || a[i].end.y!=b[i].end.y || a[i].sweep_radians!=b[i].sweep_radians) return false;
    return true;
}
std::size_t profile_segments(const AssemblyProfile& profile) {
    require(profile.outer.size()<=1024 && profile.holes.size()<=1024, "assembly profile segment budget exceeded");
    auto count=profile.outer.size();
    for (const auto& hole:profile.holes) {
        require(hole.size()<=1024-count, "assembly profile segment budget exceeded");
        count+=hole.size();
    }
    return count;
}
double profile_volume(const AssemblyProfile& profile) {
    (void)profile_segments(profile);
    require(std::isfinite(profile.elevation_m) && std::isfinite(profile.height_m) && profile.height_m>0 &&
        std::isfinite(profile.elevation_m+profile.height_m), "invalid assembly profile extrusion");
    auto area=std::abs(signed_area(profile.outer));
    for (const auto& hole:profile.holes) area-=std::abs(signed_area(hole));
    const auto volume=area*profile.height_m;
    require(std::isfinite(area) && area>0 && std::isfinite(volume) && volume>0, "assembly profile volume overflow/underflow");
    return volume;
}
template<class T> void canonical(std::vector<T>& values) {
    std::set<std::string> ids;
    for (const auto& value : values) {
        identifier(value.id);
        require(ids.insert(value.id).second, "duplicate assembly identity");
    }
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
}
template<class T> const T& find(const std::vector<T>& values, const std::string& id) {
    const auto item = std::find_if(values.begin(), values.end(), [&](const auto& value) { return value.id == id; });
    require(item != values.end(), "unknown assembly reference");
    return *item;
}
template<class T> void overlay(std::map<std::string, T>& base, const std::map<std::string, T>& changes) {
    for (const auto& [key, value] : changes) base.at(key) = value;
}
template<class T> void override_keys(const std::map<std::string, T>& base, const std::map<std::string, T>& changes) {
    for (const auto& [key, value] : changes) {
        (void)value;
        require(base.contains(key), "assembly override references undeclared property/slot");
    }
}
void fields(const nlohmann::json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "invalid assembly JSON fields");
    for (const auto* key : keys) require(value.contains(key), "missing assembly JSON field");
}
nlohmann::json encode_quantities(const std::map<std::string, AssemblyQuantityProperty>& values) {
    auto result = nlohmann::json::object();
    for (const auto& [key, property] : values) result[key] = {{"value", property.value}, {"unit", unit_name(property.unit)}};
    return result;
}
std::map<std::string, AssemblyQuantityProperty> decode_quantities(const nlohmann::json& values) {
    require(values.is_object(), "assembly quantities must be an object");
    std::map<std::string, AssemblyQuantityProperty> result;
    for (const auto& [key, value] : values.items()) {
        fields(value, {"value", "unit"});
        require(value.at("value").is_number(), "assembly quantity value must be numeric");
        const auto name = value.at("unit").get<std::string>();
        bool known = false;
        for (const auto unit : {AssemblyQuantityUnit::count, AssemblyQuantityUnit::metre,
            AssemblyQuantityUnit::square_metre, AssemblyQuantityUnit::cubic_metre, AssemblyQuantityUnit::kilogram}) {
            if (name == unit_name(unit)) {
                result[key] = {value.at("value").get<double>(), unit}; known = true; break;
            }
        }
        require(known, "unknown assembly quantity unit");
    }
    return result;
}
template<class T> void validate_overrides(const AssemblyType& type, const T& source,
    const std::vector<AssemblyMaterial>& materials) {
    override_keys(type.properties,source.property_overrides);
    override_keys(type.materials,source.material_overrides);
    override_keys(type.quantities,source.quantity_overrides);
    for(const auto& [key,id]:source.material_overrides) { (void)key; (void)find(materials,id); }
    quantities(source.quantity_overrides);
    for(const auto& [key,value]:source.quantity_overrides)
        require(type.quantities.at(key).unit==value.unit,"assembly override changes quantity dimension");
}
const AssemblyType& path_type(const std::vector<AssemblyType>& types, const std::string& root,
    const std::vector<std::string>& path) {
    require(!path.empty() && path.size()<32,"invalid assembly override path depth");
    auto* type=&find(types,root);
    for(const auto& id:path) { identifier(id); type=&find(types,find(type->parts,id).type_id); }
    return *type;
}
void validate_instance(const AssemblyInstance& instance,const std::vector<AssemblyType>& types,
    const std::vector<AssemblyMaterial>& materials) {
    identifier(instance.id);
    validate_overrides(find(types,instance.type_id),instance,materials);
    placement(instance.placement);
    require(!instance.placement || !instance.root_transform,"assembly cannot have both independent and host placement");
    if(instance.root_transform)transform(*instance.root_transform);
    require(instance.nested_overrides.size()<=4096,"assembly override budget exceeded");
    std::set<std::vector<std::string>> paths;
    for(const auto& change:instance.nested_overrides) {
        require(paths.insert(change.part_path).second,"duplicate assembly override path");
        const auto& type=path_type(types,instance.type_id,change.part_path);
        validate_overrides(type,change,materials);
        if(change.transform)transform(*change.transform);
    }
}
void add_quantity(double& target,double value) {
    require(std::isfinite(target) && target>=0 && std::isfinite(value) && value>=0,"invalid assembly aggregate value");
    target+=value;
    require(std::isfinite(target),"assembly aggregate numeric overflow");
}
nlohmann::json encode_boundary(const Boundary& boundary) {
    auto result=nlohmann::json::array();
    for(const auto& edge:boundary) result.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    return result;
}
Boundary decode_boundary(const nlohmann::json& value) {
    require(value.is_array() && value.size()<=1024,"invalid assembly boundary segment budget");
    Boundary result;
    for(const auto& edge:value) {
        fields(edge,{"start","end","sweep_radians"});
        for(const auto* key:{"start","end"}) require(edge.at(key).is_array() && edge.at(key).size()==2 &&
            edge.at(key)[0].is_number() && edge.at(key)[1].is_number(),"invalid assembly boundary point");
        require(edge.at("sweep_radians").is_number(),"invalid assembly arc sweep");
        result.push_back({{edge.at("start")[0].get<double>(),edge.at("start")[1].get<double>()},
            {edge.at("end")[0].get<double>(),edge.at("end")[1].get<double>()},edge.at("sweep_radians").get<double>()});
    }
    return result;
}
template<class T> nlohmann::json encode_overrides(const T& value) {
    return {{"property_overrides",value.property_overrides},{"material_overrides",value.material_overrides},
        {"quantity_overrides",encode_quantities(value.quantity_overrides)}};
}
template<class T> void decode_overrides(const nlohmann::json& value,T& result) {
    using Strings=std::map<std::string,std::string>;
    result.property_overrides=value.at("property_overrides").get<Strings>();
    result.material_overrides=value.at("material_overrides").get<Strings>();
    result.quantity_overrides=decode_quantities(value.at("quantity_overrides"));
    quantities(result.quantity_overrides);
}
nlohmann::json encode_paths(const std::vector<AssemblyPathOverride>& paths) {
    auto result=nlohmann::json::array();
    auto ordered=paths;
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.part_path<b.part_path;});
    for(const auto& path:ordered) {
        auto value=encode_overrides(path);value["part_path"]=path.part_path;
        value["transform"]=path.transform ? encode_assembly_transform(*path.transform):nlohmann::json(nullptr);
        result.push_back(std::move(value));
    }
    return result;
}
AssemblyTransform decode_versioned_transform(const nlohmann::json& value,bool vertical) {
    require(vertical || !value.contains("vertical_scale"),"assembly vertical scale requires a supporting schema version");
    return decode_assembly_transform(value);
}
std::vector<AssemblyPathOverride> decode_paths(const nlohmann::json& paths,bool vertical=true) {
    require(paths.is_array() && paths.size()<=4096,"invalid assembly override collection");
    std::vector<AssemblyPathOverride> result;
    std::set<std::vector<std::string>> identities;
    for(const auto& value:paths) {
        fields(value,{"part_path","transform","property_overrides","material_overrides","quantity_overrides"});
        require(value.at("part_path").is_array() && !value.at("part_path").empty() && value.at("part_path").size()<32,"invalid assembly override path");
        AssemblyPathOverride path;path.part_path=value.at("part_path").get<std::vector<std::string>>();
        for(const auto& id:path.part_path)identifier(id);
        require(identities.insert(path.part_path).second,"duplicate assembly override path");
        if(!value.at("transform").is_null())path.transform=decode_versioned_transform(value.at("transform"),vertical);
        decode_overrides(value,path);result.push_back(std::move(path));
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.part_path<b.part_path;});
    return result;
}
} // namespace

bool AssemblyProfile::operator==(const AssemblyProfile& other) const {
    if(id!=other.id || elevation_m!=other.elevation_m || height_m!=other.height_m ||
        material_slot!=other.material_slot || !boundary_equal(outer,other.outer) || holes.size()!=other.holes.size())return false;
    for(std::size_t i=0;i<holes.size();++i)if(!boundary_equal(holes[i],other.holes[i]))return false;
    return true;
}
AssemblyPoint3 transform_assembly_point(AssemblyPoint3 point,const AssemblyTransform& value) {
    transform(value);
    require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),"assembly point must be finite");
    const auto x=point.x*value.scale,y=(value.mirrored_y ? -point.y : point.y)*value.scale,
        z=point.z*z_scale(value.scale,value.vertical_scale);
    require(std::isfinite(x) && std::isfinite(y) && std::isfinite(z),"assembly point scale overflow");
    const auto c=std::cos(value.rotation_radians),s=std::sin(value.rotation_radians);
    const AssemblyPoint3 result{c*x-s*y+value.translation_m.x,s*x+c*y+value.translation_m.y,z+value.translation_m.z};
    require(std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z),"assembly transformed point overflow");
    return result;
}
AssemblyTransform compose_assembly_transform(const AssemblyTransform& parent,const AssemblyTransform& local) {
    transform(parent);transform(local);
    AssemblyTransform result{transform_assembly_point(local.translation_m,parent),
        parent.rotation_radians+(parent.mirrored_y ? -local.rotation_radians : local.rotation_radians),
        positive_product({parent.scale,local.scale}),parent.mirrored_y!=local.mirrored_y,
        positive_product({parent.vertical_scale,local.vertical_scale})};
    transform(result);return result;
}
AssemblyPlacement transform_assembly_placement(const AssemblyPlacement& source,
    const AssemblyTransform& world_transform,bool host_geometry_transformed) {
    placement(source);transform(world_transform);
    if(world_transform==AssemblyTransform{})return source;
    const AssemblyTransform local{{source.translation_m.x,source.translation_m.y,source.translation_z_m},
        source.rotation_radians,source.scale,source.mirrored_y,source.vertical_scale};
    AssemblyTransform result;
    if(!host_geometry_transformed)result=compose_assembly_transform(world_transform,local);
    else {
        if(local==AssemblyTransform{})return source;
        // G^-1 has yaw -epsilon_G*yaw_G, the same reflection parity, and
        // scale 1/scale_G. Reduce G*A*G^-1 directly: the scale is exactly A's
        // scale, parity is A's, and yaw is epsilon_G*yaw_A + (1-epsilon_A)*yaw_G.
        // This avoids reciprocal overflow and cancellation of unchanged yaw.
        result.rotation_radians=world_transform.mirrored_y ? -local.rotation_radians:local.rotation_radians;
        if(local.mirrored_y)result.rotation_radians=std::fma(2.0,world_transform.rotation_radians,result.rotation_radians);
        result.scale=local.scale;result.mirrored_y=local.mirrored_y;
        result.vertical_scale=local.vertical_scale;
        auto linear_world=world_transform;linear_world.translation_m={};
        const auto translated=transform_assembly_point(local.translation_m,linear_world);
        const auto offset=transform_assembly_point({world_transform.translation_m.x,world_transform.translation_m.y,0},result);
        result.translation_m={translated.x+(world_transform.translation_m.x-offset.x),
            translated.y+(world_transform.translation_m.y-offset.y),
            std::fma(1-z_scale(local.scale,local.vertical_scale),world_transform.translation_m.z,translated.z)};
        transform(result);
    }
    if(result==local)return source;
    AssemblyPlacement placed=source;
    placed.translation_m={result.translation_m.x,result.translation_m.y};
    placed.translation_z_m=result.translation_m.z;
    placed.rotation_radians=result.rotation_radians;placed.scale=result.scale;placed.mirrored_y=result.mirrored_y;
    placed.vertical_scale=result.vertical_scale;
    placement(placed);return placed;
}
nlohmann::json encode_assembly_transform(const AssemblyTransform& value) {
    transform(value);
    nlohmann::json result{{"translation_m",{value.translation_m.x,value.translation_m.y,value.translation_m.z}},
        {"rotation_radians",value.rotation_radians},{"scale",value.scale}};
    if(value.mirrored_y)result["mirrored_y"]=true;
    if(value.vertical_scale!=1)result["vertical_scale"]=value.vertical_scale;
    return result;
}
AssemblyTransform decode_assembly_transform(const nlohmann::json& value) {
    try {
        require(value.is_object(),"invalid assembly transform fields");
        const bool mirror=value.contains("mirrored_y"),vertical=value.contains("vertical_scale");
        require(value.size()==3u+static_cast<unsigned>(mirror)+static_cast<unsigned>(vertical) &&
            value.contains("translation_m") && value.contains("rotation_radians") && value.contains("scale"),
            "invalid assembly transform fields");
        if(mirror)require(value.at("mirrored_y").is_boolean(),"assembly mirror parity must be boolean");
        if(vertical)require(value.at("vertical_scale").is_number(),"assembly vertical scale must be numeric");
        const auto& p=value.at("translation_m");
        require(p.is_array() && p.size()==3 && p[0].is_number() && p[1].is_number() && p[2].is_number() &&
            value.at("rotation_radians").is_number() && value.at("scale").is_number(),"invalid assembly XYZ transform");
        AssemblyTransform result{{p[0].get<double>(),p[1].get<double>(),p[2].get<double>()},
            value.at("rotation_radians").get<double>(),value.at("scale").get<double>(),
            mirror && value.at("mirrored_y").get<bool>(),vertical ? value.at("vertical_scale").get<double>():1.0};
        transform(result);return result;
    } catch(const nlohmann::json::exception& error) {throw std::invalid_argument(std::string("invalid assembly transform JSON: ")+error.what());}
}
nlohmann::json encode_assembly_instance(const AssemblyInstance& instance) {
    identifier(instance.id);identifier(instance.type_id);
    require(instance.root_transform.has_value() && !instance.placement,"independent assembly instance requires root transform and no host");
    quantities(instance.quantity_overrides);
    auto result=encode_overrides(instance);
    const bool vertical=instance.root_transform->vertical_scale!=1 ||
        std::any_of(instance.nested_overrides.begin(),instance.nested_overrides.end(),
            [](const auto& path){return path.transform && path.transform->vertical_scale!=1;});
    result["schema"]=vertical ? "sketch.assembly-instance.v2":"sketch.assembly-instance.v1";
    result["id"]=instance.id;result["type_id"]=instance.type_id;
    result["root_transform"]=encode_assembly_transform(*instance.root_transform);
    result["nested_overrides"]=encode_paths(instance.nested_overrides);
    // Establish the same lexical/path/finite validation on both codec directions.
    (void)decode_assembly_instance(result);
    return result;
}
AssemblyInstance decode_assembly_instance(const nlohmann::json& value) {
    try {
        fields(value,{"schema","id","type_id","property_overrides","material_overrides","quantity_overrides","root_transform","nested_overrides"});
        const bool vertical=value.at("schema")=="sketch.assembly-instance.v2";
        require(vertical || value.at("schema")=="sketch.assembly-instance.v1","unsupported independent assembly instance schema");
        AssemblyInstance result;result.id=value.at("id").get<std::string>();result.type_id=value.at("type_id").get<std::string>();
        identifier(result.id);identifier(result.type_id);decode_overrides(value,result);
        result.root_transform=decode_versioned_transform(value.at("root_transform"),vertical);
        result.nested_overrides=decode_paths(value.at("nested_overrides"),vertical);
        return result;
    } catch(const nlohmann::json::exception& error) {throw std::invalid_argument(std::string("invalid assembly instance JSON: ")+error.what());}
}

AssemblyModel AssemblyModel::create(std::vector<AssemblyMaterial> materials,
    std::vector<AssemblyType> types, std::vector<AssemblyInstance> instances) {
    require(types.size()<=4096 && instances.size()<=4096,"assembly catalog collection budget exceeded");
    canonical(materials); canonical(types); canonical(instances);
    for (const auto& material : materials) {
        identifier(material.name);
        if (material.color_srgb) {
            const auto& color = *material.color_srgb;
            require(color.size() == 7 && color.front() == '#' &&
                std::all_of(color.begin() + 1, color.end(), [](char c) {
                    return std::string_view("0123456789abcdefABCDEF").find(c) != std::string_view::npos;
                }), "material color must be #RRGGBB in sRGB");
        }
    }
    for (auto& type : types) {
        identifier(type.name);
        for (const auto& [key, value] : type.properties) { (void)value; identifier(key); }
        for (const auto& [key, id] : type.materials) { identifier(key); (void)find(materials, id); }
        quantities(type.quantities);
        require(type.profiles.size()<=4096 && type.parts.size()<=4096,"assembly type collection budget exceeded");
        // Profile/part order is authored presentation order. Stable identities
        // remain unique, but reordering never replaces those identities.
        const auto ordered_ids = [](const auto& records) {
            std::set<std::string> ids;
            for (const auto& record : records) {
                identifier(record.id);
                require(ids.insert(record.id).second, "duplicate assembly profile/part identifier");
            }
        };
        ordered_ids(type.profiles); ordered_ids(type.parts);
        for(const auto& profile:type.profiles) {
            (void)profile_segments(profile);
            require(!validate_boundary_holes(profile.outer,profile.holes).has_value(), "invalid assembly profile boundary/holes");
            (void)profile_volume(profile);
            if(profile.material_slot)require(type.materials.contains(*profile.material_slot),"assembly profile references unknown material slot");
        }
        for(const auto& part:type.parts) {
            transform(part.transform);
            validate_overrides(find(types,part.type_id),part,materials);
        }
    }
    // Validate every graph, even types with no current root instance. Memoized
    // subtree size/depth prevents exponential validation of shared subgraphs.
    struct GraphSize {std::size_t nodes{1},depth{1},profile_segments{};};
    std::map<std::string,int> colors;
    std::map<std::string,GraphSize> sizes;
    std::function<GraphSize(const AssemblyType&,std::size_t)> graph;
    graph=[&](const AssemblyType& type,std::size_t depth) -> GraphSize {
        require(depth<=32,"assembly graph depth budget exceeded");
        require(colors[type.id]!=1,"assembly type graph cycle");
        if(colors[type.id]==2)return sizes.at(type.id);
        colors[type.id]=1;GraphSize result;
        for (const auto& profile:type.profiles) {
            const auto count=profile_segments(profile);
            require(count<=262144-result.profile_segments,"assembly expansion profile segment budget exceeded");
            result.profile_segments+=count;
        }
        for(const auto& part:type.parts) {
            const auto child=graph(find(types,part.type_id),depth+1);
            require(child.nodes<=4096-result.nodes,"assembly expansion node budget exceeded");
            require(child.profile_segments<=262144-result.profile_segments,
                "assembly expansion profile segment budget exceeded");
            result.profile_segments+=child.profile_segments;
            result.nodes+=child.nodes;result.depth=std::max(result.depth,child.depth+1);
            require(result.depth<=32,"assembly graph depth budget exceeded");
        }
        colors[type.id]=2;sizes[type.id]=result;return result;
    };
    std::size_t validation_nodes=0,validation_segments=0;
    // Every type is probed below, including unused shared subgraphs. Preflight
    // their combined work before expanding any graph; per-root caps alone
    // would let a small catalog trigger billions of repeated segment copies.
    for(const auto& type:types) {
        const auto size=graph(type,1);
        require(size.nodes<=16384-validation_nodes,
            "assembly catalog validation node work budget exceeded");
        require(size.profile_segments<=1048576-validation_segments,
            "assembly catalog validation profile work budget exceeded");
        validation_nodes+=size.nodes;validation_segments+=size.profile_segments;
    }
    for (auto& instance : instances) {
        validate_instance(instance,types,materials);
        std::sort(instance.nested_overrides.begin(),instance.nested_overrides.end(),
            [](const auto& a,const auto& b){return a.part_path<b.part_path;});
    }
    AssemblyModel model;
    model.materials_ = std::move(materials); model.types_ = std::move(types); model.instances_ = std::move(instances);
    // A catalog must also refuse unused transformed graphs with overflowing
    // coordinates, volumes, quantities or profile expansion segment budgets.
    for(const auto& type:model.types_) {
        AssemblyInstance probe;probe.id="validation";probe.type_id=type.id;
        AssemblyExpansionBudget budget;(void)model.expand(probe,budget);
    }
    AssemblyExpansionBudget document_budget;
    for(const auto& instance:model.instances_)(void)model.expand(instance,document_budget);
    return model;
}
ResolvedAssembly AssemblyModel::resolve(const std::string& instance_id) const {
    const auto& instance = find(instances_, instance_id);
    const auto& type = find(types_, instance.type_id);
    ResolvedAssembly result{instance.id, type.id, type.properties, type.materials, type.quantities};
    overlay(result.properties, instance.property_overrides);
    overlay(result.materials, instance.material_overrides);
    overlay(result.quantities, instance.quantity_overrides);
    return result;
}
AssemblyExpansion AssemblyModel::expand(const std::string& instance_id) const {
    AssemblyExpansionBudget budget;return expand(find(instances_,instance_id),budget);
}
AssemblyExpansion AssemblyModel::expand(const AssemblyInstance& instance,AssemblyExpansionBudget& budget) const {
    require(budget.max_nodes<=4096 && budget.max_profile_segments<=262144 &&
        budget.consumed_nodes<=budget.max_nodes && budget.consumed_profile_segments<=budget.max_profile_segments,
        "invalid assembly expansion budget");
    require(std::isfinite(budget.volume_m3) && budget.volume_m3>=0,"invalid assembly aggregate volume");
    for(const auto& [key,value]:budget.declared_quantities) {
        identifier(key.name);(void)unit_name(key.unit);
        require(std::isfinite(value) && value>=0,"invalid assembly aggregate quantity");
    }
    for(const auto& [id,value]:budget.material_volumes_m3) {
        identifier(id);require(std::isfinite(value) && value>=0,"invalid assembly aggregate material volume");
    }
    validate_instance(instance,types_,materials_);
    auto pending=budget;
    AssemblyExpansion result;result.source_instance=instance;
    std::map<std::pair<std::string,std::string>,double> local_volumes;
    std::map<std::vector<std::string>,const AssemblyPathOverride*> changes;
    for(const auto& change:instance.nested_overrides)changes.emplace(change.part_path,&change);
    AssemblyTransform root=instance.root_transform.value_or(AssemblyTransform{});
    if(instance.placement)root={{instance.placement->translation_m.x,instance.placement->translation_m.y,instance.placement->translation_z_m},
        instance.placement->rotation_radians,instance.placement->scale,instance.placement->mirrored_y,
        instance.placement->vertical_scale};
    std::function<void(const AssemblyType&,std::vector<std::string>,AssemblyTransform,const AssemblyPart*)> visit;
    visit=[&](const AssemblyType& type,std::vector<std::string> path,AssemblyTransform world,const AssemblyPart* part) {
        require(path.size()<32,"assembly graph depth budget exceeded");
        require(pending.consumed_nodes<pending.max_nodes,"assembly document expansion node budget exceeded");
        ++pending.consumed_nodes;
        AssemblyExpandedNode node{path,type.id,world,type.properties,type.materials,type.quantities,std::nullopt,std::nullopt};
        if(part) {
            node.source_part=*part;overlay(node.properties,part->property_overrides);
            overlay(node.materials,part->material_overrides);overlay(node.quantities,part->quantity_overrides);
        } else {
            overlay(node.properties,instance.property_overrides);overlay(node.materials,instance.material_overrides);
            overlay(node.quantities,instance.quantity_overrides);
        }
        if(const auto found=changes.find(path);found!=changes.end()) {
            const auto& change=*found->second;node.path_override=change;
            overlay(node.properties,change.property_overrides);overlay(node.materials,change.material_overrides);
            overlay(node.quantities,change.quantity_overrides);
        }
        for(const auto& [key,value]:node.quantities)add_quantity(result.declared_quantities[{key,value.unit}],value.value);
        for(const auto& profile:type.profiles) {
            const auto segments=profile_segments(profile);
            require(segments<=pending.max_profile_segments-pending.consumed_profile_segments,"assembly document profile segment budget exceeded");
            pending.consumed_profile_segments+=segments;
            // Analytical arc bounds also cover extrema not present at vertices.
            const auto validate_points=[&](const Boundary& boundary) {
                const auto bounds=boundary_bounds(boundary);
                for(const auto point:{bounds.minimum,bounds.maximum,
                    Vec2{bounds.minimum.x,bounds.maximum.y},Vec2{bounds.maximum.x,bounds.minimum.y}}) {
                    (void)transform_assembly_point({point.x,point.y,profile.elevation_m},world);
                    (void)transform_assembly_point({point.x,point.y,profile.elevation_m+profile.height_m},world);
                }
            };
            validate_points(profile.outer);for(const auto& hole:profile.holes)validate_points(hole);
            const auto key=std::pair{type.id,profile.id};
            auto found_volume=local_volumes.find(key);
            if(found_volume==local_volumes.end())found_volume=local_volumes.emplace(key,profile_volume(profile)).first;
            const auto volume=positive_product({found_volume->second,world.scale,world.scale,
                z_scale(world.scale,world.vertical_scale)});
            require(std::isfinite(volume) && volume>0,"assembly computed volume overflow/underflow");
            std::optional<std::string> material;
            if(profile.material_slot)material=node.materials.at(*profile.material_slot);
            result.profiles.push_back({path,type.id,profile,world,material,volume});
            add_quantity(result.volume_m3,volume);
            if(material)add_quantity(result.material_volumes_m3[*material],volume);
        }
        result.nodes.push_back(std::move(node));
        for(const auto& child:type.parts) {
            auto child_path=path;child_path.push_back(child.id);
            auto local=child.transform;
            if(const auto found=changes.find(child_path);found!=changes.end() && found->second->transform)
                local=*found->second->transform;
            visit(find(types_,child.type_id),std::move(child_path),compose_assembly_transform(world,local),&child);
        }
    };
    visit(find(types_,instance.type_id),{},root,nullptr);
    for(const auto& [key,value]:result.declared_quantities)add_quantity(pending.declared_quantities[key],value);
    for(const auto& [key,value]:result.material_volumes_m3)add_quantity(pending.material_volumes_m3[key],value);
    add_quantity(pending.volume_m3,result.volume_m3);
    budget=std::move(pending);return result;
}
AssemblyModel AssemblyModel::with_type(AssemblyType replacement) const {
    (void)find(types_, replacement.id);
    auto types = types_;
    for (auto& type : types) if (type.id == replacement.id) { type = std::move(replacement); break; }
    return create(materials_, std::move(types), instances_);
}
AssemblyModel AssemblyModel::with_instance(AssemblyInstance replacement) const {
    (void)find(instances_, replacement.id);
    auto instances = instances_;
    for (auto& instance : instances) if (instance.id == replacement.id) { instance = std::move(replacement); break; }
    return create(materials_, types_, std::move(instances));
}
AssemblyModel AssemblyModel::without_type(const std::string& type_id) const {
    (void)find(types_,type_id);
    for(const auto& type:types_)for(const auto& part:type.parts)
        require(part.type_id!=type_id,"cannot delete referenced assembly type");
    for(const auto& instance:instances_)require(instance.type_id!=type_id,"cannot delete referenced assembly type");
    auto types=types_;std::erase_if(types,[&](const auto& type){return type.id==type_id;});
    return create(materials_,std::move(types),instances_);
}
std::vector<AssemblyTypeUpdateImpact> AssemblyModel::preview_type_update(AssemblyType replacement) const {
    const auto id = replacement.id;
    const auto updated = with_type(std::move(replacement));
    std::vector<AssemblyTypeUpdateImpact> impacts;
    for (const auto& instance : instances_) {
        const auto expansion=expand(instance.id);
        if(std::any_of(expansion.nodes.begin(),expansion.nodes.end(),[&](const auto& node){return node.type_id==id;}))
            impacts.push_back({instance.id, resolve(instance.id), updated.resolve(instance.id), instance,
                expansion,updated.expand(instance.id)});
    }
    return impacts;
}
nlohmann::json AssemblyModel::to_json() const {
    nlohmann::json result{{"schema", "sketch.assemblies.v1"}, {"materials", nlohmann::json::array()},
        {"types", nlohmann::json::array()}, {"instances", nlohmann::json::array()}};
    bool has_placement = false;
    bool placement_xyz = false;
    bool vertical = false;
    bool nested = false;
    for(const auto& type:types_)nested=nested || !type.profiles.empty() || !type.parts.empty();
    for(const auto& type:types_)for(const auto& part:type.parts)vertical=vertical || part.transform.vertical_scale!=1;
    for(const auto& instance:instances_)nested=nested || instance.root_transform.has_value() || !instance.nested_overrides.empty();
    for (const auto& instance : instances_) {
        has_placement = has_placement || instance.placement.has_value();
        placement_xyz = placement_xyz || (instance.placement && instance.placement->translation_z_m!=0);
        vertical=vertical || (instance.placement && instance.placement->vertical_scale!=1) ||
            (instance.root_transform && instance.root_transform->vertical_scale!=1);
        for(const auto& path:instance.nested_overrides)vertical=vertical || (path.transform && path.transform->vertical_scale!=1);
    }
    // V5/V6 share V4's complete nested envelope even for a legacy-only catalog.
    placement_xyz=placement_xyz || vertical;
    nested = nested || placement_xyz;
    if (has_placement) result["schema"] = "sketch.assemblies.v3";
    for (const auto& material : materials_) {
        nlohmann::json value{{"id", material.id}, {"name", material.name}};
        if (material.color_srgb) {
            if (!has_placement) result["schema"] = "sketch.assemblies.v2";
            value["color_srgb"] = *material.color_srgb;
        }
        result["materials"].push_back(std::move(value));
    }
    if(nested)result["schema"]="sketch.assemblies.v4";
    if(placement_xyz)result["schema"]="sketch.assemblies.v5";
    if(vertical)result["schema"]="sketch.assemblies.v6";
    for (const auto& type : types_) {
        nlohmann::json value{{"id",type.id},{"name",type.name},{"properties",type.properties},
            {"materials",type.materials},{"quantities",encode_quantities(type.quantities)}};
        if(nested) {
            value["profiles"]=nlohmann::json::array();value["parts"]=nlohmann::json::array();
            for(const auto& profile:type.profiles) {
                auto holes=nlohmann::json::array();for(const auto& hole:profile.holes)holes.push_back(encode_boundary(hole));
                value["profiles"].push_back({{"id",profile.id},{"outer",encode_boundary(profile.outer)},
                    {"holes",std::move(holes)},{"elevation_m",profile.elevation_m},{"height_m",profile.height_m},
                    {"material_slot",profile.material_slot ? nlohmann::json(*profile.material_slot):nlohmann::json(nullptr)}});
            }
            for(const auto& part:type.parts) {
                auto encoded=encode_overrides(part);encoded["id"]=part.id;encoded["type_id"]=part.type_id;
                encoded["transform"]=encode_assembly_transform(part.transform);value["parts"].push_back(std::move(encoded));
            }
        }
        result["types"].push_back(std::move(value));
    }
    for (const auto& instance : instances_) {
        nlohmann::json value{{"id", instance.id}, {"type_id", instance.type_id},
            {"property_overrides", instance.property_overrides},
            {"material_overrides", instance.material_overrides},
            {"quantity_overrides", encode_quantities(instance.quantity_overrides)}};
        if (instance.placement) {
            value["placement"] = {
                {"host_entity_id", instance.placement->host_entity_id},
                {"translation_m", {instance.placement->translation_m.x,
                                      instance.placement->translation_m.y}},
                {"rotation_radians", instance.placement->rotation_radians},
                {"scale", instance.placement->scale}};
            if(instance.placement->mirrored_y)value["placement"]["mirrored_y"]=true;
            if(placement_xyz)value["placement"]["translation_m"].push_back(instance.placement->translation_z_m);
            if(vertical)value["placement"]["vertical_scale"]=instance.placement->vertical_scale;
        }
        if(nested) {
            value["root_transform"]=instance.root_transform ? encode_assembly_transform(*instance.root_transform):nlohmann::json(nullptr);
            value["nested_overrides"]=encode_paths(instance.nested_overrides);
        }
        result["instances"].push_back(std::move(value));
    }
    return result;
}
AssemblyModel AssemblyModel::from_json(const nlohmann::json& value) {
    try {
        fields(value, {"schema", "materials", "types", "instances"});
        const auto schema = value.at("schema").get<std::string>();
        const bool vertical = schema == "sketch.assemblies.v6";
        const bool placement_xyz = schema == "sketch.assemblies.v5" || vertical;
        const bool nested = schema == "sketch.assemblies.v4" || placement_xyz;
        const bool appearance = schema == "sketch.assemblies.v2" || schema == "sketch.assemblies.v3" || nested;
        const bool placements = schema == "sketch.assemblies.v3" || nested;
        require(appearance || schema == "sketch.assemblies.v1", "unsupported assembly schema");
        for (const auto* key : {"materials", "types", "instances"})
            require(value.at(key).is_array(), "assembly collections must be arrays");
        require(value.at("types").size()<=4096 && value.at("instances").size()<=4096,"assembly catalog collection budget exceeded");
        std::vector<AssemblyMaterial> materials;
        std::vector<AssemblyType> types;
        std::vector<AssemblyInstance> instances;
        using Strings = std::map<std::string, std::string>;
        for (const auto& item : value.at("materials")) {
            if (appearance && item.contains("color_srgb")) fields(item, {"id", "name", "color_srgb"});
            else fields(item, {"id", "name"});
            materials.push_back({item.at("id").get<std::string>(), item.at("name").get<std::string>(),
                item.contains("color_srgb") ? std::optional{item.at("color_srgb").get<std::string>()} : std::nullopt});
        }
        for (const auto& item : value.at("types")) {
            if(nested)fields(item,{"id","name","properties","materials","quantities","profiles","parts"});
            else fields(item, {"id", "name", "properties", "materials", "quantities"});
            AssemblyType type{item.at("id").get<std::string>(), item.at("name").get<std::string>(),
                item.at("properties").get<Strings>(), item.at("materials").get<Strings>(), decode_quantities(item.at("quantities"))};
            if(nested) {
                require(item.at("profiles").is_array() && item.at("profiles").size()<=4096 &&
                    item.at("parts").is_array() && item.at("parts").size()<=4096,"invalid assembly type collections");
                for(const auto& encoded:item.at("profiles")) {
                    fields(encoded,{"id","outer","holes","elevation_m","height_m","material_slot"});
                    require(encoded.at("holes").is_array() && encoded.at("holes").size()<=1024 &&
                        encoded.at("elevation_m").is_number() && encoded.at("height_m").is_number(),"invalid assembly profile");
                    AssemblyProfile profile;profile.id=encoded.at("id").get<std::string>();profile.outer=decode_boundary(encoded.at("outer"));
                    auto segment_count=profile.outer.size();
                    for(const auto& hole:encoded.at("holes")) {
                        require(hole.is_array() && hole.size()<=1024-segment_count,"assembly profile segment budget exceeded");
                        segment_count+=hole.size();profile.holes.push_back(decode_boundary(hole));
                    }
                    profile.elevation_m=encoded.at("elevation_m").get<double>();profile.height_m=encoded.at("height_m").get<double>();
                    if(!encoded.at("material_slot").is_null())profile.material_slot=encoded.at("material_slot").get<std::string>();
                    type.profiles.push_back(std::move(profile));
                }
                for(const auto& encoded:item.at("parts")) {
                    fields(encoded,{"id","type_id","transform","property_overrides","material_overrides","quantity_overrides"});
                    AssemblyPart part;part.id=encoded.at("id").get<std::string>();part.type_id=encoded.at("type_id").get<std::string>();
                    part.transform=decode_versioned_transform(encoded.at("transform"),vertical);decode_overrides(encoded,part);
                    type.parts.push_back(std::move(part));
                }
            }
            types.push_back(std::move(type));
        }
        for (const auto& item : value.at("instances")) {
            const bool has_placement = item.is_object() && item.contains("placement");
            require(!has_placement || placements, "assembly placement requires schema v3");
            if(nested && has_placement)fields(item,{"id","type_id","property_overrides","material_overrides","quantity_overrides","placement","root_transform","nested_overrides"});
            else if(nested)fields(item,{"id","type_id","property_overrides","material_overrides","quantity_overrides","root_transform","nested_overrides"});
            else if (has_placement) fields(item, {"id", "type_id", "property_overrides", "material_overrides", "quantity_overrides", "placement"});
            else fields(item, {"id", "type_id", "property_overrides", "material_overrides", "quantity_overrides"});
            std::optional<AssemblyPlacement> decoded_placement;
            if (has_placement) {
                const auto& encoded = item.at("placement");
                const bool mirror=encoded.is_object() && encoded.contains("mirrored_y");
                if(vertical && mirror)fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale","mirrored_y","vertical_scale"});
                else if(vertical)fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale","vertical_scale"});
                else if(mirror)fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale","mirrored_y"});
                else fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale"});
                if(mirror) {
                    require(encoded.at("mirrored_y").is_boolean(),"assembly placement mirror parity must be boolean");
                }
                require(encoded.at("host_entity_id").is_string() &&
                        encoded.at("translation_m").is_array() &&
                        encoded.at("translation_m").size() == (placement_xyz ? 3u:2u) &&
                        encoded.at("translation_m")[0].is_number() &&
                        encoded.at("translation_m")[1].is_number() &&
                        (!placement_xyz || encoded.at("translation_m")[2].is_number()) &&
                        encoded.at("rotation_radians").is_number() &&
                        encoded.at("scale").is_number() &&
                        (!vertical || encoded.at("vertical_scale").is_number()), "invalid assembly placement");
                decoded_placement = AssemblyPlacement{
                    encoded.at("host_entity_id").get<std::string>(),
                    {encoded.at("translation_m")[0].get<double>(),
                     encoded.at("translation_m")[1].get<double>()},
                    encoded.at("rotation_radians").get<double>(),
                    encoded.at("scale").get<double>(),
                    encoded.contains("mirrored_y") && encoded.at("mirrored_y").get<bool>(),
                    placement_xyz ? encoded.at("translation_m")[2].get<double>():0.0,
                    vertical ? encoded.at("vertical_scale").get<double>():1.0};
            }
            AssemblyInstance instance{item.at("id").get<std::string>(), item.at("type_id").get<std::string>(),
                item.at("property_overrides").get<Strings>(), item.at("material_overrides").get<Strings>(),
                decode_quantities(item.at("quantity_overrides")), std::move(decoded_placement)};
            if(nested) {
                if(!item.at("root_transform").is_null())instance.root_transform=decode_versioned_transform(item.at("root_transform"),vertical);
                instance.nested_overrides=decode_paths(item.at("nested_overrides"),vertical);
            }
            instances.push_back(std::move(instance));
        }
        return create(std::move(materials), std::move(types), std::move(instances));
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly JSON: ") + error.what());
    }
}
nlohmann::json transform_hosted_assembly_model(const nlohmann::json& actual_model,
    const std::map<std::string,AssemblyTransform,std::less<>>& instance_transforms) {
    const auto source=AssemblyModel::from_json(actual_model);
    std::map<std::string,AssemblyPlacement,std::less<>> changed;
    AssemblyExpansionBudget source_budget;
    bool needs_xyz=false;
    bool needs_vertical=false;
    for(const auto& [id,world_transform]:instance_transforms) {
        const auto& instance=find(source.instances(),id);
        require(instance.placement.has_value(),"assembly transformation requires an actual hosted instance");
        const auto expansion=source.expand(instance,source_budget);
        const auto placed=transform_assembly_placement(*instance.placement,world_transform,expansion.profiles.empty());
        if(placed!=*instance.placement) {
            needs_xyz=needs_xyz || placed.translation_z_m!=0;
            needs_vertical=needs_vertical || placed.vertical_scale!=1;
            changed.emplace(id,placed);
        }
    }
    if(changed.empty())return actual_model;
    auto result=actual_model;
    const auto schema=actual_model.at("schema").get<std::string>();
    const bool source_xyz=schema=="sketch.assemblies.v5" || schema=="sketch.assemblies.v6";
    const bool upgrade_vertical=needs_vertical && schema!="sketch.assemblies.v6";
    const bool upgrade_xyz=(needs_xyz || needs_vertical) && !source_xyz;
    if(upgrade_xyz || upgrade_vertical) {
        result["schema"]=upgrade_vertical ? "sketch.assemblies.v6":"sketch.assemblies.v5";
        if(schema=="sketch.assemblies.v3") {
            for(auto& type:result.at("types")) {
                type["profiles"]=nlohmann::json::array();type["parts"]=nlohmann::json::array();
            }
            for(auto& instance:result.at("instances")) {
                instance["root_transform"]=nullptr;instance["nested_overrides"]=nlohmann::json::array();
            }
        }
        for(auto& instance:result.at("instances"))if(instance.contains("placement")) {
            if(upgrade_xyz)instance.at("placement").at("translation_m").push_back(0.0);
            if(upgrade_vertical)instance.at("placement")["vertical_scale"]=1.0;
        }
    }
    for(auto& encoded:result.at("instances")) {
        const auto id=encoded.at("id").get<std::string>();
        const auto update=changed.find(id);
        if(update==changed.end())continue;
        const auto& before=*find(source.instances(),id).placement;
        const auto& after=update->second;
        auto& placed=encoded.at("placement");
        auto& p=placed.at("translation_m");
        if(before.translation_m.x!=after.translation_m.x)p[0]=after.translation_m.x;
        if(before.translation_m.y!=after.translation_m.y)p[1]=after.translation_m.y;
        if(before.translation_z_m!=after.translation_z_m)p[2]=after.translation_z_m;
        if(before.rotation_radians!=after.rotation_radians)placed["rotation_radians"]=after.rotation_radians;
        if(before.scale!=after.scale)placed["scale"]=after.scale;
        if(before.vertical_scale!=after.vertical_scale)placed["vertical_scale"]=after.vertical_scale;
        if(before.mirrored_y!=after.mirrored_y)placed["mirrored_y"]=after.mirrored_y;
    }
    // The strict codec validates every final instance with one document budget,
    // including aggregate quantities, geometry and transformed profile bounds.
    (void)AssemblyModel::from_json(result);
    return result;
}
nlohmann::json retain_assembly_catalog_dialect(
    const nlohmann::json& actual_model, nlohmann::json generated_model) {
    const auto saved=actual_model.value("schema", nlohmann::json{});
    const auto generated=generated_model.at("schema");
    const bool keep_vertical=saved=="sketch.assemblies.v6" && generated!=saved;
    const bool keep_xyz=saved=="sketch.assemblies.v5" && generated!=saved &&
        generated!="sketch.assemblies.v6";
    if(!keep_vertical && !keep_xyz)return generated_model;
    generated_model["schema"]=saved;
    for(auto& type:generated_model.at("types")) {
        if(!type.contains("profiles"))type["profiles"]=nlohmann::json::array();
        if(!type.contains("parts"))type["parts"]=nlohmann::json::array();
    }
    for(auto& instance:generated_model.at("instances")) {
        if(!instance.contains("root_transform"))instance["root_transform"]=nullptr;
        if(!instance.contains("nested_overrides"))instance["nested_overrides"]=nlohmann::json::array();
        if(!instance.contains("placement"))continue;
        auto& placed=instance.at("placement");
        if(placed.at("translation_m").size()==2)placed.at("translation_m").push_back(0.0);
        if(keep_vertical && !placed.contains("vertical_scale"))placed["vertical_scale"]=1.0;
    }
    (void)AssemblyModel::from_json(generated_model);
    return generated_model;
}
} // namespace sketch

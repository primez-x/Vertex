#include "sketch/assembly_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <numbers>
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
void portable_topology_identifier(std::string_view value) {
    require(value.find('\0')==std::string_view::npos,"assembly profile topology identity contains NUL");
    for(std::size_t i=0;i<value.size();++i) {
        const auto c=static_cast<unsigned char>(value[i]);
        if(c<0x80)continue;
        const std::size_t tail=c>=0xc2 && c<=0xdf ? 1 :
            c>=0xe0 && c<=0xef ? 2 : c>=0xf0 && c<=0xf4 ? 3 : 0;
        require(tail && tail<value.size()-i,"assembly profile topology identity contains invalid UTF-8");
        const auto first=static_cast<unsigned char>(value[i+1]);
        require(!((c==0xe0 && first<0xa0) || (c==0xed && first>=0xa0) ||
            (c==0xf0 && first<0x90) || (c==0xf4 && first>=0x90)),
            "assembly profile topology identity contains invalid UTF-8");
        for(std::size_t j=1;j<=tail;++j) {
            const auto byte=static_cast<unsigned char>(value[i+j]);
            require(byte>=0x80 && byte<=0xbf,"assembly profile topology identity contains invalid UTF-8");
        }
        i+=tail;
    }
}
constexpr std::size_t profile_topology_byte_limit=1024*1024;
constexpr std::size_t collection_topology_byte_limit=64*1024*1024;
constexpr std::size_t catalog_topology_byte_limit=256*1024*1024;
constexpr std::size_t mapped_profile_wire_bytes=512;
// Upper bounds escaped JSON output, including punctuation/member names. Raw
// strings are inspected by reference before any topology arrays are copied.
struct ProfileTopologyAdmission {
    std::size_t bytes{128};
    std::set<std::string_view> rings,segments,vertices;
    void add(const std::string& id,std::set<std::string_view>& identities) {
        require(id.size()<=128,"assembly profile topology identifier budget exceeded");
        identifier(id);
        portable_topology_identifier(id);
        require(identities.insert(id).second,"duplicate assembly profile topology identity");
        const auto count=6*id.size()+4;
        require(count<=profile_topology_byte_limit-bytes,"assembly profile topology output budget exceeded");
        bytes+=count;
    }
    void ring(const std::string& id) {
        require(128<=profile_topology_byte_limit-bytes,"assembly profile topology output budget exceeded");
        bytes+=128;add(id,rings);
    }
};
std::size_t profile_topology_bytes(const AssemblyProfile& profile) {
    if(!profile.topology)return profile.planar_affine ? mapped_profile_wire_bytes:0;
    require(profile.outer.size()<=1024 && profile.holes.size()<=1024,
        "assembly profile topology ring budget exceeded");
    const auto& topology=*profile.topology;
    require(topology.holes.size()==profile.holes.size(),"assembly profile topology hole count mismatch");
    ProfileTopologyAdmission admission;
    if(profile.planar_affine)admission.bytes+=mapped_profile_wire_bytes;
    std::size_t count=0;
    const auto ring=[&](const AssemblyProfileRingIdentity& identity,const Boundary& boundary) {
        require(!boundary.empty() && boundary.size()<=1024-count,"assembly profile topology segment budget exceeded");
        count+=boundary.size();
        require(identity.segment_ids.size()==boundary.size() && identity.vertex_ids.size()==boundary.size(),
            "assembly profile topology segment/vertex count mismatch");
        admission.ring(identity.ring_id);
        for(const auto& id:identity.segment_ids)admission.add(id,admission.segments);
        for(const auto& id:identity.vertex_ids)admission.add(id,admission.vertices);
    };
    ring(topology.outer,profile.outer);
    for(std::size_t i=0;i<topology.holes.size();++i)ring(topology.holes[i],profile.holes[i]);
    return admission.bytes;
}
void add_topology_bytes(std::size_t& target,std::size_t count,std::size_t limit) {
    require(target<=limit && count<=limit-target,"assembly profile topology collection output budget exceeded");
    target+=count;
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
constexpr std::size_t mapped_profile_work_limit=67108864;
void validate_mapped_work_budget(const AssemblyExpansionBudget& budget) {
    require(budget.max_mapped_profile_work<=mapped_profile_work_limit &&
        budget.consumed_mapped_profile_work<=budget.max_mapped_profile_work,
        "invalid assembly mapped profile work budget");
}
void reserve_mapped_profile_work(AssemblyExpansionBudget& budget,std::size_t segments,
    std::size_t holes,std::size_t arcs) {
    validate_mapped_work_budget(budget);
    require(segments<=1024 && holes<=1024 && arcs<=segments,
        "assembly mapped profile structural budget exceeded");
    const auto topology=segments+holes+1;
    // Three conservative analytical passes, matching complete-catalog raw
    // admission. Check each factor against the remaining allowance before
    // multiplication, including on 32-bit size_t and lowered caller limits.
    const auto product=[&](std::initializer_list<std::size_t> factors) {
        const auto remaining=budget.max_mapped_profile_work-budget.consumed_mapped_profile_work;
        if(std::find(factors.begin(),factors.end(),0)!=factors.end())return;
        std::size_t work=1;
        for(const auto factor:factors) {
            require(factor<=remaining/work,"assembly mapped profile analytical work budget exceeded");
            work*=factor;
        }
        budget.consumed_mapped_profile_work+=work;
    };
    product({24,topology,topology});
    product({900000,arcs,segments});
}
void reserve_mapped_profiles(const std::vector<AssemblyProfile>& profiles,AssemblyExpansionBudget& budget) {
    validate_mapped_work_budget(budget);
    require(profiles.size()<=4096,"assembly profile collection budget exceeded");
    for(const auto& profile:profiles)if(profile.planar_affine) {
        const auto segments=profile_segments(profile);
        std::size_t arcs=0;
        const auto count=[&](const Boundary& boundary) {
            for(const auto& edge:boundary)if(edge.sweep_radians!=0)++arcs;
        };
        count(profile.outer);for(const auto& hole:profile.holes)count(hole);
        reserve_mapped_profile_work(budget,segments,profile.holes.size(),arcs);
    }
}
void reserve_instance_mapped_profiles(const AssemblyInstance& instance,AssemblyExpansionBudget& budget) {
    require(instance.nested_overrides.size()<=4096,"assembly override budget exceeded");
    if(instance.profile_overrides)reserve_mapped_profiles(*instance.profile_overrides,budget);
    for(const auto& path:instance.nested_overrides)if(path.profile_overrides)
        reserve_mapped_profiles(*path.profile_overrides,budget);
}
void reserve_raw_mapped_profiles(const nlohmann::json& profiles,AssemblyExpansionBudget& budget) {
    // Full bounded structural admission precedes any boundary allocation or
    // analytical validator. This helper never decodes profile geometry.
    (void)preflight_assembly_profiles(profiles);
    for(const auto& profile:profiles)if(profile.contains("planar_affine")) {
        std::size_t segments=0,arcs=0;
        const auto count=[&](const auto& boundary) {
            segments+=boundary.size();
            for(const auto& edge:boundary)if(edge.at("sweep_radians")!=0)++arcs;
        };
        count(profile.at("outer"));for(const auto& hole:profile.at("holes"))count(hole);
        reserve_mapped_profile_work(budget,segments,profile.at("holes").size(),arcs);
    }
}
// The entrywise L1 norm of M^-1 bounds 1/sigma_min. Round outward so validating
// source geometry at this tolerance conservatively protects effective physical
// features. Extreme maps without a representable outward bound fail closed;
// the pure affine helper has a wider mathematical domain than profile admission.
double mapped_source_tolerance(const AssemblyProfilePlanarAffine& affine,double scale=1) {
    validate_assembly_profile_affine(affine);
    const auto determinant=std::nextafter(assembly_profile_affine_determinant(affine),0.0);
    require(determinant>0,"mapped assembly profile tolerance bound is unrepresentable");
    const auto upper_inverse=[&](double coefficient) {
        if(coefficient==0)return 0.0;
        const auto result=std::nextafter(std::abs(coefficient)/determinant,
            std::numeric_limits<double>::infinity());
        require(std::isfinite(result) && result>0,"mapped assembly profile inverse tolerance bound is unrepresentable");
        return result;
    };
    const auto xx=upper_inverse(affine.xx),xy=upper_inverse(affine.xy),
        yx=upper_inverse(affine.yx),yy=upper_inverse(affine.yy);
    const auto largest=std::max({xx,xy,yx,yy});
    const auto infinity=std::numeric_limits<double>::infinity();
    double norm=0;
    for(const auto coefficient:{xx,xy,yx,yy})if(coefficient>0)
        norm=std::nextafter(norm+std::nextafter(coefficient/largest,infinity),infinity);
    require(std::isfinite(scale) && scale>0,"invalid mapped assembly profile physical scale");
    const auto physical_tolerance=std::nextafter(default_geometry_tolerance_metres/scale,infinity);
    require(std::isfinite(physical_tolerance) && physical_tolerance>0,
        "mapped assembly profile physical tolerance is unrepresentable");
    // Cover the two normalized-product roundings as well as the final grid
    // rounding. This guard does not change the pure affine helper's admission.
    norm=std::nextafter(norm*(1+8*std::numeric_limits<double>::epsilon()),infinity);
    const auto tolerance=std::nextafter(positive_product({physical_tolerance,largest,norm}),infinity);
    require(std::isfinite(tolerance),"mapped assembly profile source tolerance is unrepresentable");
    return std::max(default_geometry_tolerance_metres,tolerance);
}
double profile_source_area(const AssemblyProfile& profile) {
    auto area=std::abs(signed_area(profile.outer));
    for(const auto& hole:profile.holes)area-=std::abs(signed_area(hole));
    require(std::isfinite(area) && area>0,"assembly profile area overflow/underflow");
    return area;
}
void validate_mapped_profile(const AssemblyProfile& profile,double scale=1) {
    if(!profile.planar_affine)return;
    const auto& affine=*profile.planar_affine;
    const auto tolerance=mapped_source_tolerance(affine,scale);
    require(!validate_boundary_holes(profile.outer,profile.holes,tolerance).has_value(),
        "mapped assembly profile features fail conservative physical tolerance admission");
    const auto ring=[&](const Boundary& boundary) {
        // This additionally refuses historical tolerance-gapped connectivity.
        (void)assembly_profile_closed_boundary_signed_area(boundary,affine,tolerance);
        const auto bounds=assembly_profile_boundary_bounds(boundary,affine);
        const auto physical_tolerance=default_geometry_tolerance_metres/scale;
        const auto width=bounds.maximum.x-bounds.minimum.x,height=bounds.maximum.y-bounds.minimum.y;
        require(std::isfinite(width) && std::isfinite(height) && width>physical_tolerance && height>physical_tolerance,
            "mapped assembly profile bounds collapse below physical tolerance");
        for(const auto& segment:boundary) {
            const auto start=assembly_profile_affine_point(segment.start,affine);
            const auto end=assembly_profile_affine_point(segment.end,affine);
            const auto chord=std::hypot(end.x-start.x,end.y-start.y);
            require(std::isfinite(chord) && chord>physical_tolerance,
                "mapped assembly profile endpoint chord collapses below physical tolerance");
        }
    };
    ring(profile.outer);for(const auto& hole:profile.holes)ring(hole);
    const double factors[]{scale,scale};
    require(assembly_profile_affine_scaled_area(profile_source_area(profile),affine,factors)>
        default_geometry_tolerance_metres*default_geometry_tolerance_metres,
        "mapped assembly profile area collapses below physical tolerance");
}
double profile_volume(const AssemblyProfile& profile) {
    (void)profile_segments(profile);
    require(std::isfinite(profile.elevation_m) && std::isfinite(profile.height_m) && profile.height_m>0 &&
        std::isfinite(profile.elevation_m+profile.height_m), "invalid assembly profile extrusion");
    const auto area=profile_source_area(profile);
    if(profile.planar_affine) {
        require(profile.height_m>default_geometry_tolerance_metres,
            "mapped assembly profile extrusion collapses below physical tolerance");
        const double factors[]{profile.height_m};
        return assembly_profile_affine_scaled_area(area,*profile.planar_affine,factors);
    }
    const auto volume=area*profile.height_m;
    require(std::isfinite(area) && area>0 && std::isfinite(volume) && volume>0, "assembly profile volume overflow/underflow");
    return volume;
}
std::size_t profile_collection_segments(const std::vector<AssemblyProfile>& profiles) {
    require(profiles.size()<=4096,"assembly profile collection budget exceeded");
    std::size_t segments=0,topology_bytes=0;
    for(const auto& profile:profiles) {
        const auto count=profile_segments(profile);
        require(count<=262144-segments,"assembly profile collection work budget exceeded");segments+=count;
        add_topology_bytes(topology_bytes,profile_topology_bytes(profile),collection_topology_byte_limit);
    }
    return segments;
}
std::size_t profile_collection_topology_bytes(const std::vector<AssemblyProfile>& profiles) {
    std::size_t result=0;
    for(const auto& profile:profiles)add_topology_bytes(result,profile_topology_bytes(profile),collection_topology_byte_limit);
    return result;
}
std::size_t instance_profile_topology_bytes(const AssemblyInstance& instance) {
    std::size_t result=0;
    const auto add=[&](const auto& profiles) {
        if(profiles)add_topology_bytes(result,profile_collection_topology_bytes(*profiles),collection_topology_byte_limit);
    };
    add(instance.profile_overrides);
    for(const auto& path:instance.nested_overrides)add(path.profile_overrides);
    return result;
}
std::size_t instance_profile_segments(const AssemblyInstance& instance) {
    require(instance.nested_overrides.size()<=4096,"assembly override budget exceeded");
    std::size_t segments=0,topology_bytes=0;
    const auto add=[&](const auto& profiles) {
        if(!profiles)return;
        const auto count=profile_collection_segments(*profiles);
        require(count<=262144-segments,"assembly override profile work budget exceeded");segments+=count;
        add_topology_bytes(topology_bytes,profile_collection_topology_bytes(*profiles),collection_topology_byte_limit);
    };
    add(instance.profile_overrides);
    for(const auto& path:instance.nested_overrides)add(path.profile_overrides);
    return segments;
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
AssemblyProfilePlanarAffine decode_profile_affine(const nlohmann::json& value) {
    fields(value,{"version","source_origin_m","target_origin_m","matrix"});
    require(value.at("version").is_number_integer() && value.at("version")==1,
        "unsupported assembly profile affine version");
    for(const auto* key:{"source_origin_m","target_origin_m","matrix"}) {
        const auto& numbers=value.at(key);
        require(numbers.is_array() && numbers.size()==(std::string_view(key)=="matrix" ? 4u:2u),
            "invalid assembly profile affine coefficient count");
        for(const auto& number:numbers)require(number.is_number() && std::isfinite(number.get<double>()),
            "assembly profile affine coefficients must be finite numbers");
    }
    const auto& source=value.at("source_origin_m");const auto& target=value.at("target_origin_m");
    const auto& matrix=value.at("matrix");
    AssemblyProfilePlanarAffine result{{source[0].get<double>(),source[1].get<double>()},
        {target[0].get<double>(),target[1].get<double>()},matrix[0].get<double>(),matrix[1].get<double>(),
        matrix[2].get<double>(),matrix[3].get<double>()};
    validate_assembly_profile_affine(result);return result;
}
void profile_fields(const nlohmann::json& value,bool identified=true,bool mapped=true) {
    if(value.is_object() && (value.contains("profile_version") || value.contains("planar_affine"))) {
        require(mapped,"mapped assembly profiles require a supporting schema version");
        fields(value,{"profile_version","id","outer","holes","elevation_m","height_m","material_slot","topology","planar_affine"});
        require(value.at("profile_version").is_number_integer() && value.at("profile_version")==3,
            "unsupported mapped assembly profile version");
        (void)decode_profile_affine(value.at("planar_affine"));
    } else if(value.is_object() && (value.contains("version") || value.contains("topology"))) {
        require(identified,"identified assembly profiles require a supporting schema version");
        fields(value,{"version","id","outer","holes","elevation_m","height_m","material_slot","topology"});
        require(value.at("version").is_number_integer() && value.at("version")==2,
            "unsupported assembly profile version");
    } else fields(value,{"id","outer","holes","elevation_m","height_m","material_slot"});
}
std::size_t preflight_profile_topology(const nlohmann::json& profile) {
    const bool mapped=profile.contains("planar_affine");
    if(!profile.contains("topology") || (profile.contains("profile_version") && profile.at("topology").is_null()))
        return mapped ? mapped_profile_wire_bytes:0;
    const auto& topology=profile.at("topology");
    fields(topology,{"version","outer","holes"});
    require(topology.at("version").is_number_integer() && topology.at("version")==1,
        "unsupported assembly profile topology version");
    require(topology.at("holes").is_array() && topology.at("holes").size()==profile.at("holes").size(),
        "assembly profile topology hole count mismatch");
    ProfileTopologyAdmission admission;
    if(mapped)admission.bytes+=mapped_profile_wire_bytes;
    const auto ring=[&](const nlohmann::json& identity,const nlohmann::json& boundary) {
        fields(identity,{"ring_id","segment_ids","vertex_ids"});
        require(identity.at("segment_ids").is_array() && identity.at("vertex_ids").is_array() &&
            identity.at("segment_ids").size()==boundary.size() && identity.at("vertex_ids").size()==boundary.size(),
            "assembly profile topology segment/vertex count mismatch");
        admission.ring(identity.at("ring_id").get_ref<const std::string&>());
        for(const auto& id:identity.at("segment_ids"))admission.add(id.get_ref<const std::string&>(),admission.segments);
        for(const auto& id:identity.at("vertex_ids"))admission.add(id.get_ref<const std::string&>(),admission.vertices);
    };
    ring(topology.at("outer"),profile.at("outer"));
    for(std::size_t i=0;i<topology.at("holes").size();++i)ring(topology.at("holes")[i],profile.at("holes")[i]);
    return admission.bytes;
}
bool profiles_are_mapped(const std::vector<AssemblyProfile>& profiles) {
    return std::any_of(profiles.begin(),profiles.end(),[](const auto& profile){return profile.planar_affine.has_value();});
}
bool instance_is_mapped(const AssemblyInstance& instance) {
    return (instance.profile_overrides && profiles_are_mapped(*instance.profile_overrides)) ||
        std::any_of(instance.nested_overrides.begin(),instance.nested_overrides.end(),[](const auto& path){
            return path.profile_overrides && profiles_are_mapped(*path.profile_overrides);
        });
}
// Examine all raw override collections before any boundary/path allocation.
// Standalone profile arrays own their dialect; enclosing codecs restrict it.
void preflight_profile_dialect(const nlohmann::json& profiles,bool identified,bool mapped) {
    require(profiles.is_array() && profiles.size()<=4096,"invalid assembly profile collection");
    for(const auto& profile:profiles)profile_fields(profile,identified,mapped);
}
void preflight_instance_profile_dialect(const nlohmann::json& instance,bool geometry,bool mapped) {
    const auto inspect=[&](const nlohmann::json& row) {
        if(!row.contains("profile_overrides"))return;
        require(geometry,"assembly profile overrides require a supporting schema version");
        preflight_profile_dialect(row.at("profile_overrides"),geometry,mapped);
    };
    inspect(instance);
    if(instance.contains("nested_overrides")) {
        const auto& paths=instance.at("nested_overrides");
        require(paths.is_array() && paths.size()<=4096,"invalid assembly override collection");
        for(const auto& path:paths)inspect(path);
    }
}
std::size_t raw_collection_topology_bytes(const nlohmann::json& profiles) {
    std::size_t result=0;
    for(const auto& profile:profiles)add_topology_bytes(result,preflight_profile_topology(profile),collection_topology_byte_limit);
    return result;
}
void fields_with_profiles(const nlohmann::json& value, std::initializer_list<const char*> keys,
    bool geometry) {
    const bool present=value.is_object() && value.contains("profile_overrides");
    require(!present || geometry,"assembly profile overrides require a supporting schema version");
    require(value.is_object() && value.size()==keys.size()+(present ? 1u:0u),"invalid assembly JSON fields");
    for(const auto* key:keys)require(value.contains(key),"missing assembly JSON field");
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
    // Bound the entire authored override payload before expensive analytical
    // validation. Many independent paths must not multiply the collection cap.
    (void)instance_profile_segments(instance);
    const auto validate_profiles=[&](const auto& source,const AssemblyType& type) {
        if(!source.profile_overrides)return;
        (void)validate_assembly_profiles(*source.profile_overrides);
        for(const auto& profile:*source.profile_overrides)if(profile.material_slot)
            require(type.materials.contains(*profile.material_slot),"assembly profile references unknown material slot");
    };
    validate_profiles(instance,find(types,instance.type_id));
    std::set<std::vector<std::string>> paths;
    for(const auto& change:instance.nested_overrides) {
        require(paths.insert(change.part_path).second,"duplicate assembly override path");
        const auto& type=path_type(types,instance.type_id,change.part_path);
        validate_overrides(type,change,materials);
        validate_profiles(change,type);
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
nlohmann::json encode_profile(const AssemblyProfile& profile) {
    auto holes=nlohmann::json::array();for(const auto& hole:profile.holes)holes.push_back(encode_boundary(hole));
    nlohmann::json result{{"id",profile.id},{"outer",encode_boundary(profile.outer)},
        {"holes",std::move(holes)},{"elevation_m",profile.elevation_m},{"height_m",profile.height_m},
        {"material_slot",profile.material_slot ? nlohmann::json(*profile.material_slot):nlohmann::json(nullptr)}};
    if(profile.topology) {
        const auto ring=[](const AssemblyProfileRingIdentity& identity) -> nlohmann::json {
            return {{"ring_id",identity.ring_id},{"segment_ids",identity.segment_ids},{"vertex_ids",identity.vertex_ids}};
        };
        auto identified_holes=nlohmann::json::array();
        for(const auto& hole:profile.topology->holes)identified_holes.push_back(ring(hole));
        result["version"]=2;
        result["topology"]={{"version",1},{"outer",ring(profile.topology->outer)},{"holes",std::move(identified_holes)}};
    }
    if(profile.planar_affine) {
        const auto& affine=*profile.planar_affine;
        result.erase("version");result["profile_version"]=3;
        if(!profile.topology)result["topology"]=nullptr;
        result["planar_affine"]={{"version",1},
            {"source_origin_m",{affine.source_origin.x,affine.source_origin.y}},
            {"target_origin_m",{affine.target_origin.x,affine.target_origin.y}},
            {"matrix",{affine.xx,affine.xy,affine.yx,affine.yy}}};
    }
    return result;
}
AssemblyProfile decode_profile(const nlohmann::json& encoded) {
    AssemblyProfile profile;profile.id=encoded.at("id").get<std::string>();profile.outer=decode_boundary(encoded.at("outer"));
    auto segment_count=profile.outer.size();
    for(const auto& hole:encoded.at("holes")) {
        require(hole.is_array() && hole.size()<=1024-segment_count,"assembly profile segment budget exceeded");
        segment_count+=hole.size();profile.holes.push_back(decode_boundary(hole));
    }
    profile.elevation_m=encoded.at("elevation_m").get<double>();profile.height_m=encoded.at("height_m").get<double>();
    if(!encoded.at("material_slot").is_null())profile.material_slot=encoded.at("material_slot").get<std::string>();
    if(encoded.contains("topology") && !encoded.at("topology").is_null()) {
        const auto ring=[](const nlohmann::json& identity) {
            return AssemblyProfileRingIdentity{identity.at("ring_id").get<std::string>(),
                identity.at("segment_ids").get<std::vector<std::string>>(),identity.at("vertex_ids").get<std::vector<std::string>>()};
        };
        const auto& topology=encoded.at("topology");
        profile.topology=AssemblyProfileTopology{ring(topology.at("outer")),{}};
        for(const auto& hole:topology.at("holes"))profile.topology->holes.push_back(ring(hole));
    }
    if(encoded.contains("planar_affine"))profile.planar_affine=decode_profile_affine(encoded.at("planar_affine"));
    return profile;
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
    require(paths.size()<=4096,"assembly override budget exceeded");
    auto result=nlohmann::json::array();
    auto ordered=paths;
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.part_path<b.part_path;});
    for(const auto& path:ordered) {
        auto value=encode_overrides(path);value["part_path"]=path.part_path;
        value["transform"]=path.transform ? encode_assembly_transform(*path.transform):nlohmann::json(nullptr);
        if(path.profile_overrides)value["profile_overrides"]=encode_assembly_profiles(*path.profile_overrides);
        result.push_back(std::move(value));
    }
    return result;
}
AssemblyTransform decode_versioned_transform(const nlohmann::json& value,bool vertical) {
    require(vertical || !value.contains("vertical_scale"),"assembly vertical scale requires a supporting schema version");
    return decode_assembly_transform(value);
}
std::vector<AssemblyPathOverride> decode_paths(const nlohmann::json& paths,bool vertical=true,bool geometry=false,
    std::size_t initial_segments=0,std::size_t initial_topology_bytes=0) {
    require(paths.is_array() && paths.size()<=4096,"invalid assembly override collection");
    auto segments=initial_segments;
    auto topology_bytes=initial_topology_bytes;
    require(segments<=262144,"assembly override profile work budget exceeded");
    for(const auto& value:paths)if(value.contains("profile_overrides")) {
        require(geometry,"assembly profile overrides require a supporting schema version");
        const auto count=preflight_assembly_profiles(value.at("profile_overrides"));
        require(count<=262144-segments,"assembly override profile work budget exceeded");segments+=count;
        add_topology_bytes(topology_bytes,raw_collection_topology_bytes(value.at("profile_overrides")),collection_topology_byte_limit);
    }
    std::vector<AssemblyPathOverride> result;
    std::set<std::vector<std::string>> identities;
    for(const auto& value:paths) {
        fields_with_profiles(value,{"part_path","transform","property_overrides","material_overrides","quantity_overrides"},geometry);
        require(value.at("part_path").is_array() && !value.at("part_path").empty() && value.at("part_path").size()<32,"invalid assembly override path");
        AssemblyPathOverride path;path.part_path=value.at("part_path").get<std::vector<std::string>>();
        for(const auto& id:path.part_path)identifier(id);
        require(identities.insert(path.part_path).second,"duplicate assembly override path");
        if(!value.at("transform").is_null())path.transform=decode_versioned_transform(value.at("transform"),vertical);
        decode_overrides(value,path);
        if(value.contains("profile_overrides"))path.profile_overrides=decode_assembly_profiles(value.at("profile_overrides"));
        result.push_back(std::move(path));
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.part_path<b.part_path;});
    return result;
}
} // namespace

std::size_t preflight_assembly_profiles(const nlohmann::json& value) {
    try {
        require(value.is_array() && value.size()<=4096,"invalid assembly profile collection");
        std::size_t segments=0,topology_bytes=0;
        AssemblyExpansionBudget work;
        std::set<std::string_view> ids;
        for(const auto& encoded:value) {
            profile_fields(encoded);
            const auto& id=encoded.at("id").get_ref<const std::string&>();identifier(id);
            require(ids.insert(id).second,"duplicate assembly profile identifier");
            if(!encoded.at("material_slot").is_null())identifier(encoded.at("material_slot").get_ref<const std::string&>());
            require(encoded.at("elevation_m").is_number() && encoded.at("height_m").is_number(),"invalid assembly profile extrusion");
            const auto elevation=encoded.at("elevation_m").get<double>(),height=encoded.at("height_m").get<double>();
            require(std::isfinite(elevation) && std::isfinite(height) && height>0 && std::isfinite(elevation+height),
                "invalid assembly profile extrusion");
            require(encoded.at("holes").is_array() && encoded.at("holes").size()<=1024,"invalid assembly profile holes");
            std::size_t profile_count=0,arcs=0;
            const auto ring=[&](const nlohmann::json& boundary) {
                require(boundary.is_array() && !boundary.empty() && boundary.size()<=1024-profile_count,
                    "assembly profile segment budget exceeded");
                profile_count+=boundary.size();
                for(const auto& edge:boundary) {
                    fields(edge,{"start","end","sweep_radians"});
                    for(const auto* key:{"start","end"}) {
                        const auto& point=edge.at(key);
                        require(point.is_array() && point.size()==2 && point[0].is_number() && point[1].is_number(),
                            "invalid assembly boundary point");
                        require(std::isfinite(point[0].get<double>()) && std::isfinite(point[1].get<double>()),
                            "assembly boundary point must be finite");
                    }
                    require(edge.at("sweep_radians").is_number() && std::isfinite(edge.at("sweep_radians").get<double>()),
                        "invalid assembly arc sweep");
                    if(edge.at("sweep_radians")!=0)++arcs;
                }
            };
            ring(encoded.at("outer"));for(const auto& hole:encoded.at("holes"))ring(hole);
            require(profile_count<=262144-segments,"assembly profile collection work budget exceeded");segments+=profile_count;
            add_topology_bytes(topology_bytes,preflight_profile_topology(encoded),collection_topology_byte_limit);
            if(encoded.contains("planar_affine"))
                reserve_mapped_profile_work(work,profile_count,encoded.at("holes").size(),arcs);
        }
        return segments;
    } catch(const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly profiles JSON: ")+error.what());
    }
}
void validate_assembly_profile_topology(const AssemblyProfile& profile) {
    (void)profile_topology_bytes(profile);
}
std::size_t validate_assembly_profiles(const std::vector<AssemblyProfile>& profiles) {
    require(profiles.size()<=4096,"assembly profile collection budget exceeded");
    AssemblyExpansionBudget work;
    reserve_mapped_profiles(profiles,work);
    std::size_t segments=0,topology_bytes=0;
    std::set<std::string_view> ids;
    for(const auto& profile:profiles) {
        identifier(profile.id);require(ids.insert(profile.id).second,"duplicate assembly profile identifier");
        const auto count=profile_segments(profile);
        require(count<=262144-segments,"assembly profile collection work budget exceeded");segments+=count;
        add_topology_bytes(topology_bytes,profile_topology_bytes(profile),collection_topology_byte_limit);
    }
    for(const auto& profile:profiles) {
        require(!validate_boundary_holes(profile.outer,profile.holes).has_value(),"invalid assembly profile boundary/holes");
        validate_mapped_profile(profile);
        (void)profile_volume(profile);
        if(profile.material_slot)identifier(*profile.material_slot);
    }
    return segments;
}
nlohmann::json encode_assembly_profiles(const std::vector<AssemblyProfile>& profiles) {
    (void)validate_assembly_profiles(profiles);
    std::vector<const AssemblyProfile*> ordered;ordered.reserve(profiles.size());
    for(const auto& profile:profiles)ordered.push_back(&profile);
    std::sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->id<b->id;});
    auto result=nlohmann::json::array();
    for(const auto* profile:ordered)result.push_back(encode_profile(*profile));
    return result;
}
std::vector<AssemblyProfile> decode_assembly_profiles(const nlohmann::json& value) {
    (void)preflight_assembly_profiles(value);
    std::vector<AssemblyProfile> result;result.reserve(value.size());
    for(const auto& encoded:value)result.push_back(decode_profile(encoded));
    (void)validate_assembly_profiles(result);
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    return result;
}

bool AssemblyProfile::operator==(const AssemblyProfile& other) const {
    if(id!=other.id || elevation_m!=other.elevation_m || height_m!=other.height_m ||
        material_slot!=other.material_slot || topology!=other.topology || planar_affine!=other.planar_affine ||
        !boundary_equal(outer,other.outer) || holes.size()!=other.holes.size())return false;
    for(std::size_t i=0;i<holes.size();++i)if(!boundary_equal(holes[i],other.holes[i]))return false;
    return true;
}
AssemblyRotationComponents assembly_rotation_components(double radians) {
    require(std::isfinite(radians),"assembly rotation must be finite");
    const auto reduced=std::remainder(radians,2.0*std::numbers::pi);
    // Equality is intentional: nearby authored angles must not be snapped.
    if(reduced==0)return {1,0};
    if(reduced==std::numbers::pi || reduced==-std::numbers::pi)return {-1,0};
    if(reduced==std::numbers::pi*0.5)return {0,1};
    if(reduced==-std::numbers::pi*0.5)return {0,-1};
    return {std::cos(radians),std::sin(radians)};
}
AssemblyPoint3 transform_assembly_point(AssemblyPoint3 point,const AssemblyTransform& value) {
    transform(value);
    require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),"assembly point must be finite");
    const auto x=point.x*value.scale,y=(value.mirrored_y ? -point.y : point.y)*value.scale,
        z=point.z*z_scale(value.scale,value.vertical_scale);
    require(std::isfinite(x) && std::isfinite(y) && std::isfinite(z),"assembly point scale overflow");
    const auto [c,s]=assembly_rotation_components(value.rotation_radians);
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
AssemblyTransform conjugate_assembly_transform_through_rigid_frame(
    const AssemblyTransform& operation,const AssemblyTransform& frame) {
    transform(operation);transform(frame);
    require(frame.scale==1 && frame.vertical_scale==1 && !frame.mirrored_y,
        "assembly conjugation frame must be orientation-preserving and rigid");
    if(operation==AssemblyTransform{} || (frame.translation_m==AssemblyPoint3{} &&
        frame.rotation_radians==0))return operation;
    auto result=operation;
    // R_phi * R_theta * D * R_-phi keeps theta exactly without reflection;
    // reflected yaw is theta+2*phi. Avoid adding/cancelling a frame angle.
    if(operation.mirrored_y)result.rotation_radians=std::remainder(std::fma(2.0,
        std::remainder(frame.rotation_radians,2.0*std::numbers::pi),operation.rotation_radians),
        2.0*std::numbers::pi);
    const auto rotated=transform_assembly_point(operation.translation_m,
        AssemblyTransform{{},frame.rotation_radians,1.0,false});
    const auto [c,s]=assembly_rotation_components(result.rotation_radians);
    const auto parity=result.mirrored_y ? -1.0 : 1.0;
    // R_phi*t + (I-L_world)*origin avoids subtracting and re-adding origin.
    const auto x_coefficient=(1.0-result.scale)+result.scale*(1.0-c);
    const auto y_coefficient=parity==1.0 ? x_coefficient :
        (1.0+result.scale)-result.scale*(1.0-c);
    result.translation_m={
        rotated.x+x_coefficient*frame.translation_m.x+result.scale*s*parity*frame.translation_m.y,
        rotated.y-result.scale*s*frame.translation_m.x+y_coefficient*frame.translation_m.y,
        rotated.z+(1.0-z_scale(result.scale,result.vertical_scale))*frame.translation_m.z};
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
    (void)instance_profile_segments(instance);
    AssemblyExpansionBudget work;
    reserve_instance_mapped_profiles(instance,work);
    auto result=encode_overrides(instance);
    const bool vertical=instance.root_transform->vertical_scale!=1 ||
        std::any_of(instance.nested_overrides.begin(),instance.nested_overrides.end(),
            [](const auto& path){return path.transform && path.transform->vertical_scale!=1;});
    const bool geometry=instance.profile_overrides.has_value() ||
        std::any_of(instance.nested_overrides.begin(),instance.nested_overrides.end(),
            [](const auto& path){return path.profile_overrides.has_value();});
    result["schema"]=instance_is_mapped(instance) ? "sketch.assembly-instance.v4":geometry ? "sketch.assembly-instance.v3":
        vertical ? "sketch.assembly-instance.v2":"sketch.assembly-instance.v1";
    result["id"]=instance.id;result["type_id"]=instance.type_id;
    result["root_transform"]=encode_assembly_transform(*instance.root_transform);
    result["nested_overrides"]=encode_paths(instance.nested_overrides);
    if(instance.profile_overrides)result["profile_overrides"]=encode_assembly_profiles(*instance.profile_overrides);
    // Establish the same lexical/path/finite validation on both codec directions.
    (void)decode_assembly_instance(result);
    return result;
}
AssemblyInstance decode_assembly_instance(const nlohmann::json& value) {
    AssemblyExpansionBudget budget;
    return decode_assembly_instance(value,budget);
}
AssemblyInstance decode_assembly_instance(const nlohmann::json& value,AssemblyExpansionBudget& budget) {
    try {
        validate_mapped_work_budget(budget);
        auto pending=budget;
        const bool mapped=value.at("schema")=="sketch.assembly-instance.v4";
        const bool geometry=mapped || value.at("schema")=="sketch.assembly-instance.v3";
        fields_with_profiles(value,{"schema","id","type_id","property_overrides","material_overrides","quantity_overrides","root_transform","nested_overrides"},geometry);
        const bool vertical=geometry || value.at("schema")=="sketch.assembly-instance.v2";
        require(vertical || value.at("schema")=="sketch.assembly-instance.v1","unsupported independent assembly instance schema");
        preflight_instance_profile_dialect(value,geometry,mapped);
        // Reserve every collection before decode_paths() validates the first
        // one. A root's many nested replacements share the caller's allowance.
        std::size_t admitted_segments=0,admitted_topology_bytes=0;
        const auto admit=[&](const auto& row) {
            if(!row.contains("profile_overrides"))return;
            const auto& profiles=row.at("profile_overrides");
            const auto count=preflight_assembly_profiles(profiles);
            require(count<=262144-admitted_segments,"assembly override profile work budget exceeded");
            admitted_segments+=count;
            add_topology_bytes(admitted_topology_bytes,raw_collection_topology_bytes(profiles),collection_topology_byte_limit);
            reserve_raw_mapped_profiles(profiles,pending);
        };
        admit(value);
        for(const auto& path:value.at("nested_overrides"))admit(path);
        AssemblyInstance result;result.id=value.at("id").get<std::string>();result.type_id=value.at("type_id").get<std::string>();
        identifier(result.id);identifier(result.type_id);decode_overrides(value,result);
        result.root_transform=decode_versioned_transform(value.at("root_transform"),vertical);
        const auto segments=value.contains("profile_overrides") ? preflight_assembly_profiles(value.at("profile_overrides")):0;
        const auto topology_bytes=value.contains("profile_overrides") ? raw_collection_topology_bytes(value.at("profile_overrides")):0;
        result.nested_overrides=decode_paths(value.at("nested_overrides"),vertical,geometry,segments,topology_bytes);
        if(value.contains("profile_overrides"))result.profile_overrides=decode_assembly_profiles(value.at("profile_overrides"));
        budget=std::move(pending);
        return result;
    } catch(const nlohmann::json::exception& error) {throw std::invalid_argument(std::string("invalid assembly instance JSON: ")+error.what());}
}

AssemblyModel AssemblyModel::create(std::vector<AssemblyMaterial> materials,
    std::vector<AssemblyType> types, std::vector<AssemblyInstance> instances) {
    require(types.size()<=4096 && instances.size()<=4096,"assembly catalog collection budget exceeded");
    std::size_t type_segments=0,override_segments=0,type_topology_bytes=0,override_topology_bytes=0;
    AssemblyExpansionBudget validation_work;
    for(const auto& type:types) {
        const auto count=profile_collection_segments(type.profiles);
        require(count<=1048576-type_segments,"assembly catalog validation profile work budget exceeded");type_segments+=count;
        add_topology_bytes(type_topology_bytes,profile_collection_topology_bytes(type.profiles),catalog_topology_byte_limit);
        reserve_mapped_profiles(type.profiles,validation_work);
    }
    for(const auto& instance:instances) {
        const auto count=instance_profile_segments(instance);
        require(count<=262144-override_segments,"assembly document profile segment budget exceeded");override_segments+=count;
        add_topology_bytes(override_topology_bytes,instance_profile_topology_bytes(instance),collection_topology_byte_limit);
        reserve_instance_mapped_profiles(instance,validation_work);
    }
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
        const auto order_profiles=[](auto& profiles) {
            if(profiles)std::sort(profiles->begin(),profiles->end(),[](const auto& a,const auto& b){return a.id<b.id;});
        };
        order_profiles(instance.profile_overrides);
        for(auto& path:instance.nested_overrides)order_profiles(path.profile_overrides);
    }
    AssemblyModel model;
    model.materials_ = std::move(materials); model.types_ = std::move(types); model.instances_ = std::move(instances);
    // A catalog must also refuse unused transformed graphs with overflowing
    // coordinates, volumes, quantities or profile expansion segment budgets.
    for(const auto& type:model.types_) {
        AssemblyInstance probe;probe.id="validation";probe.type_id=type.id;
        AssemblyExpansionBudget budget;
        budget.consumed_mapped_profile_work=validation_work.consumed_mapped_profile_work;
        (void)model.expand(probe,budget);
        validation_work.consumed_mapped_profile_work=budget.consumed_mapped_profile_work;
    }
    AssemblyExpansionBudget document_budget;
    document_budget.consumed_mapped_profile_work=validation_work.consumed_mapped_profile_work;
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
    validate_mapped_work_budget(budget);
    require(std::isfinite(budget.volume_m3) && budget.volume_m3>=0,"invalid assembly aggregate volume");
    for(const auto& [key,value]:budget.declared_quantities) {
        identifier(key.name);(void)unit_name(key.unit);
        require(std::isfinite(value) && value>=0,"invalid assembly aggregate quantity");
    }
    for(const auto& [id,value]:budget.material_volumes_m3) {
        identifier(id);require(std::isfinite(value) && value>=0,"invalid assembly aggregate material volume");
    }
    auto pending=budget;
    (void)instance_profile_segments(instance);
    reserve_instance_mapped_profiles(instance,pending);
    std::map<std::vector<std::string>,const AssemblyPathOverride*> changes;
    for(const auto& change:instance.nested_overrides) {
        // Validate borrowed path depth/identities/references before any key
        // copy. Typed callers need the same structural fence as JSON callers.
        (void)path_type(types_,instance.type_id,change.part_path);
        require(changes.emplace(change.part_path,&change).second,"duplicate assembly override path");
    }
    // Walk bounded structure without copying profiles or doing geometry work.
    // Reserve every actual selected collection before even the first override
    // validator runs; shared DAG visits and path replacements remain distinct.
    auto admission=pending;
    std::function<void(const AssemblyType&,std::vector<std::string>)> admit;
    admit=[&](const AssemblyType& type,std::vector<std::string> path) {
        require(path.size()<32,"assembly graph depth budget exceeded");
        require(admission.consumed_nodes<admission.max_nodes,"assembly document expansion node budget exceeded");
        ++admission.consumed_nodes;
        const auto* profiles=&type.profiles;
        if(path.empty() && instance.profile_overrides)profiles=&*instance.profile_overrides;
        if(const auto found=changes.find(path);found!=changes.end() && found->second->profile_overrides)
            profiles=&*found->second->profile_overrides;
        for(const auto& profile:*profiles) {
            const auto count=profile_segments(profile);
            require(count<=admission.max_profile_segments-admission.consumed_profile_segments,
                "assembly document profile segment budget exceeded");
            admission.consumed_profile_segments+=count;
        }
        reserve_mapped_profiles(*profiles,admission);
        for(const auto& child:type.parts) {
            auto child_path=path;child_path.push_back(child.id);
            admit(find(types_,child.type_id),std::move(child_path));
        }
    };
    admit(find(types_,instance.type_id),{});
    pending.consumed_mapped_profile_work=admission.consumed_mapped_profile_work;
    validate_instance(instance,types_,materials_);
    AssemblyExpansion result;result.source_instance=instance;
    // Profile addresses distinguish selected-node overrides from shared type
    // profiles even when their stable profile IDs happen to be identical.
    std::map<const AssemblyProfile*,double> local_volumes;
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
        const auto* profiles=&type.profiles;
        if(part) {
            node.source_part=*part;overlay(node.properties,part->property_overrides);
            overlay(node.materials,part->material_overrides);overlay(node.quantities,part->quantity_overrides);
        } else {
            overlay(node.properties,instance.property_overrides);overlay(node.materials,instance.material_overrides);
            overlay(node.quantities,instance.quantity_overrides);
            if(instance.profile_overrides)profiles=&*instance.profile_overrides;
        }
        if(const auto found=changes.find(path);found!=changes.end()) {
            const auto& change=*found->second;node.path_override=change;
            overlay(node.properties,change.property_overrides);overlay(node.materials,change.material_overrides);
            overlay(node.quantities,change.quantity_overrides);
            if(change.profile_overrides)profiles=&*change.profile_overrides;
        }
        for(const auto& [key,value]:node.quantities)add_quantity(result.declared_quantities[{key,value.unit}],value.value);
        for(const auto& profile:*profiles) {
            const auto segments=profile_segments(profile);
            require(segments<=pending.max_profile_segments-pending.consumed_profile_segments,"assembly document profile segment budget exceeded");
            pending.consumed_profile_segments+=segments;
            // Analytical arc bounds also cover extrema not present at vertices.
            const auto validate_points=[&](const Boundary& boundary) {
                const auto bounds=profile.planar_affine ? assembly_profile_boundary_bounds(boundary,*profile.planar_affine):
                    boundary_bounds(boundary);
                for(const auto point:{bounds.minimum,bounds.maximum,
                    Vec2{bounds.minimum.x,bounds.maximum.y},Vec2{bounds.maximum.x,bounds.minimum.y}}) {
                    (void)transform_assembly_point({point.x,point.y,profile.elevation_m},world);
                    (void)transform_assembly_point({point.x,point.y,profile.elevation_m+profile.height_m},world);
                }
                if(profile.planar_affine)for(const auto& segment:boundary) {
                    const auto a=assembly_profile_affine_point(segment.start,*profile.planar_affine);
                    const auto b=assembly_profile_affine_point(segment.end,*profile.planar_affine);
                    const auto start=transform_assembly_point({a.x,a.y,profile.elevation_m},world);
                    const auto end=transform_assembly_point({b.x,b.y,profile.elevation_m},world);
                    const auto top=transform_assembly_point({a.x,a.y,profile.elevation_m+profile.height_m},world);
                    const auto chord=std::hypot(end.x-start.x,end.y-start.y);
                    require(std::isfinite(chord) && chord>default_geometry_tolerance_metres &&
                        top.z-start.z>default_geometry_tolerance_metres,
                        "mapped assembly profile world coordinates collapse below physical tolerance");
                }
            };
            validate_points(profile.outer);for(const auto& hole:profile.holes)validate_points(hole);
            validate_mapped_profile(profile,world.scale);
            const auto* key=&profile;
            auto found_volume=local_volumes.find(key);
            if(found_volume==local_volumes.end())found_volume=local_volumes.emplace(key,profile_volume(profile)).first;
            const double mapped_factors[]{profile.height_m,world.scale,world.scale,world.scale,world.vertical_scale};
            const auto volume=profile.planar_affine
                ? assembly_profile_affine_scaled_area(profile_source_area(profile),*profile.planar_affine,mapped_factors)
                : positive_product({found_volume->second,world.scale,world.scale,z_scale(world.scale,world.vertical_scale)});
            if(profile.planar_affine)require(positive_product({profile.height_m,world.scale,world.vertical_scale})>
                default_geometry_tolerance_metres,"expanded mapped assembly profile extrusion collapses below physical tolerance");
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
    bool geometry = false;
    bool mapped = false;
    for(const auto& type:types_)for(const auto& profile:type.profiles) {
        mapped=mapped || profile.planar_affine.has_value();
        geometry=geometry || profile.topology.has_value() || profile.planar_affine.has_value();
    }
    for(const auto& type:types_)nested=nested || !type.profiles.empty() || !type.parts.empty();
    for(const auto& type:types_)for(const auto& part:type.parts)vertical=vertical || part.transform.vertical_scale!=1;
    for(const auto& instance:instances_)nested=nested || instance.root_transform.has_value() || !instance.nested_overrides.empty();
    for (const auto& instance : instances_) {
        mapped=mapped || instance_is_mapped(instance);
        geometry=geometry || instance.profile_overrides.has_value();
        has_placement = has_placement || instance.placement.has_value();
        placement_xyz = placement_xyz || (instance.placement && instance.placement->translation_z_m!=0);
        vertical=vertical || (instance.placement && instance.placement->vertical_scale!=1) ||
            (instance.root_transform && instance.root_transform->vertical_scale!=1);
        for(const auto& path:instance.nested_overrides) {
            vertical=vertical || (path.transform && path.transform->vertical_scale!=1);
            geometry=geometry || path.profile_overrides.has_value();
        }
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
    if(geometry)result["schema"]="sketch.assemblies.v8";
    if(mapped)result["schema"]="sketch.assemblies.v9";
    for (const auto& type : types_) {
        nlohmann::json value{{"id",type.id},{"name",type.name},{"properties",type.properties},
            {"materials",type.materials},{"quantities",encode_quantities(type.quantities)}};
        if(geometry ? (!type.profiles.empty() || !type.parts.empty()):nested) {
            value["profiles"]=nlohmann::json::array();value["parts"]=nlohmann::json::array();
            for(const auto& profile:type.profiles)value["profiles"].push_back(encode_profile(profile));
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
        if(geometry ? (instance.root_transform.has_value() || !instance.nested_overrides.empty()):nested) {
            value["root_transform"]=instance.root_transform ? encode_assembly_transform(*instance.root_transform):nlohmann::json(nullptr);
            value["nested_overrides"]=encode_paths(instance.nested_overrides);
        }
        if(instance.profile_overrides)value["profile_overrides"]=encode_assembly_profiles(*instance.profile_overrides);
        result["instances"].push_back(std::move(value));
    }
    return result;
}
AssemblyModel AssemblyModel::from_json(const nlohmann::json& value) {
    try {
        fields(value, {"schema", "materials", "types", "instances"});
        const auto schema = value.at("schema").get<std::string>();
        // v7 is a closed union of retained row envelopes, not an extension of
        // any older dialect's required fields or placement dimensions.
        const bool mapped = schema == "sketch.assemblies.v9";
        const bool geometry = mapped || schema == "sketch.assemblies.v8";
        const bool row_local = schema == "sketch.assemblies.v7" || geometry;
        const bool vertical = schema == "sketch.assemblies.v6" || row_local;
        const bool placement_xyz = schema == "sketch.assemblies.v5" || vertical;
        const bool nested = schema == "sketch.assemblies.v4" || placement_xyz;
        const bool appearance = schema == "sketch.assemblies.v2" || schema == "sketch.assemblies.v3" || nested;
        const bool placements = schema == "sketch.assemblies.v3" || nested;
        require(appearance || schema == "sketch.assemblies.v1", "unsupported assembly schema");
        for (const auto* key : {"materials", "types", "instances"})
            require(value.at(key).is_array(), "assembly collections must be arrays");
        require(value.at("types").size()<=4096 && value.at("instances").size()<=4096,"assembly catalog collection budget exceeded");
        if(geometry) {
            // Admission precedes boundary allocation for the complete new
            // dialect, including payloads spread over many rows and paths.
            std::size_t type_segments=0,override_segments=0,type_topology_bytes=0,override_topology_bytes=0;
            AssemblyExpansionBudget decode_work;
            for(const auto& item:value.at("types"))if(item.contains("profiles")) {
                preflight_profile_dialect(item.at("profiles"),geometry,mapped);
                const auto count=preflight_assembly_profiles(item.at("profiles"));
                require(count<=1048576-type_segments,"assembly catalog validation profile work budget exceeded");
                type_segments+=count;
                add_topology_bytes(type_topology_bytes,raw_collection_topology_bytes(item.at("profiles")),catalog_topology_byte_limit);
                reserve_raw_mapped_profiles(item.at("profiles"),decode_work);
            }
            const auto preflight_override=[&](const nlohmann::json& item) {
                if(!item.contains("profile_overrides"))return;
                preflight_profile_dialect(item.at("profile_overrides"),geometry,mapped);
                const auto count=preflight_assembly_profiles(item.at("profile_overrides"));
                require(count<=262144-override_segments,"assembly document profile segment budget exceeded");
                override_segments+=count;
                add_topology_bytes(override_topology_bytes,raw_collection_topology_bytes(item.at("profile_overrides")),collection_topology_byte_limit);
                reserve_raw_mapped_profiles(item.at("profile_overrides"),decode_work);
            };
            for(const auto& item:value.at("instances")) {
                preflight_override(item);
                if(item.contains("nested_overrides")) {
                    const auto& paths=item.at("nested_overrides");
                    require(paths.is_array() && paths.size()<=4096,"invalid assembly override collection");
                    for(const auto& path:paths)preflight_override(path);
                }
            }
        }
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
            if(row_local)require(item.contains("profiles")==item.contains("parts"),
                "assembly type profiles and parts must be present together");
            const bool type_nested=nested && (!row_local || item.contains("profiles"));
            if(type_nested)fields(item,{"id","name","properties","materials","quantities","profiles","parts"});
            else fields(item, {"id", "name", "properties", "materials", "quantities"});
            AssemblyType type{item.at("id").get<std::string>(), item.at("name").get<std::string>(),
                item.at("properties").get<Strings>(), item.at("materials").get<Strings>(), decode_quantities(item.at("quantities"))};
            if(type_nested) {
                require(item.at("profiles").is_array() && item.at("profiles").size()<=4096 &&
                    item.at("parts").is_array() && item.at("parts").size()<=4096,"invalid assembly type collections");
                for(const auto& encoded:item.at("profiles")) {
                    profile_fields(encoded,geometry,mapped);
                    require(encoded.at("holes").is_array() && encoded.at("holes").size()<=1024 &&
                        encoded.at("elevation_m").is_number() && encoded.at("height_m").is_number(),"invalid assembly profile");
                    type.profiles.push_back(decode_profile(encoded));
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
            if(row_local)require(item.contains("root_transform")==item.contains("nested_overrides"),
                "assembly instance root transform and nested overrides must be present together");
            const bool instance_nested=nested && (!row_local || item.contains("root_transform"));
            if(instance_nested && has_placement)fields_with_profiles(item,{"id","type_id","property_overrides","material_overrides","quantity_overrides","placement","root_transform","nested_overrides"},geometry);
            else if(instance_nested)fields_with_profiles(item,{"id","type_id","property_overrides","material_overrides","quantity_overrides","root_transform","nested_overrides"},geometry);
            else if (has_placement) fields_with_profiles(item, {"id", "type_id", "property_overrides", "material_overrides", "quantity_overrides", "placement"},geometry);
            else fields_with_profiles(item, {"id", "type_id", "property_overrides", "material_overrides", "quantity_overrides"},geometry);
            std::optional<AssemblyPlacement> decoded_placement;
            if (has_placement) {
                const auto& encoded = item.at("placement");
                const bool mirror=encoded.is_object() && encoded.contains("mirrored_y");
                const bool placement_vertical=vertical && (!row_local || encoded.contains("vertical_scale"));
                if(placement_vertical && mirror)fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale","mirrored_y","vertical_scale"});
                else if(placement_vertical)fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale","vertical_scale"});
                else if(mirror)fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale","mirrored_y"});
                else fields(encoded,{"host_entity_id","translation_m","rotation_radians","scale"});
                if(mirror) {
                    require(encoded.at("mirrored_y").is_boolean(),"assembly placement mirror parity must be boolean");
                }
                const auto& coordinates=encoded.at("translation_m");
                const bool coordinates_xyz=row_local ? coordinates.is_array() && coordinates.size()==3 : placement_xyz;
                require(encoded.at("host_entity_id").is_string() &&
                        encoded.at("translation_m").is_array() &&
                        (row_local ? (coordinates.size()==2 || coordinates.size()==3) :
                            coordinates.size() == (placement_xyz ? 3u:2u)) &&
                        encoded.at("translation_m")[0].is_number() &&
                        encoded.at("translation_m")[1].is_number() &&
                        (!coordinates_xyz || encoded.at("translation_m")[2].is_number()) &&
                        encoded.at("rotation_radians").is_number() &&
                        encoded.at("scale").is_number() &&
                        (!placement_vertical || encoded.at("vertical_scale").is_number()), "invalid assembly placement");
                decoded_placement = AssemblyPlacement{
                    encoded.at("host_entity_id").get<std::string>(),
                    {encoded.at("translation_m")[0].get<double>(),
                     encoded.at("translation_m")[1].get<double>()},
                    encoded.at("rotation_radians").get<double>(),
                    encoded.at("scale").get<double>(),
                    encoded.contains("mirrored_y") && encoded.at("mirrored_y").get<bool>(),
                    coordinates_xyz ? encoded.at("translation_m")[2].get<double>():0.0,
                    placement_vertical ? encoded.at("vertical_scale").get<double>():1.0};
            }
            AssemblyInstance instance{item.at("id").get<std::string>(), item.at("type_id").get<std::string>(),
                item.at("property_overrides").get<Strings>(), item.at("material_overrides").get<Strings>(),
                decode_quantities(item.at("quantity_overrides")), std::move(decoded_placement)};
            const auto segments=item.contains("profile_overrides") ? preflight_assembly_profiles(item.at("profile_overrides")):0;
            if(instance_nested) {
                if(!item.at("root_transform").is_null())instance.root_transform=decode_versioned_transform(item.at("root_transform"),vertical);
                const auto topology_bytes=item.contains("profile_overrides") ? raw_collection_topology_bytes(item.at("profile_overrides")):0;
                instance.nested_overrides=decode_paths(item.at("nested_overrides"),vertical,geometry,segments,topology_bytes);
            }
            if(item.contains("profile_overrides"))instance.profile_overrides=decode_assembly_profiles(item.at("profile_overrides"));
            instances.push_back(std::move(instance));
        }
        return create(std::move(materials), std::move(types), std::move(instances));
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly JSON: ") + error.what());
    }
}
nlohmann::json transform_hosted_assembly_model(const nlohmann::json& actual_model,
    const std::map<std::string,AssemblyTransform,std::less<>>& instance_transforms,
    bool preserve_untouched_rows) {
    const auto source=AssemblyModel::from_json(actual_model);
    std::map<std::string,AssemblyPlacement,std::less<>> changed;
    AssemblyExpansionBudget source_budget;
    bool needs_xyz=false;
    bool needs_vertical=false;
    for(const auto& [id,world_transform]:instance_transforms) {
        const auto& instance=find(source.instances(),id);
        require(instance.placement.has_value(),"assembly transformation requires an actual hosted instance");
        const auto expansion=source.expand(instance,source_budget);
        require(!expansion.profiles.empty() || (!instance.profile_overrides &&
            std::none_of(instance.nested_overrides.begin(),instance.nested_overrides.end(),[](const auto& node) {
                return node.profile_overrides.has_value();
            })),"explicit component geometry cannot fall back to the legacy host body");
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
    const bool row_local=schema=="sketch.assemblies.v7" || schema=="sketch.assemblies.v8" || schema=="sketch.assemblies.v9";
    const bool source_xyz=schema=="sketch.assemblies.v5" || schema=="sketch.assemblies.v6" || row_local;
    const bool upgrade_vertical=needs_vertical && schema!="sketch.assemblies.v6" && !row_local;
    const bool upgrade_xyz=(needs_xyz || needs_vertical) && !source_xyz;
    if((upgrade_xyz || upgrade_vertical) && preserve_untouched_rows) {
        result["schema"]="sketch.assemblies.v7";
    } else if(upgrade_xyz || upgrade_vertical) {
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
        if(before.translation_z_m!=after.translation_z_m) {
            if(p.size()==2)p.push_back(after.translation_z_m);
            else p[2]=after.translation_z_m;
        }
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
nlohmann::json merge_disjoint_hosted_assembly_models(const nlohmann::json& actual_model,
    const nlohmann::json& accumulated_model, const nlohmann::json& incoming_model) {
    const auto exact_json=[](const nlohmann::json& a,const nlohmann::json& b) {
        return a==b && a.dump()==b.dump();
    };
    const auto remainder=[](nlohmann::json model) {
        model.erase("schema"); model.erase("instances"); return model;
    };
    (void)AssemblyModel::from_json(actual_model);
    (void)AssemblyModel::from_json(accumulated_model);
    (void)AssemblyModel::from_json(incoming_model);
    require(exact_json(remainder(actual_model),remainder(accumulated_model)) &&
        exact_json(remainder(actual_model),remainder(incoming_model)),
        "hosted composition changed actual catalog definitions");
    for(const auto* model:{&accumulated_model,&incoming_model})
        require(model->at("schema")==actual_model.at("schema") ||
            (actual_model.at("schema")!="sketch.assemblies.v8" && actual_model.at("schema")!="sketch.assemblies.v9" &&
                model->at("schema")=="sketch.assemblies.v7"),
            "hosted composition requires the actual or row-local catalog dialect");
    const auto& rows=actual_model.at("instances");
    require(accumulated_model.at("instances").size()==rows.size() &&
        incoming_model.at("instances").size()==rows.size(),
        "hosted composition changed the actual instance inventory");
    auto result=accumulated_model;
    if(incoming_model.at("schema")=="sketch.assemblies.v7")result["schema"]="sketch.assemblies.v7";
    for(std::size_t i=0;i<rows.size();++i) {
        const auto& before=rows.at(i);
        const auto& existing=accumulated_model.at("instances").at(i);
        const auto& incoming=incoming_model.at("instances").at(i);
        require(existing.at("id")==before.at("id") && incoming.at("id")==before.at("id"),
            "hosted composition changed actual row identities or order");
        for(const auto* row:{&existing,&incoming}) {
            if(exact_json(before,*row))continue;
            require(before.contains("placement") && row->contains("placement") &&
                exact_json(before.at("placement").at("host_entity_id"),row->at("placement").at("host_entity_id")),
                "hosted composition changed an unhosted row or placement host");
            auto retained_row=*row;
            retained_row.at("placement")=before.at("placement");
            require(exact_json(before,retained_row),"hosted composition changed fields outside placement authority");
        }
        if(exact_json(before,incoming))continue;
        require(exact_json(before,existing),"hosted composition overlaps one actual instance row");
        result.at("instances").at(i)=incoming;
    }
    (void)AssemblyModel::from_json(result);
    return result;
}
nlohmann::json merge_disjoint_directionally_resized_hosted_assembly_models(
    const nlohmann::json& actual_model, const nlohmann::json& accumulated_model,
    const nlohmann::json& incoming_model) {
    const auto exact_json=[](const nlohmann::json& a,const nlohmann::json& b) {
        return a==b && a.dump()==b.dump();
    };
    const auto remainder=[](nlohmann::json value) {
        value.erase("schema"); value.erase("instances"); return value;
    };
    const auto actual=AssemblyModel::from_json(actual_model);
    const auto accumulated=AssemblyModel::from_json(accumulated_model);
    const auto incoming=AssemblyModel::from_json(incoming_model);
    const auto index=[](const AssemblyModel& model) {
        std::map<std::string,const AssemblyInstance*,std::less<>> result;
        for(const auto& instance:model.instances())result.emplace(instance.id,&instance);
        return result;
    };
    const auto actual_instances=index(actual),accumulated_instances=index(accumulated),incoming_instances=index(incoming);
    require(exact_json(remainder(actual_model),remainder(accumulated_model)) &&
        exact_json(remainder(actual_model),remainder(incoming_model)),
        "directional hosted composition changed catalog definitions");
    for(const auto* model:{&accumulated_model,&incoming_model})
        require(model->at("schema")==actual_model.at("schema") ||
            model->at("schema")=="sketch.assemblies.v9" ||
            (actual_model.at("schema")!="sketch.assemblies.v8" &&
                actual_model.at("schema")!="sketch.assemblies.v9" && model->at("schema")=="sketch.assemblies.v7"),
            "directional hosted composition requires the actual or mapped row-local dialect");
    const auto& rows=actual_model.at("instances");
    require(accumulated_model.at("instances").size()==rows.size() &&
        incoming_model.at("instances").size()==rows.size(),
        "directional hosted composition changed the instance inventory");
    AssemblyExpansionBudget actual_work, accumulated_work, incoming_work;
    bool needs_mapped=false, needs_xyz=false, needs_vertical=false;
    const auto admit_row=[&](const AssemblyModel& model,const AssemblyInstance& before,
        const AssemblyInstance& after,const nlohmann::json& raw_before,const nlohmann::json& raw_after,
        const AssemblyExpansion& source,AssemblyExpansionBudget& work) {
        auto placement_only=after;placement_only.placement=before.placement;
        if(placement_only==before) {
            require(before.placement && after.placement &&
                before.placement->host_entity_id==after.placement->host_entity_id,
                "historical directional-cohort placement lost its actual host");
            auto retained=raw_after;retained["placement"]=raw_before.at("placement");
            for(const auto* key:{"root_transform","nested_overrides"})
                if(!raw_before.contains(key))retained.erase(key);
            require(exact_json(retained,raw_before),
                "historical directional-cohort placement changed retained row fields");
            (void)model.expand(after,work);
            needs_xyz=needs_xyz || after.placement->translation_z_m!=0;
            needs_vertical=needs_vertical || after.placement->vertical_scale!=1;
            return;
        }
        require(before.placement && after.placement==before.placement &&
            after.id==before.id && after.type_id==before.type_id &&
            after.root_transform==before.root_transform &&
            after.property_overrides==before.property_overrides &&
            after.material_overrides==before.material_overrides &&
            after.quantity_overrides==before.quantity_overrides,
            "directional hosted composition changed nongeometry instance authority");
        auto retained=raw_after;
        for(const auto* key:{"profile_overrides","nested_overrides","root_transform"}) {
            if(raw_before.contains(key))retained[key]=raw_before.at(key);else retained.erase(key);
        }
        require(exact_json(retained,raw_before),
            "directional hosted composition changed retained instance fields");
        std::map<std::vector<std::string>,const AssemblyPathOverride*> paths;
        for(const auto& path:before.nested_overrides)paths.emplace(path.part_path,&path);
        std::map<std::vector<std::string>,const nlohmann::json*> raw_paths;
        if(raw_before.contains("nested_overrides"))for(const auto& path:raw_before.at("nested_overrides"))
            raw_paths.emplace(path.at("part_path").get<std::vector<std::string>>(),&path);
        if(raw_after.contains("nested_overrides"))for(const auto& path:raw_after.at("nested_overrides")) {
            const auto found=raw_paths.find(path.at("part_path").get<std::vector<std::string>>());
            if(found==raw_paths.end())continue;
            auto unchanged=path;
            if(found->second->contains("profile_overrides"))unchanged["profile_overrides"]=found->second->at("profile_overrides");
            else unchanged.erase("profile_overrides");
            require(exact_json(unchanged,*found->second),
                "directional hosted composition changed retained path fields");
        }
        for(const auto& path:after.nested_overrides) {
            const auto found=paths.find(path.part_path);
            if(found==paths.end())
                require(!path.transform && path.property_overrides.empty() && path.material_overrides.empty() &&
                    path.quantity_overrides.empty() && path.profile_overrides.has_value(),
                    "directional hosted composition added nongeometry path authority");
            else {
                const auto& original=*found->second;
                require(path.transform==original.transform && path.property_overrides==original.property_overrides &&
                    path.material_overrides==original.material_overrides && path.quantity_overrides==original.quantity_overrides,
                    "directional hosted composition changed a path pose or overrides");
                paths.erase(found);
            }
        }
        require(paths.empty(),"directional hosted composition removed an actual path override");
        const auto candidate=model.expand(after,work);
        require(candidate.nodes.size()==source.nodes.size(),"directional hosted composition changed node inventory");
        for(std::size_t i=0;i<source.nodes.size();++i)
            require(source.nodes[i].part_path==candidate.nodes[i].part_path &&
                source.nodes[i].type_id==candidate.nodes[i].type_id &&
                source.nodes[i].transform==candidate.nodes[i].transform,
                "directional hosted composition changed an actual node frame");
        if(source.profiles.empty()) {
            require(candidate.profiles.size()==1 && candidate.profiles.front().part_path.empty() &&
                !candidate.profiles.front().profile.topology,
                "borrowed-host directional composition requires one anonymous root profile");
        } else {
            require(candidate.profiles.size()==source.profiles.size(),
                "directional hosted composition changed the own-profile inventory");
            for(std::size_t i=0;i<source.profiles.size();++i) {
                const auto& old=source.profiles[i];const auto& next=candidate.profiles[i];
                auto profile=next.profile;profile.planar_affine=old.profile.planar_affine;
                require(old.part_path==next.part_path && old.type_id==next.type_id &&
                    old.transform==next.transform && profile==old.profile,
                    "directional hosted composition changed source rings, topology or physical profile fields");
            }
        }
        needs_mapped=true;
    };
    auto result=accumulated_model;
    for(std::size_t i=0;i<rows.size();++i) {
        const auto& before=rows[i];const auto& existing=accumulated_model.at("instances")[i];
        const auto& next=incoming_model.at("instances")[i];
        require(existing.at("id")==before.at("id") && next.at("id")==before.at("id"),
            "directional hosted composition changed instance identities or order");
        const bool accumulated_changed=!exact_json(before,existing),incoming_changed=!exact_json(before,next);
        if(!accumulated_changed && !incoming_changed)continue;
        require(!accumulated_changed || !incoming_changed,"directional hosted composition overlaps one actual row");
        const auto id=before.at("id").get<std::string>();
        const auto& source_instance=*actual_instances.at(id);
        const auto source=actual.expand(source_instance,actual_work);
        if(accumulated_changed)admit_row(accumulated,source_instance,*accumulated_instances.at(id),before,existing,source,accumulated_work);
        else {
            admit_row(incoming,source_instance,*incoming_instances.at(id),before,next,source,incoming_work);
            result.at("instances")[i]=next;
        }
    }
    // Filtered phase contributions may carry a dialect promoted by baseline
    // rows that are no longer part of this original catalog. Derive the final
    // dialect from its actual source and the admitted accumulated/incoming rows,
    // just as row-preserving historical placement replay does. Definitions and
    // untouched rows stay exact; an incoming v9 tag alone grants no promotion.
    const auto schema=actual_model.at("schema").get<std::string>();
    const bool row_local=schema=="sketch.assemblies.v7" || schema=="sketch.assemblies.v8" || schema=="sketch.assemblies.v9";
    const bool source_xyz=schema=="sketch.assemblies.v5" || schema=="sketch.assemblies.v6" || row_local;
    result["schema"]=actual_model.at("schema");
    if(needs_mapped)result["schema"]="sketch.assemblies.v9";
    else if(((needs_xyz || needs_vertical) && !source_xyz) ||
        (needs_vertical && schema!="sketch.assemblies.v6" && !row_local))
        result["schema"]="sketch.assemblies.v7";
    (void)AssemblyModel::from_json(result);
    return result;
}
nlohmann::json retain_assembly_catalog_dialect(
    const nlohmann::json& actual_model, nlohmann::json generated_model) {
    const auto saved=actual_model.value("schema", nlohmann::json{});
    const auto generated=generated_model.at("schema");
    if(saved=="sketch.assemblies.v7" || saved=="sketch.assemblies.v8" || saved=="sketch.assemblies.v9" ||
        generated=="sketch.assemblies.v8" || generated=="sketch.assemblies.v9") {
        const auto source=AssemblyModel::from_json(actual_model);
        const auto candidate=AssemblyModel::from_json(generated_model);
        generated_model["schema"]=saved=="sketch.assemblies.v9" || generated=="sketch.assemblies.v9"
            ? nlohmann::json("sketch.assemblies.v9")
            : saved=="sketch.assemblies.v8" || generated=="sketch.assemblies.v8"
                ? nlohmann::json("sketch.assemblies.v8"):saved;
        // Canonical edits may add empty envelopes or normalize numbers to all
        // rows. Restore only source-qualified, semantically unchanged rows.
        // Both closed codecs have validated identities, fields and budgets.
        const auto retain_rows=[&](const char* key,const auto& before,const auto& after) {
            std::map<std::string,std::size_t,std::less<>> source_indices,raw_indices;
            for(std::size_t i=0;i<before.size();++i)source_indices.emplace(before[i].id,i);
            const auto& original=actual_model.at(key);
            for(std::size_t i=0;i<original.size();++i)
                raw_indices.emplace(original[i].at("id").get<std::string>(),i);
            for(auto& row:generated_model.at(key)) {
                const auto id=row.at("id").get<std::string>();
                const auto found=source_indices.find(id);
                if(found==source_indices.end())continue;
                const auto& current=find(after,id);
                if(before[found->second]==current)row=original.at(raw_indices.at(id));
            }
            auto ordered=nlohmann::json::array();
            std::map<std::string,const nlohmann::json*,std::less<>> generated_rows;
            for(const auto& row:generated_model.at(key))generated_rows.emplace(row.at("id").get<std::string>(),&row);
            for(const auto& row:original) {
                const auto id=row.at("id").get<std::string>();
                if(const auto found=generated_rows.find(id);found!=generated_rows.end()) {
                    ordered.push_back(*found->second);generated_rows.erase(found);
                }
            }
            for(const auto& [id,row]:generated_rows) {(void)id;ordered.push_back(*row);}
            generated_model[key]=std::move(ordered);
        };
        retain_rows("materials",source.materials(),candidate.materials());
        retain_rows("types",source.types(),candidate.types());
        retain_rows("instances",source.instances(),candidate.instances());
        const auto retain_transform=[](const AssemblyTransform& before,const AssemblyTransform& after,
            const nlohmann::json& raw,nlohmann::json& row) {
            auto& coordinates=row.at("translation_m");
            const auto& original=raw.at("translation_m");
            if(before.translation_m.x==after.translation_m.x)coordinates[0]=original[0];
            if(before.translation_m.y==after.translation_m.y)coordinates[1]=original[1];
            if(before.translation_m.z==after.translation_m.z) {
                if(original.size()==3)coordinates[2]=original[2];
                else if(coordinates.size()==3)coordinates.erase(coordinates.begin()+2);
            }
            const auto field=[&](const char* key,bool unchanged) {
                if(!unchanged)return;
                if(raw.contains(key))row[key]=raw.at(key);else row.erase(key);
            };
            field("rotation_radians",before.rotation_radians==after.rotation_radians);
            field("scale",before.scale==after.scale);
            field("mirrored_y",before.mirrored_y==after.mirrored_y);
            field("vertical_scale",before.vertical_scale==after.vertical_scale);
        };
        const auto retain_profiles=[](const auto& before,const auto& after,const nlohmann::json& raw,
            nlohmann::json& row,const char* field="profile_overrides") {
            if((!before && !after) || (before && after && *before==*after)) {
                if(raw.contains(field))row[field]=raw.at(field);
                else row.erase(field);
                return;
            }
            if(!before || !after || !raw.contains(field) || !row.contains(field))return;
            for(auto& encoded:row.at(field)) {
                const auto id=encoded.at("id").template get<std::string>();
                const auto old=std::find_if(before->begin(),before->end(),[&](const auto& profile){return profile.id==id;});
                const auto next=std::find_if(after->begin(),after->end(),[&](const auto& profile){return profile.id==id;});
                if(old==before->end() || next==after->end())continue;
                const auto raw_profile=std::find_if(raw.at(field).begin(),raw.at(field).end(),
                    [&](const auto& profile){return profile.at("id")==id;});
                require(raw_profile!=raw.at(field).end(),"assembly profile lost its raw source");
                const auto& old_raw=*raw_profile;
                if(*old==*next) {encoded=old_raw;continue;}
                if(boundary_equal(old->outer,next->outer))encoded["outer"]=old_raw.at("outer");
                const bool same_holes=old->holes.size()==next->holes.size() &&
                    std::equal(old->holes.begin(),old->holes.end(),next->holes.begin(),boundary_equal);
                if(same_holes)encoded["holes"]=old_raw.at("holes");
                if(old->elevation_m==next->elevation_m)encoded["elevation_m"]=old_raw.at("elevation_m");
                if(old->height_m==next->height_m)encoded["height_m"]=old_raw.at("height_m");
                if(old->material_slot==next->material_slot)encoded["material_slot"]=old_raw.at("material_slot");
                if(old->topology==next->topology && old_raw.contains("topology") && encoded.contains("topology")) {
                    encoded["topology"]=old_raw.at("topology");
                    if(encoded.contains("version") && old_raw.contains("version"))encoded["version"]=old_raw.at("version");
                }
                if(old->planar_affine && next->planar_affine) {
                    encoded["profile_version"]=old_raw.at("profile_version");
                    auto& affine=encoded.at("planar_affine");const auto& original=old_raw.at("planar_affine");
                    if(old->planar_affine==next->planar_affine)affine=original;
                    else {
                        affine["version"]=original.at("version");
                        const auto& a=*old->planar_affine;const auto& b=*next->planar_affine;
                        if(a.source_origin.x==b.source_origin.x)affine["source_origin_m"][0]=original.at("source_origin_m")[0];
                        if(a.source_origin.y==b.source_origin.y)affine["source_origin_m"][1]=original.at("source_origin_m")[1];
                        if(a.target_origin.x==b.target_origin.x)affine["target_origin_m"][0]=original.at("target_origin_m")[0];
                        if(a.target_origin.y==b.target_origin.y)affine["target_origin_m"][1]=original.at("target_origin_m")[1];
                        const double before_matrix[]{a.xx,a.xy,a.yx,a.yy},after_matrix[]{b.xx,b.xy,b.yx,b.yy};
                        for(std::size_t i=0;i<4;++i)if(before_matrix[i]==after_matrix[i])affine["matrix"][i]=original.at("matrix")[i];
                    }
                }
            }
            std::map<std::string,const nlohmann::json*,std::less<>> generated;
            for(const auto& profile:row.at(field))generated.emplace(profile.at("id").template get<std::string>(),&profile);
            auto ordered=nlohmann::json::array();
            for(const auto& profile:raw.at(field)) {
                const auto id=profile.at("id").template get<std::string>();
                if(const auto found=generated.find(id);found!=generated.end()) {
                    ordered.push_back(*found->second);generated.erase(found);
                }
            }
            for(const auto& [id,profile]:generated) {(void)id;ordered.push_back(*profile);}
            row[field]=std::move(ordered);
        };
        for(auto& row:generated_model.at("types")) {
            const auto id=row.at("id").get<std::string>();
            const auto before=std::find_if(source.types().begin(),source.types().end(),[&](const auto& type){return type.id==id;});
            if(before==source.types().end())continue;
            const auto& after=find(candidate.types(),id);
            const auto& raw=*std::find_if(actual_model.at("types").begin(),actual_model.at("types").end(),
                [&](const auto& type){return type.at("id")==id;});
            retain_profiles(&before->profiles,&after.profiles,raw,row,"profiles");
            if(before->parts==after.parts) {
                if(raw.contains("parts"))row["parts"]=raw.at("parts");else row.erase("parts");
            }
            if(row.contains("profiles")!=row.contains("parts")) {
                if(!row.contains("profiles"))row["profiles"]=nlohmann::json::array();
                if(!row.contains("parts"))row["parts"]=nlohmann::json::array();
            }
            if(before->quantities==after.quantities)row["quantities"]=raw.at("quantities");
        }
        // Geometry edits can change one field of an instance. Retain its
        // unchanged placement/transform and nested envelopes individually,
        // including raw integer/float spelling and absent optional defaults.
        for(auto& row:generated_model.at("instances")) {
            const auto id=row.at("id").get<std::string>();
            const auto before=std::find_if(source.instances().begin(),source.instances().end(),
                [&](const auto& instance){return instance.id==id;});
            if(before==source.instances().end())continue;
            const auto& after=find(candidate.instances(),id);
            const auto& raw=*std::find_if(actual_model.at("instances").begin(),actual_model.at("instances").end(),
                [&](const auto& instance){return instance.at("id")==id;});
            const auto retain_field=[&](const char* key) {
                if(raw.contains(key))row[key]=raw.at(key);else row.erase(key);
            };
            if(before->placement==after.placement)retain_field("placement");
            else if(before->placement && after.placement) {
                const auto as_transform=[](const AssemblyPlacement& placement) {
                    return AssemblyTransform{{placement.translation_m.x,placement.translation_m.y,placement.translation_z_m},
                        placement.rotation_radians,placement.scale,placement.mirrored_y,placement.vertical_scale};
                };
                retain_transform(as_transform(*before->placement),as_transform(*after.placement),
                    raw.at("placement"),row.at("placement"));
            }
            if(before->root_transform==after.root_transform)retain_field("root_transform");
            else if(before->root_transform && after.root_transform)
                retain_transform(*before->root_transform,*after.root_transform,raw.at("root_transform"),row.at("root_transform"));
            if(before->property_overrides==after.property_overrides)retain_field("property_overrides");
            if(before->material_overrides==after.material_overrides)retain_field("material_overrides");
            if(before->quantity_overrides==after.quantity_overrides)retain_field("quantity_overrides");
            if(before->nested_overrides==after.nested_overrides)retain_field("nested_overrides");
            else if(raw.contains("nested_overrides") && row.contains("nested_overrides")) {
                for(auto& path:row.at("nested_overrides")) {
                    const auto identity=path.at("part_path").get<std::vector<std::string>>();
                    const auto old=std::find_if(before->nested_overrides.begin(),before->nested_overrides.end(),
                        [&](const auto& change){return change.part_path==identity;});
                    const auto next=std::find_if(after.nested_overrides.begin(),after.nested_overrides.end(),
                        [&](const auto& change){return change.part_path==identity;});
                    if(old==before->nested_overrides.end() || next==after.nested_overrides.end())continue;
                    const auto& old_raw=*std::find_if(raw.at("nested_overrides").begin(),raw.at("nested_overrides").end(),
                        [&](const auto& change){return change.at("part_path")==path.at("part_path");});
                    if(*old==*next)path=old_raw;
                    else {
                        if(old->transform==next->transform)path["transform"]=old_raw.at("transform");
                        else if(old->transform && next->transform)
                            retain_transform(*old->transform,*next->transform,old_raw.at("transform"),path.at("transform"));
                        if(old->property_overrides==next->property_overrides)path["property_overrides"]=old_raw.at("property_overrides");
                        if(old->material_overrides==next->material_overrides)path["material_overrides"]=old_raw.at("material_overrides");
                        if(old->quantity_overrides==next->quantity_overrides)path["quantity_overrides"]=old_raw.at("quantity_overrides");
                        retain_profiles(old->profile_overrides,next->profile_overrides,old_raw,path);
                    }
                }
                std::map<std::vector<std::string>,const nlohmann::json*> generated_paths;
                for(const auto& path:row.at("nested_overrides"))
                    generated_paths.emplace(path.at("part_path").get<std::vector<std::string>>(),&path);
                auto ordered=nlohmann::json::array();
                for(const auto& path:raw.at("nested_overrides")) {
                    const auto identity=path.at("part_path").get<std::vector<std::string>>();
                    if(const auto found=generated_paths.find(identity);found!=generated_paths.end()) {
                        ordered.push_back(*found->second);generated_paths.erase(found);
                    }
                }
                for(const auto& [identity,path]:generated_paths) {(void)identity;ordered.push_back(*path);}
                row["nested_overrides"]=std::move(ordered);
            }
            retain_profiles(before->profile_overrides,after.profile_overrides,raw,row);
            // V7/V8/V9 require the root/path envelope pair together. A changed
            // path collection may introduce it on a previously bare row.
            if(row.contains("root_transform")!=row.contains("nested_overrides")) {
                if(!row.contains("root_transform"))row["root_transform"]=nullptr;
                if(!row.contains("nested_overrides"))row["nested_overrides"]=nlohmann::json::array();
            }
        }
        (void)AssemblyModel::from_json(generated_model);
        return generated_model;
    }
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

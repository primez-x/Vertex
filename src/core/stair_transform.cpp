#include "sketch/stair_transform.hpp"

#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/stair_object_edit.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Names = std::set<std::string, std::less<>>;
constexpr std::size_t entity_limit = 65536;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Stair transform: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            c=='-' || c=='_' || c=='.' || c==':';
    })) invalid("identity must contain 1..128 supported ASCII characters");
}
const Json* field(const Json& value, const std::string& key) {
    if (!value.is_object()) return nullptr;
    const auto found=value.find(key);
    return found==value.end()?nullptr:&*found;
}
void keys(const Json& value, const Names& required) {
    if (!value.is_object() || value.size()!=required.size()) invalid("proof fields are not closed");
    for (const auto& name : required) if (!value.contains(name)) invalid("proof field is missing");
}
double scalar(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) invalid("proof scalar must be finite");
    return value.get<double>();
}
Vec3 point(const Json& value) {
    if (!value.is_array() || value.size()!=3) invalid("proof point must contain three scalars");
    return {scalar(value.at(0)),scalar(value.at(1)),scalar(value.at(2))};
}
// Count structural work before recursive discovery, codec or layout expansion.
// This checks metadata for resource use only; strings never confer edit rights.
struct Budget {
    std::size_t nodes{}, bytes{};
    std::size_t byte_limit{64*1024*1024}, node_limit{4*1024*1024};
    void text(const std::string& text) {
        if (text.size()>byte_limit-bytes) invalid("source string budget exceeded");
        bytes+=text.size();
    }
    void read(const Json& value, std::size_t depth=0) {
        if (depth>64 || ++nodes>node_limit) invalid("source node/nesting budget exceeded");
        if ((value.is_object() || value.is_array()) && value.size()>entity_limit)
            invalid("source collection budget exceeded");
        if (value.is_number_float()) (void)scalar(value);
        if (value.is_binary()) {
            if (value.get_binary().size()>byte_limit-bytes) invalid("source binary budget exceeded");
            bytes+=value.get_binary().size();
        }
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child,depth+1);
        else if (value.is_object()) for (const auto& [key,child] : value.items()) {
            text(key); read(child,depth+1);
        }
    }
};
bool touches(const Json& value, const Names& names) {
    if (value.is_string()) return names.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key,child] : value.items()) if (names.contains(key) || touches(child,names)) return true;
    } else if (value.is_array()) for (const auto& child : value) if (touches(child,names)) return true;
    return false;
}
bool host_binding(const Json& value, const Names& names) {
    if (value.is_object()) {
        for (const auto& [key,child] : value.items()) {
            if (key=="host_entity_id" && touches(child,names)) return true;
            if (host_binding(child,names)) return true;
        }
    } else if (value.is_array()) for (const auto& child : value) if (host_binding(child,names)) return true;
    return false;
}
std::optional<std::string> rail_host(const Entity& entity) {
    if (entity.type!="railing") return std::nullopt;
    const auto host=field(entity.properties,"host");
    const auto id=host?field(*host,"stair_id"):nullptr;
    if (!id || !id->is_string()) return std::nullopt;
    return id->get<std::string>();
}
Json project_record(const Json& raw, const Names& owned) {
    Json result=Json::object();
    for (const auto& key : owned) result[key]=raw.contains(key)?raw.at(key):Json(nullptr);
    return result;
}
// Supply the existing typed profile lane with its complete closed projection.
// Opaque children stay in the actual source; entered receipts are source-derived.
StairObjectEditIntent profile_intent(const Entity& entity) {
    const bool stair=entity.type=="stair";
    const Names owned=stair?Names{"version","form","base_position_m","orientation_rad","riser_count",
        "total_rise_m","going_m","width_m","top_landing","level_connection","flights","landings","vertical_placement"}:
        Names{"version","form","base_position_m","orientation_rad","length_m","height_m","thickness_m",
            "post_spacing_m","host","vertical_placement"};
    auto profile=project_record(entity.properties,owned);
    for (const auto* key : {"flights","landings"}) if (profile.contains(key) && !profile.at(key).is_null()) {
        auto rows=Json::array();
        const Names fields=std::string_view(key)=="flights"?Names{"id","riser_count","going_m","width_m"}:
            Names{"id","depth_m","thickness_m","turn","return_gap_m","straight_alignment"};
        for (const auto& row : profile.at(key)) rows.push_back(project_record(row,fields));
        profile[key]=std::move(rows);
    }
    for (const auto* key : {"top_landing","level_connection","vertical_placement","host"}) {
        const auto raw=field(profile,key);
        if (!raw || raw->is_null()) continue;
        Names fields;
        if (std::string_view(key)=="top_landing") fields={"depth_m","thickness_m"};
        else if (std::string_view(key)=="level_connection") fields={"version","graph_id","link_id","lower_level_id","upper_level_id"};
        else if (std::string_view(key)=="vertical_placement") fields={"version","mode","offset_m"};
        else if (entity.properties.at("version")==2) fields={"stair_id","flight_id","side","start_fraction","end_fraction"};
        else if (raw->at("role")=="top") fields={"stair_id","role","incoming_flight_id","edge_index","start_fraction","end_fraction"};
        else fields={"stair_id","role","landing_id","incoming_flight_id","outgoing_flight_id","edge_index","start_fraction","end_fraction"};
        profile[key]=project_record(*raw,fields);
    }
    return {entity.id,std::move(profile),nullptr};
}
void catalog_context(const Entity& catalog, const Entities& source, const Names& inactive) {
    if (inactive.contains(catalog.id)) invalid("affected catalog is inactive: "+catalog.id);
    struct Binding { const char* key; const char* type; };
    constexpr Binding bindings[]{{"property_id","property"},{"building_id","building"},
        {"floor_id","floor"},{"layer_id","layer"},{"wall_id","wall"}};
    for (const auto& binding : bindings) {
        const auto id=field(catalog.properties,binding.key);
        if (!id) continue;
        if (!id->is_string()) invalid("affected catalog context is malformed: "+catalog.id);
        const auto found=source.find(id->get_ref<const std::string&>());
        if (found==source.end() || found->second.type!=binding.type)
            invalid("affected catalog context is unresolved: "+catalog.id);
    }
}
bool supported_catalog(const Json& raw) {
    const auto schema=field(raw,"schema");
    if (!schema || !schema->is_string()) return false;
    for (int version=1;version<=7;++version)
        if (*schema=="sketch.assemblies.v"+std::to_string(version)) return true;
    return false;
}
bool affected_catalog(const Entity& catalog, const Names& hosts) {
    if (host_binding(catalog.extensions,hosts) || host_binding(catalog.properties,hosts)) return true;
    const auto raw=field(catalog.properties,"model");
    if (!raw || !supported_catalog(*raw)) return touches(catalog.properties,hosts);
    const auto rows=field(*raw,"instances");
    if (!rows || !rows->is_array()) return rows && touches(*rows,hosts);
    for (const auto& row : *rows) {
        if (!row.is_object() && touches(row,hosts)) return true;
        const auto placement=field(row,"placement");
        if (placement && !placement->is_null() && touches(*placement,hosts)) return true;
    }
    return false;
}
void catalog_bounds(const Json& raw, std::size_t& inventory) {
    if (!supported_catalog(raw)) invalid("affected catalog schema is unsupported");
    for (const auto* key : {"materials","types","instances"}) {
        const auto rows=field(raw,key);
        if (!rows || !rows->is_array() || rows->size()>entity_limit-inventory)
            invalid("affected catalog inventory budget exceeded");
        inventory+=rows->size();
    }
}
// Source-frame G drives legacy host-derived parts. Type-owned profiles are
// world-authored, so F*G*F^-1 drives their placement. Raw level compensation
// belongs only to the physical producer and must never enter either operator.
AssemblyTransform world_transform(const Entities& source, const std::string& host,
    const ArchitecturalGroupTransform& captured) {
    const auto placement=resolve_site_presentation(source,host);
    const auto& frame=placement.forward;
    return conjugate_assembly_transform_through_rigid_frame(architectural_group_assembly_transform(captured),
        {{frame.translation_m.x,frame.translation_m.y,frame.translation_m.z},frame.rotation_radians,1.0,false});
}
bool admit_instance(const AssemblyModel& model, const AssemblyInstance& instance,
    const Entities& source, AssemblyExpansionBudget& budget) {
    if (!instance.placement) invalid("affected instance has no actual hosted placement");
    const auto& placement=*instance.placement;
    const auto host=source.find(placement.host_entity_id);
    if (host==source.end() || (host->second.type!="stair" && host->second.type!="railing"))
        invalid("affected instance has no actual stair or railing host");
    const auto expansion=model.expand(instance,budget);
    if (!expansion.profiles.empty()) {
        (void)make_assembly_geometry(expansion);
        return true;
    }
    const auto resolved=rail_host(host->second)?host->second:resolve_vertical_placement(source,host->second);
    (void)transform_assembly_shape(make_building_shape(decode_building_entity(resolved),source),
        {{placement.translation_m.x,placement.translation_m.y,placement.translation_z_m},
            placement.rotation_radians,placement.scale,placement.mirrored_y,placement.vertical_scale});
    return false;
}
Entities coordinate_catalogs(const Entities& source, Entities result,
    const std::map<std::string,ArchitecturalGroupTransform,std::less<>>& operations) {
    Names hosts;
    for (const auto& [id,operation] : operations) { (void)operation; hosts.insert(id); }
    const auto scope=constraint_phase_scope(source);
    std::size_t inventory=0;
    AssemblyExpansionBudget source_budget,candidate_budget;
    for (const auto& [id,catalog] : source) {
        if (catalog.type!="assembly_model" || !affected_catalog(catalog,hosts)) continue;
        catalog_context(catalog,source,scope.inactive_owner_ids);
        const auto raw=field(catalog.properties,"model");
        if (!raw) invalid("affected catalog has no actual model");
        catalog_bounds(*raw,inventory);
        const auto model=AssemblyModel::from_json(*raw);
        // Refuse affected references outside actual supported placement rows;
        // never rewrite opaque aliases, definitions or metadata strings.
        auto remainder=catalog;
        for (auto& row : remainder.properties.at("model").at("instances")) {
            row.erase("id");
            if (row.contains("placement") && row.at("placement").is_object()) row.at("placement").erase("host_entity_id");
        }
        if (touches(remainder.properties,hosts) || touches(remainder.extensions,hosts))
            invalid("affected catalog contains unsupported host references: "+id);
        std::map<std::string,AssemblyTransform,std::less<>> transforms;
        Names affected;
        for (const auto& instance : model.instances()) {
            if (!instance.placement || !hosts.contains(instance.placement->host_entity_id)) continue;
            const auto& host=instance.placement->host_entity_id;
            const bool typed=admit_instance(model,instance,source,source_budget);
            affected.insert(instance.id);
            // Identity physical owners retain exact catalogs, even when another
            // selected owner changes. All affected rows still receive admission.
            if (architectural_group_assembly_transform(operations.at(host))==AssemblyTransform{}) continue;
            transforms.emplace(instance.id,typed?world_transform(source,host,operations.at(host)):
                architectural_group_assembly_transform(operations.at(host)));
        }
        if (affected.empty()) invalid("affected catalog binding is outside a supported hosted row: "+id);
        auto& after=result.at(id);
        if (!transforms.empty()) after.properties["model"]=transform_hosted_assembly_model(*raw,transforms,true);
        const auto candidate=AssemblyModel::from_json(after.properties.at("model"));
        for (const auto& instance : candidate.instances()) if (affected.contains(instance.id))
            (void)admit_instance(candidate,instance,result,candidate_budget);
    }
    return result;
}
} // namespace

void stair_transform_detail::source_bounds(const Entities& source) {
    if (source.size()>entity_limit) invalid("source entity budget exceeded");
    Budget budget;
    for (const auto& [id,entity] : source) {
        identity(id);
        if (id!=entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("source requires actual identified entity envelopes");
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
    }
}
Json encode_stair_transform_intent(const StairTransformIntent& intent) {
    identity(intent.object_id);
    (void)architectural_group_assembly_transform(intent.transform);
    const auto& t=intent.transform;
    Json result{{"version",intent.quantity_entries.is_null()?1:2},{"object_id",intent.object_id},{"transform",{
        {"pivot_m",{t.pivot.x,t.pivot.y,t.pivot.z}},{"offset_m",{t.offset.x,t.offset.y,t.offset.z}},
        {"rotation_z_radians",t.rotation_z_radians},{"uniform_scale",t.scale},
        {"flip_horizontal",t.flip_horizontal},{"flip_vertical",t.flip_vertical}}}};
    if (!intent.quantity_entries.is_null()) {
        if (!intent.quantity_entries.is_object()) invalid("entered quantities must be a complete map");
        Budget budget; budget.byte_limit=proof_limit; budget.node_limit=65536;
        budget.read(intent.quantity_entries);
        result["quantity_entries"]=intent.quantity_entries;
        if (result.dump().size()>proof_limit) invalid("proof byte budget exceeded");
    }
    return result;
}
StairTransformIntent decode_stair_transform_intent(const Json& value) try {
    const auto version=field(value,"version");
    if (!version || !version->is_number_integer() || (*version!=1 && *version!=2))
        invalid("proof version is unsupported");
    const bool entered=*version==2;
    keys(value,entered?Names{"version","object_id","transform","quantity_entries"}:
        Names{"version","object_id","transform"});
    if (!value.at("object_id").is_string())
        invalid("proof version or identity is unsupported");
    if (entered) {
        if (!value.at("quantity_entries").is_object()) invalid("entered quantities must be a complete map");
        Budget budget; budget.byte_limit=proof_limit; budget.node_limit=65536; budget.read(value);
    }
    const auto& id=value.at("object_id").get_ref<const std::string&>();
    identity(id);
    const auto& raw=value.at("transform");
    keys(raw,{"pivot_m","offset_m","rotation_z_radians","uniform_scale","flip_horizontal","flip_vertical"});
    if (!raw.at("flip_horizontal").is_boolean() || !raw.at("flip_vertical").is_boolean())
        invalid("proof reflection flags must be boolean");
    StairTransformIntent intent{id,{point(raw.at("pivot_m")),point(raw.at("offset_m")),
        scalar(raw.at("rotation_z_radians")),scalar(raw.at("uniform_scale")),
        raw.at("flip_horizontal").get<bool>(),raw.at("flip_vertical").get<bool>()}};
    if (entered) intent.quantity_entries=value.at("quantity_entries");
    (void)encode_stair_transform_intent(intent);
    return intent;
} catch (const Json::exception& error) {
    invalid(std::string("malformed proof: ")+error.what());
}
Entities replay_stair_transform_entities(const Entities& source, const std::vector<StairTransformIntent>& intents) try {
    if (intents.size()>maximum_architectural_group_targets) invalid("target budget exceeded");
    if (intents.empty()) return source;
    stair_transform_detail::source_bounds(source);
    std::vector<ArchitecturalGroupTransformTarget> targets;
    std::map<std::string,ArchitecturalGroupTransform,std::less<>> operations;
    std::map<std::string,const Json*,std::less<>> entered_quantities;
    std::size_t bytes=0;
    for (const auto& intent : intents) {
        const auto size=encode_stair_transform_intent(intent).dump().size();
        if (size>proof_limit-bytes) invalid("batch proof byte budget exceeded");
        bytes+=size;
        if (!operations.emplace(intent.object_id,intent.transform).second) invalid("duplicate targets");
        if (!intent.quantity_entries.is_null()) entered_quantities.emplace(intent.object_id,&intent.quantity_entries);
        targets.push_back({intent.object_id,intent.transform});
    }
    const auto scope=constraint_phase_scope(source);
    // Active attached rails participate even when hidden or omitted from selection.
    // A selected rail already has equivalent intent; the host operation wins
    // exactly once rather than stacking an additional move on its dependent.
    for (const auto& [id,entity] : source) if (const auto host=rail_host(entity); host && operations.contains(*host)) {
        // Another saved design's dependent keeps its original descriptor. An
        // explicitly selected inactive owner remains an error below.
        if (!operations.contains(id) && scope.inactive_owner_ids.contains(id)) continue;
        if (!operations.contains(id) && operations.size()==4096) invalid("affected profile budget exceeded");
        operations[id]=operations.at(*host);
    }
    if (operations.size()>4096) invalid("affected profile budget exceeded");
    // Bound the entire physical cohort and every affected catalog inventory
    // before any family codec/layout or native admission starts. Codec limits
    // additionally bound individual flight/post expansions and assembly graphs.
    Names hosts;
    std::size_t risers=0, inventory=0;
    for (const auto& [id,operation] : operations) {
        (void)operation;
        hosts.insert(id);
        const auto found=source.find(id);
        if (found==source.end()) invalid("actual target is missing");
        if (scope.inactive_owner_ids.contains(id)) invalid("actual target is inactive in the saved design");
        if (found->second.type=="stair") {
            const auto count=field(found->second.properties,"riser_count");
            if (!count || !count->is_number_integer() || *count<1 || *count>10000)
                invalid("actual stair riser budget exceeded");
            const auto size=count->get<std::size_t>();
            if (size>100000-risers) invalid("affected stair geometry budget exceeded");
            risers+=size;
        }
    }
    for (const auto& [id,catalog] : source) {
        (void)id;
        if (catalog.type!="assembly_model" || !affected_catalog(catalog,hosts)) continue;
        const auto raw=field(catalog.properties,"model");
        if (!raw) invalid("affected catalog has no actual model");
        catalog_bounds(*raw,inventory);
    }
    const auto geometry=stair_transform_detail::stage_geometry(source,targets);
    std::vector<StairObjectEditIntent> profiles;
    profiles.reserve(operations.size());
    for (const auto& [id,operation] : operations) {
        auto profile=profile_intent(geometry.at(id));
        const auto base=field(source.at(id).properties,"base_position_m");
        if (base && base->is_array() && operation.scale==1.0) {
            const auto resolved=resolve_vertical_placement(source,source.at(id));
            const auto& pivot=resolved.properties.at("base_position_m");
            if (operation.pivot.x==scalar(pivot.at(0)) && operation.pivot.y==scalar(pivot.at(1)) &&
                operation.pivot.z==scalar(pivot.at(2))) {
                const double offsets[]{operation.offset.x,operation.offset.y,operation.offset.z};
                const bool reflected_stair=source.at(id).type=="stair" &&
                    operation.flip_horizontal!=operation.flip_vertical;
                // A stair's odd reflection changes its canonical XY base to
                // the first flight's far side. Preserve that producer shift;
                // anchored yaw and unmoved Z retain exact source encodings.
                for (std::size_t axis=0;axis<3;++axis)
                    if (offsets[axis]==0.0 && (axis==2 || !reflected_stair))
                        profile.profile_fields.at("base_position_m").at(axis)=base->at(axis);
            }
        }
        if (const auto entered=entered_quantities.find(id); entered!=entered_quantities.end()) {
            profile.quantity_entries=*entered->second;
            // Numeric placement inputs own their exact entered metres. Restore
            // only roundoff at the captured transform's resulting coordinates;
            // the typed profile lane still validates every receipt and metadata.
            for (std::size_t axis=0;axis<3;++axis) {
                const auto pointer="/base_position_m/"+std::to_string(axis);
                const auto receipt=field(profile.quantity_entries,pointer);
                const auto version=receipt?field(*receipt,"version"):nullptr;
                if (!version || *version!=1) continue;
                auto& coordinate=profile.profile_fields.at("base_position_m").at(axis);
                const auto transformed=scalar(coordinate);
                const auto original=scalar(source.at(id).properties.at("base_position_m").at(axis));
                const auto old_quantities=field(source.at(id).properties,"quantity_entries");
                const auto old_receipt=old_quantities?field(*old_quantities,pointer):nullptr;
                if (transformed==original && old_receipt && old_receipt->dump()==receipt->dump()) continue;
                const auto metres=decode_constraint_quantity_receipt(*receipt).metres;
                if (transformed==original && metres!=transformed)
                    invalid("entered receipt cannot move an unchanged coordinate");
                if (std::abs(metres-transformed)>1e-12*std::max(1.0,std::abs(transformed)))
                    invalid("entered placement differs from its captured transform");
                if (metres!=transformed) coordinate=metres;
            }
        }
        profiles.push_back(std::move(profile));
    }
    // Actual profile replay retains numeric encodings, metadata and unaffected
    // receipts, rejects stale quantities and admits attachments/connected levels.
    auto result=replay_stair_object_edit_entities(source,profiles);
    result=coordinate_catalogs(source,std::move(result),operations);
    validate_document_site_frames(result);
    return result;
} catch (const Standard_Failure& error) {
    const auto message=error.GetMessageString();
    invalid(std::string("native geometry admission failed: ")+(message?message:"Open CASCADE failure"));
} catch (const Json::exception& error) {
    invalid(std::string("malformed actual source: ")+error.what());
}
Entities stage_stair_group_transform_entities(const Entities& source,
    std::span<const ArchitecturalGroupTransformTarget> targets) {
    if (targets.empty() || targets.size()>maximum_architectural_group_targets) invalid("target budget exceeded");
    std::vector<StairTransformIntent> intents;
    intents.reserve(targets.size());
    for (const auto& target : targets) intents.push_back({target.entity_id,target.transform});
    return replay_stair_transform_entities(source,intents);
}
} // namespace sketch

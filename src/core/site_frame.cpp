#include "sketch/site_frame.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/constraint_entity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>
#include <span>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
void require(bool condition, std::string_view message) {
    if (!condition) throw std::invalid_argument(std::string(message));
}
void identifier(std::string_view value) {
    require(!value.empty() && value.size() <= 1024 &&
        !std::all_of(value.begin(),value.end(),[](unsigned char c){ return std::isspace(c); }),
        "site-frame identifier must be nonblank and bounded");
    try { (void)Json(std::string(value)).dump(); }
    catch (const Json::exception&) { throw std::invalid_argument("site-frame identifier must be valid UTF-8"); }
}
void finite(double value) { require(std::isfinite(value),"site-frame number must be finite"); }
void finite(Vec3 point) { finite(point.x); finite(point.y); finite(point.z); }
void rigid(const SiteRigidTransform& value) { finite(value.translation_m); finite(value.rotation_radians); }
void edit(const SiteEditTransform& value) {
    finite(value.translation_m); finite(value.rotation_radians); finite(value.scale);
    require(value.scale > 0,"site edit requires positive uniform scale");
}
void fields(const Json& value, std::initializer_list<std::string_view> expected) {
    require(value.is_object() && value.size()==expected.size(),"invalid site-frame object fields");
    for (const auto field : expected) require(value.contains(std::string(field)),"missing site-frame field");
}
void version(const Json& value) {
    require(value.at("version").is_number_integer() && value.at("version")==1,
        "unsupported site-frame version");
}
double number(const Json& value) {
    require(value.is_number(),"site-frame coordinate must be numeric");
    const auto result=value.get<double>(); finite(result); return result;
}
std::string text(const Json& value) {
    require(value.is_string(),"site-frame identity/mode must be a string");
    auto result=value.get<std::string>(); identifier(result); return result;
}
Vec3 vector(const Json& value) {
    require(value.is_array() && value.size()==3,"site frame requires exactly three coordinates");
    return {number(value[0]),number(value[1]),number(value[2])};
}
const Json* property(const Entity& entity, const char* key) {
    require(entity.properties.is_object(),"site-frame owner properties must be an object");
    const auto found=entity.properties.find(key);
    return found==entity.properties.end() ? nullptr : &found.value();
}
std::string reference(const Entity& entity, const char* key) {
    const auto* value=property(entity,key); return value ? text(*value) : std::string{};
}
bool architectural(std::string_view type) {
    constexpr std::array<std::string_view,12> types{"boundary","measurement_boundary","room_boundary",
        "room","wall","slab","roof","stair","railing","column","beam","opening"};
    return std::find(types.begin(),types.end(),type)!=types.end();
}
bool container(std::string_view type) {
    return type=="property" || type=="building" || type=="floor" || type=="layer";
}
void valid_limits(const SiteFrameLimits& limits) {
    require(limits.maximum_entities>0 && limits.maximum_join_members>0 &&
        limits.maximum_dependencies>0 && limits.maximum_dependency_bytes>0 &&
        limits.maximum_cached_dependency_entries>0,
        "site-frame limits must be nonzero");
}

// Reject deeply nested/oversized opaque metadata before recursive JSON dump.
// Actual encoded byte counts below remain the authoritative digest budget.
void preflight_json(const Json& value, std::size_t& remaining, std::size_t depth=0) {
    require(depth<=64,"site-frame dependency metadata nesting exceeds budget");
    require(!value.is_discarded(),"discarded JSON cannot bind site-frame dependencies");
    if (value.is_number_float()) finite(value.get<double>());
    const auto consume=[&](std::size_t count) {
        require(count<=remaining,"site-frame dependency bytes exceed budget"); remaining-=count;
    };
    consume(1);
    if (value.is_string()) consume(value.get_ref<const std::string&>().size());
    if (value.is_object()) {
        for (auto it=value.begin();it!=value.end();++it) {
            consume(it.key().size()); preflight_json(it.value(),remaining,depth+1);
        }
    } else if (value.is_array()) {
        for (const auto& item:value) preflight_json(item,remaining,depth+1);
    }
}
struct Captured {
    SitePresentationPlacement placement;
    std::set<std::string,std::less<>> dependencies;
    std::optional<SiteAnnotationTarget> annotation_target;
};

class Resolver {
public:
    Resolver(const SiteFrameEntities& entities,const SiteFrameLimits& limits)
        : entities_(entities),limits_(limits) {
        valid_limits(limits);
        require(entities.size()<=limits.maximum_entities,"site-frame entity count exceeds budget");
        for (const auto& [id,entity]:entities) {
            identifier(id); require(id==entity.id,"site-frame entity map identity mismatch");
            require(entity.properties.is_object(),"site-frame properties must be objects");
        }
        organization_=organize_project(entities);
    }
    void validate_contract(const Entity& entity) const {
        if (validated_contracts_.contains(entity.id)) return;
        if (const auto* p=property(entity,"site_frame")) {
            require(entity.type=="property","site_frame is owned only by a property");
            (void)decode_site_frame(*p);
        }
        if (const auto* p=property(entity,"site_placement")) {
            require(entity.type=="building","site_placement is owned only by a building");
            (void)decode_building_site_placement(*p);
        }
        if (const auto* p=property(entity,"presentation_frame")) {
            require(!container(entity.type),"containers cannot override their presentation frame");
            (void)decode_presentation_frame(*p);
        }
        if (const auto* p=property(entity,"terrain_elevation_binding")) {
            require(entity.type=="terrain_surface","terrain elevation binding requires terrain_surface");
            (void)decode_terrain_elevation_binding(*p);
        }
        validated_contracts_.insert(entity.id);
    }
    Captured resolve(std::string_view id) {
        if (const auto found=cached_.find(id);found!=cached_.end()) return found->second;
        const auto& entity=target(id);
        require(visiting_.size()<64 && visiting_.insert(entity.id).second,
            "site-frame host/join relationship cycle or depth exceeds budget");
        validate_contract(entity);
        Captured result; add(result,entity.id);
        const auto host=physical_host(entity);
        if (!host.empty()) {
            auto parent=resolve(host);
            result.placement=parent.placement; merge(result,parent);
            // Visibility/annotation layer is independent but must share the
            // physical host's property/building/floor when explicitly present.
            auto own=own_context(entity,result);
            if (!own.property_id.empty()) {
                require(own.property_id==parent.placement.drawing_context.property_id &&
                    (own.building_id.empty() || own.building_id==parent.placement.drawing_context.building_id) &&
                    (own.floor_id.empty() || own.floor_id==parent.placement.drawing_context.floor_id),
                    "hosted object drawing context differs from physical host");
                if (!own.layer_id.empty()) result.placement.drawing_context=own;
            }
            if (const auto* p=property(entity,"presentation_frame"))
                require(decode_presentation_frame(*p)==result.placement.source_frame.mode,
                    "hosted object cannot override the host presentation frame");
        } else if (entity.type=="wall_join" || entity.type=="roof_join") {
            resolve_join(entity,result);
        } else {
            result.placement.drawing_context=own_context(entity,result);
            resolve_independent(entity,result);
        }
        result.placement.inverse=inverse_site_transform(result.placement.forward);
        require(result.dependencies.size()<=limits_.maximum_cached_dependency_entries-cached_dependency_entries_,
            "site-frame cached dependencies exceed budget while resolving '"+entity.id+"'");
        cached_dependency_entries_+=result.dependencies.size();
        visiting_.erase(entity.id);
        cached_.emplace(entity.id,result);
        return result;
    }
    SitePresentationPlacement finish(Captured result) const {
        auto envelope=Json{{"domain","vertex-site-presentation-dependencies-v1"},
            {"source_frame",{{"mode",static_cast<int>(result.placement.source_frame.mode)},
                {"property_id",result.placement.source_frame.property_id},
                {"building_id",result.placement.source_frame.building_id}}},
            {"entities",Json::array()}};
        if (result.annotation_target)
            envelope["annotation_target"]={{"owner_entity_id",result.annotation_target->owner_entity_id},
                {"child_id",result.annotation_target->child_id}};
        std::size_t remaining=limits_.maximum_dependency_bytes;
        for (const auto& id:result.dependencies) {
            const auto& receipt=entity_receipt(id);
            require(receipt.bytes<=remaining,"site-frame dependency encoded bytes exceed budget");
            remaining-=receipt.bytes;
            envelope["entities"].push_back({{"id",id},{"sha256",receipt.sha256}});
            result.placement.dependency_ids.push_back(id);
        }
        const auto encoded=envelope.dump();
        require(encoded.size()<=limits_.maximum_dependency_bytes,
            "site-frame dependency encoded bytes exceed budget");
        result.placement.dependency_digest=sha256_hex(std::as_bytes(std::span(encoded.data(),encoded.size())));
        return result.placement;
    }
    Captured resolve_annotation(const SiteAnnotationTarget& requested) {
        identifier(requested.owner_entity_id); identifier(requested.child_id);
        if (const auto found=annotation_cached_.find(requested);found!=annotation_cached_.end()) return found->second;
        const auto& owner=target(requested.owner_entity_id,kAnnotationEntityType);
        validate_contract(owner);
        const auto& children=annotation_children(owner);
        const auto child=children.find(requested.child_id);
        require(child!=children.end(),"site-frame annotation child is missing");
        Captured result; result.annotation_target=requested; add(result,owner.id);
        const auto* explicit_frame=property(owner,"presentation_frame");
        if (!child->second.empty()) {
            result.placement.drawing_context=container_context(target(child->second,"layer"));
            context_dependencies(result,result.placement.drawing_context);
        } else require(!explicit_frame,"explicit annotation frame requires a child layer");
        if (explicit_frame) enroll(result,decode_presentation_frame(*explicit_frame));
        result.placement.inverse=inverse_site_transform(result.placement.forward);
        require(result.dependencies.size()<=limits_.maximum_cached_dependency_entries-cached_dependency_entries_,
            "site-frame cached annotation dependencies exceed budget");
        cached_dependency_entries_+=result.dependencies.size();
        annotation_cached_.emplace(requested,result);
        return result;
    }
    void validate_annotation_children(const Entity& owner) {
        if (owner.type!=kAnnotationEntityType || !property(owner,"presentation_frame")) return;
        const auto& children=annotation_children(owner);
        if (children.empty()) {
            Captured result; add(result,owner.id); (void)finish(std::move(result));
        }
        for (const auto& [id,layer]:children) {
            (void)layer; (void)finish(resolve_annotation({owner.id,id}));
        }
    }
    bool document_resolution_required(std::string_view id) {
        if (!document_affected_) index_document_affected();
        return document_affected_->contains(id);
    }
    void validate_constraint_frames(const Entity& entity) {
        if (entity.type!="constraint") return;
        const auto decoded=decode_constraint_entity(entity);
        // Future envelopes stay opaque; only the public decoder's supported
        // geometry bindings confer authority to inspect an owner's frame.
        if (!decoded.supported()) return;
        std::set<std::string,std::less<>> owners;
        for (const auto& binding:decoded.constraint->bindings) owners.insert(binding.owner_id);
        if (owners.size()<2) return;
        if (std::none_of(owners.begin(),owners.end(),[&](const auto& owner) {
                return document_resolution_required(owner);
            })) return; // Unenrolled legacy bindings need no presentation resolution.
        std::vector<SitePresentationPlacement> placements;
        std::optional<SitePresentationPlacement> enrolled;
        for (const auto& owner:owners) {
            auto placement=resolve(owner).placement;
            if (placement.source_frame.mode!=SiteFrameMode::world && !enrolled) enrolled=placement;
            placements.push_back(std::move(placement));
        }
        if (!enrolled) return; // Existing world-only relations retain semantics.
        for (const auto& placement:placements) {
            const auto& a=placement.forward; const auto& b=enrolled->forward;
            require(placement.source_frame==enrolled->source_frame &&
                a.translation_m.x==b.translation_m.x && a.translation_m.y==b.translation_m.y &&
                a.translation_m.z==b.translation_m.z && a.rotation_radians==b.rotation_radians,
                "constraint '"+entity.id+"' geometry owners must share one source frame and rigid pose");
        }
    }
private:
    const SiteFrameEntities& entities_;
    const SiteFrameLimits& limits_;
    ProjectOrganization organization_;
    std::map<std::string,Captured,std::less<>> cached_;
    std::map<SiteAnnotationTarget,Captured> annotation_cached_;
    std::map<std::string,std::map<std::string,std::string,std::less<>>,std::less<>> annotation_children_;
    std::size_t indexed_annotation_children_{};
    std::set<std::string,std::less<>> visiting_;
    std::size_t cached_dependency_entries_{};
    struct EntityReceipt { std::size_t bytes{}; std::string sha256; };
    mutable std::map<std::string,EntityReceipt,std::less<>> entity_receipts_;
    mutable std::map<std::string,SiteRigidTransform,std::less<>> property_poses_;
    mutable std::map<std::string,SiteRigidTransform,std::less<>> building_poses_;
    mutable std::set<std::string,std::less<>> validated_contracts_;
    std::optional<std::set<std::string,std::less<>>> document_affected_;
    void index_document_affected() {
        // Admission selects the contract's affected graph, without trying to
        // repair legacy references. Public resolve() remains strict for every
        // requested owner. Property site_frame alone never enrolls descendants.
        using IdSet=std::set<std::string,std::less<>>;
        using Dependents=std::map<std::string,std::vector<std::string>,std::less<>>;
        IdSet affected,building_context;
        for (const auto& [id,entity]:entities_) {
            for (const auto* key:{"site_frame","site_placement","presentation_frame","terrain_elevation_binding"})
                if (property(entity,key)) { affected.insert(id); break; }
            if (entity.type=="building" && property(entity,"site_placement")) building_context.insert(id);
        }
        if (affected.empty()) { document_affected_=std::move(affected); return; }
        Dependents context_dependents,physical_dependents;
        std::size_t remaining=limits_.maximum_cached_dependency_entries;
        const auto edge=[&](Dependents& dependents,const Entity& owner,const Json* value,
                            std::string_view expected_type) {
            if (!value || !value->is_string()) return;
            const auto& id=value->get_ref<const std::string&>();
            const auto found=entities_.find(id);
            if (found==entities_.end() || found->second.type!=expected_type) return;
            require(remaining>0,"site-frame admission relationship index exceeds budget");
            --remaining; dependents[id].push_back(owner.id);
        };
        for (const auto& [id,entity]:entities_) {
            (void)id;
            // Opaque objects, annotations and assemblies stay world unless
            // explicitly opted in. Only geometric/container ancestry confers
            // implicit building enrollment; property ancestry does not.
            if (architectural(entity.type) || container(entity.type) ||
                entity.type=="wall_join" || entity.type=="roof_join") {
                edge(context_dependents,entity,property(entity,"building_id"),"building");
                edge(context_dependents,entity,property(entity,"floor_id"),"floor");
                edge(context_dependents,entity,property(entity,"layer_id"),"layer");
            }
            if (entity.type=="opening")
                edge(physical_dependents,entity,property(entity,"wall_id"),"wall");
            if (entity.type=="railing") {
                if (const auto* host=property(entity,"host"); host && host->is_object()) {
                    const auto stair=host->find("stair_id");
                    if (stair!=host->end()) edge(physical_dependents,entity,&stair.value(),"stair");
                }
            }
            if (entity.type=="wall_join" || entity.type=="roof_join") {
                const bool wall=entity.type=="wall_join";
                const auto* members=property(entity,wall ? "wall_ids" : "roof_ids");
                if (members && members->is_array()) {
                    // Count every inspected member, including unknown legacy
                    // references, so an opaque join cannot make the scan unbounded.
                    require(members->size()<=remaining,"site-frame admission join scan exceeds budget");
                    remaining-=members->size();
                    for (const auto& member:*members)
                        edge(physical_dependents,entity,&member,wall ? "wall" : "roof");
                }
            }
        }
        const auto propagate=[](IdSet& selected,const Dependents& dependents) {
            std::vector<std::string> pending(selected.begin(),selected.end());
            for (std::size_t index=0;index<pending.size();++index) {
                const auto found=dependents.find(pending[index]);
                if (found==dependents.end()) continue;
                for (const auto& child:found->second)
                    if (selected.insert(child).second) pending.push_back(child);
            }
        };
        propagate(building_context,context_dependents);
        affected.insert(building_context.begin(),building_context.end());
        propagate(affected,physical_dependents);
        document_affected_=std::move(affected);
    }
    const std::map<std::string,std::string,std::less<>>& annotation_children(const Entity& owner) {
        if (const auto found=annotation_children_.find(owner.id);found!=annotation_children_.end()) return found->second;
        std::size_t remaining=limits_.maximum_dependency_bytes;
        preflight_json(owner.properties,remaining);
        const auto state=decode_annotation_entity(owner);
        require(state.labels.size()<=limits_.maximum_entities &&
            state.symbols.size()<=limits_.maximum_entities-state.labels.size(),
            "site-frame annotation child count exceeds budget");
        const auto count=state.labels.size()+state.symbols.size();
        require(count<=limits_.maximum_cached_dependency_entries-indexed_annotation_children_,
            "site-frame annotation child index exceeds shared budget");
        indexed_annotation_children_+=count;
        std::map<std::string,std::string,std::less<>> children;
        for (const auto& child:state.labels) children.emplace(child.id,child.placement.layer_id);
        for (const auto& child:state.symbols) children.emplace(child.id,child.placement.layer_id);
        return annotation_children_.emplace(owner.id,std::move(children)).first->second;
    }
    const EntityReceipt& entity_receipt(const std::string& id) const {
        if (const auto found=entity_receipts_.find(id);found!=entity_receipts_.end()) return found->second;
        const auto& e=target(id);
        std::size_t remaining=limits_.maximum_dependency_bytes;
        preflight_json(e.properties,remaining); preflight_json(e.extensions,remaining);
        std::string encoded;
        try {
            encoded=Json{{"domain","vertex-site-presentation-entity-v1"},
                {"id",e.id},{"type",e.type},{"properties",e.properties},
                {"required",e.required},{"extensions",e.extensions}}.dump();
        } catch (const Json::exception&) {
            throw std::invalid_argument("invalid JSON in site-frame dependency '"+id+"'");
        }
        require(encoded.size()<=limits_.maximum_dependency_bytes,
            "site-frame dependency encoded bytes exceed budget");
        EntityReceipt receipt{encoded.size(),sha256_hex(std::as_bytes(std::span(encoded.data(),encoded.size())))};
        return entity_receipts_.emplace(id,std::move(receipt)).first->second;
    }
    const Entity& target(std::string_view id,std::string_view type={}) const {
        identifier(id); const auto found=entities_.find(id);
        require(found!=entities_.end(),"site-frame reference targets missing entity '"+std::string(id)+"'");
        require(type.empty() || found->second.type==type,
            "site-frame reference '"+std::string(id)+"' requires type '"+std::string(type)+"'");
        return found->second;
    }
    void add(Captured& result,const std::string& id) const {
        (void)target(id); result.dependencies.insert(id);
        require(result.dependencies.size()<=limits_.maximum_dependencies,
            "site-frame dependency count exceeds budget");
    }
    void merge(Captured& result,const Captured& other) const {
        for (const auto& id:other.dependencies) add(result,id);
    }
    void context_dependencies(Captured& result,const DrawingContext& context) const {
        for (const auto* id:{&context.property_id,&context.building_id,&context.floor_id,&context.layer_id})
            if (!id->empty()) { validate_contract(target(*id)); add(result,*id); }
        if (!context.floor_id.empty()) {
            const auto& floor=target(context.floor_id,"floor");
            if (const auto* value=property(floor,"vertical_level_binding")) {
                require(value->is_object() && value->contains("graph_id"),"malformed floor vertical-level binding");
                const auto graph=text(value->at("graph_id"));
                (void)target(graph,"vertical_levels"); add(result,graph);
            }
        }
    }
    DrawingContext container_context(const Entity& entity) const {
        const auto& node=organization_.nodes.at(entity.id);
        if (!node.issues.empty()) throw std::invalid_argument("unresolved site-frame container '"+
            entity.id+"': "+node.issues.front());
        return node.context;
    }
    DrawingContext own_context(const Entity& entity,Captured& result) const {
        DrawingContext context;
        if (container(entity.type)) context=container_context(entity);
        else {
            // Only these four keys confer context; annotations' wall/target
            // witnesses and assembly catalogs confer no physical placement.
            constexpr std::array<std::pair<const char*,const char*>,4> refs{{
                {"layer_id","layer"},{"floor_id","floor"},{"building_id","building"},{"property_id","property"}}};
            bool resolved=false;
            for (const auto& [key,type]:refs) {
                const auto id=reference(entity,key); if (id.empty()) continue;
                const auto candidate=container_context(target(id,type));
                if (!resolved) { context=candidate; resolved=true; }
                const std::string* actual=type==std::string_view("layer") ? &context.layer_id :
                    type==std::string_view("floor") ? &context.floor_id :
                    type==std::string_view("building") ? &context.building_id : &context.property_id;
                require(*actual==id,"contradictory explicit site-frame container references");
            }
        }
        context_dependencies(result,context);
        return context;
    }
    std::string physical_host(const Entity& entity) const {
        if (entity.type=="opening") {
            const auto id=reference(entity,"wall_id");
            require(!id.empty(),"opening presentation requires its wall host");
            (void)target(id,"wall"); return id;
        }
        if (entity.type=="railing") {
            if (const auto* h=property(entity,"host")) {
                require(h->is_object() && h->contains("stair_id"),"malformed hosted railing reference");
                const auto id=text(h->at("stair_id")); (void)target(id,"stair"); return id;
            }
        }
        return {};
    }
    SiteRigidTransform property_pose(const std::string& id) const {
        require(!id.empty(),"site frame requires an explicit property context");
        if (const auto found=property_poses_.find(id);found!=property_poses_.end()) return found->second;
        const auto& p=target(id,"property");
        const auto* value=property(p,"site_frame");
        const auto pose=value ? decode_site_frame(*value).to_world : SiteRigidTransform{};
        property_poses_.emplace(id,pose); return pose;
    }
    SiteRigidTransform building_pose(const DrawingContext& context) const {
        require(!context.building_id.empty(),"building frame requires explicit building context");
        if (const auto found=building_poses_.find(context.building_id);found!=building_poses_.end()) return found->second;
        const auto& b=target(context.building_id,"building");
        const auto* value=property(b,"site_placement");
        const auto pose=compose_site_transforms(property_pose(context.property_id),
            value ? decode_building_site_placement(*value) : SiteRigidTransform{});
        building_poses_.emplace(context.building_id,pose); return pose;
    }
    void enroll(Captured& result,SiteFrameMode mode) const {
        auto& p=result.placement;
        if (mode==SiteFrameMode::world) { p.source_frame={}; p.forward={}; return; }
        const auto& context=p.drawing_context;
        if (mode==SiteFrameMode::site) {
            p.forward=property_pose(context.property_id);
            p.source_frame={mode,context.property_id,{}};
        } else {
            p.forward=building_pose(context);
            p.source_frame={mode,context.property_id,context.building_id};
        }
    }
    void resolve_independent(const Entity& entity,Captured& result) const {
        if (entity.type=="terrain_surface") {
            const auto* binding=property(entity,"terrain_elevation_binding");
            require(!property(entity,"presentation_frame"),
                "terrain presentation_frame conflicts with terrain elevation policy");
            if (!binding) return;
            const auto value=decode_terrain_elevation_binding(*binding);
            enroll(result,SiteFrameMode::site);
            if (value.mode==TerrainElevationBinding::Mode::declared_absolute) {
                const auto& p=target(result.placement.drawing_context.property_id,"property");
                const auto* frame=property(p,"site_frame");
                require(frame!=nullptr,"absolute terrain requires an explicit property vertical datum");
                const auto site=decode_site_frame(*frame);
                require(site.vertical_datum.identifier==value.datum_identifier,
                    "terrain datum differs from property vertical datum");
                result.placement.forward.translation_m.z-=site.vertical_datum.height_at_origin_m;
                rigid(result.placement.forward);
            }
            return;
        }
        if (entity.type=="assembly_instance") {
            const auto id=reference(entity,"assembly_catalog_id");
            if (!id.empty()) { (void)target(id,"assembly_model"); add(result,id); }
        }
        if (const auto* explicit_frame=property(entity,"presentation_frame")) {
            enroll(result,decode_presentation_frame(*explicit_frame)); return;
        }
        if (entity.type=="property") { enroll(result,SiteFrameMode::site); return; }
        if (architectural(entity.type) || container(entity.type)) {
            const auto& context=result.placement.drawing_context;
            if (!context.building_id.empty() && property(target(context.building_id,"building"),"site_placement"))
                enroll(result,SiteFrameMode::building);
        }
    }
    void resolve_join(const Entity& entity,Captured& result) {
        const bool wall=entity.type=="wall_join";
        const auto* members=property(entity,wall ? "wall_ids" : "roof_ids");
        require(members && members->is_array() && members->size()>=2 &&
            members->size()<=limits_.maximum_join_members,"invalid site-frame join member count");
        std::set<std::string,std::less<>> distinct;
        bool first=true;
        for (const auto& member:*members) {
            const auto id=text(member);
            require(distinct.insert(id).second,"duplicate site-frame join member");
            (void)target(id,wall ? "wall" : "roof");
            const auto captured=resolve(id);
            if (first) { result.placement=captured.placement; first=false; }
            else require(result.placement.source_frame==captured.placement.source_frame &&
                result.placement.drawing_context.building_id==captured.placement.drawing_context.building_id &&
                result.placement.drawing_context.property_id==captured.placement.drawing_context.property_id,
                "join '"+entity.id+"' members must share one building and source coordinate frame");
            merge(result,captured);
        }
        const auto context=own_context(entity,result);
        if (!context.property_id.empty()) {
            require(context.property_id==result.placement.drawing_context.property_id &&
                (context.building_id.empty() || context.building_id==result.placement.drawing_context.building_id),
                "join context differs from member building");
            if (!context.layer_id.empty()) result.placement.drawing_context=context;
        }
        if (const auto* p=property(entity,"presentation_frame"))
            require(decode_presentation_frame(*p)==result.placement.source_frame.mode,
                "join cannot override member frame");
    }
};
} // namespace

SiteFrame decode_site_frame(const Json& value) {
    fields(value,{"version","origin_m","rotation_radians","vertical_datum"}); version(value);
    const auto& datum=value.at("vertical_datum"); fields(datum,{"identifier","height_at_origin_m"});
    return {{vector(value.at("origin_m")),number(value.at("rotation_radians"))},
        {text(datum.at("identifier")),number(datum.at("height_at_origin_m"))}};
}
SiteRigidTransform decode_building_site_placement(const Json& value) {
    fields(value,{"version","translation_m","rotation_radians"}); version(value);
    return {vector(value.at("translation_m")),number(value.at("rotation_radians"))};
}
SiteFrameMode decode_presentation_frame(const Json& value) {
    fields(value,{"version","mode"}); version(value); const auto mode=text(value.at("mode"));
    if (mode=="world") return SiteFrameMode::world;
    if (mode=="site") return SiteFrameMode::site;
    require(mode=="building","unknown presentation coordinate frame"); return SiteFrameMode::building;
}
TerrainElevationBinding decode_terrain_elevation_binding(const Json& value) {
    require(value.is_object() && value.contains("mode"),"missing terrain elevation mode");
    const auto mode=text(value.at("mode"));
    if (mode=="relative_site_origin") {
        fields(value,{"version","mode"}); version(value); return {};
    }
    require(mode=="declared_absolute","unknown terrain elevation mode");
    fields(value,{"version","mode","datum_identifier"}); version(value);
    return {TerrainElevationBinding::Mode::declared_absolute,text(value.at("datum_identifier"))};
}
Vec3 site_transform_delta(Vec3 delta,const SiteRigidTransform& transform) {
    finite(delta); rigid(transform);
    const auto c=std::cos(transform.rotation_radians),s=std::sin(transform.rotation_radians);
    Vec3 result{c*delta.x-s*delta.y,s*delta.x+c*delta.y,delta.z}; finite(result); return result;
}
Vec3 site_transform_point(Vec3 point,const SiteRigidTransform& transform) {
    auto result=site_transform_delta(point,transform);
    result.x+=transform.translation_m.x; result.y+=transform.translation_m.y; result.z+=transform.translation_m.z;
    finite(result); return result;
}
Boundary site_transform_boundary(const Boundary& boundary,const SiteRigidTransform& transform) {
    rigid(transform); require(boundary.size()<=1000000,"presentation boundary segment budget exceeded");
    auto result=boundary;
    for (auto& segment:result) {
        finite(segment.sweep_radians);
        const auto a=site_transform_point({segment.start.x,segment.start.y,0},transform);
        const auto b=site_transform_point({segment.end.x,segment.end.y,0},transform);
        segment.start={a.x,a.y}; segment.end={b.x,b.y};
    }
    return result;
}
SiteRigidTransform compose_site_transforms(const SiteRigidTransform& outer,const SiteRigidTransform& inner) {
    rigid(outer); rigid(inner);
    SiteRigidTransform result{site_transform_point(inner.translation_m,outer),outer.rotation_radians+inner.rotation_radians};
    rigid(result); return result;
}
SiteRigidTransform inverse_site_transform(const SiteRigidTransform& transform) {
    rigid(transform);
    SiteRigidTransform result{{},-transform.rotation_radians};
    result.translation_m=site_transform_delta({-transform.translation_m.x,-transform.translation_m.y,-transform.translation_m.z},result);
    return result;
}
Vec3 site_edit_point(Vec3 point,const SiteEditTransform& transform) {
    edit(transform);
    const auto rotated=site_transform_delta(point,{{},transform.rotation_radians});
    Vec3 result{transform.scale*rotated.x+transform.translation_m.x,
        transform.scale*rotated.y+transform.translation_m.y,transform.scale*rotated.z+transform.translation_m.z};
    finite(result); return result;
}
SiteEditTransform conjugate_site_edit(const SiteEditTransform& world,const SiteRigidTransform& frame) {
    edit(world); rigid(frame);
    const auto moved_origin=site_edit_point(frame.translation_m,world);
    const auto local_origin=site_transform_point(moved_origin,inverse_site_transform(frame));
    SiteEditTransform result{local_origin,world.rotation_radians,world.scale}; edit(result); return result;
}
SitePresentationPlacement resolve_site_presentation(const SiteFrameEntities& entities,
    std::string_view entity_id,const SiteFrameLimits& limits) {
    Resolver resolver(entities,limits); return resolver.finish(resolver.resolve(entity_id));
}
SitePresentationPlacement resolve_site_presentation(const DocumentSnapshot& snapshot,
    std::string_view entity_id,const SiteFrameLimits& limits) {
    auto result=resolve_site_presentation(snapshot.entities(),entity_id,limits);
    result.snapshot_document_id=snapshot.document_id(); result.snapshot_revision=snapshot.revision(); return result;
}
std::map<std::string,SitePresentationPlacement,std::less<>> resolve_site_presentations(
    const SiteFrameEntities& entities,std::span<const std::string> entity_ids,const SiteFrameLimits& limits) {
    valid_limits(limits);
    require(entity_ids.size()<=limits.maximum_entities,"site-frame batch owner count exceeds budget");
    std::set<std::string,std::less<>> requested;
    for (const auto& id:entity_ids) {
        identifier(id); require(requested.insert(id).second,"duplicate site-frame batch owner '"+id+"'");
    }
    Resolver resolver(entities,limits);
    std::map<std::string,SitePresentationPlacement,std::less<>> result;
    for (const auto& id:requested) result.emplace(id,resolver.finish(resolver.resolve(id)));
    return result;
}
std::map<std::string,SitePresentationPlacement,std::less<>> resolve_site_presentations(
    const DocumentSnapshot& snapshot,std::span<const std::string> entity_ids,const SiteFrameLimits& limits) {
    auto result=resolve_site_presentations(snapshot.entities(),entity_ids,limits);
    for (auto& [id,placement]:result) {
        (void)id; placement.snapshot_document_id=snapshot.document_id(); placement.snapshot_revision=snapshot.revision();
    }
    return result;
}
std::map<SiteAnnotationTarget,SitePresentationPlacement> resolve_site_annotation_presentations(
    const SiteFrameEntities& entities,std::span<const SiteAnnotationTarget> targets,const SiteFrameLimits& limits) {
    valid_limits(limits);
    require(targets.size()<=limits.maximum_entities,"site-frame annotation batch count exceeds budget");
    std::set<SiteAnnotationTarget> requested;
    for (const auto& target:targets) {
        identifier(target.owner_entity_id); identifier(target.child_id);
        require(requested.insert(target).second,"duplicate site-frame annotation batch target");
    }
    Resolver resolver(entities,limits);
    std::map<SiteAnnotationTarget,SitePresentationPlacement> result;
    for (const auto& target:requested) result.emplace(target,resolver.finish(resolver.resolve_annotation(target)));
    return result;
}
std::map<SiteAnnotationTarget,SitePresentationPlacement> resolve_site_annotation_presentations(
    const DocumentSnapshot& snapshot,std::span<const SiteAnnotationTarget> targets,const SiteFrameLimits& limits) {
    auto result=resolve_site_annotation_presentations(snapshot.entities(),targets,limits);
    for (auto& [target,placement]:result) {
        (void)target; placement.snapshot_document_id=snapshot.document_id(); placement.snapshot_revision=snapshot.revision();
    }
    return result;
}
void validate_document_site_frames(const SiteFrameEntities& entities,const SiteFrameLimits& limits) {
    Resolver resolver(entities,limits);
    for (const auto& [id,entity]:entities) {
        resolver.validate_contract(entity);
        resolver.validate_annotation_children(entity);
        resolver.validate_constraint_frames(entity);
        // An annotation owner may contain children from several buildings;
        // its explicit mode enrolls each child rather than requiring owner context.
        if (resolver.document_resolution_required(id) &&
            (entity.type!=kAnnotationEntityType || !property(entity,"presentation_frame")))
            (void)resolver.resolve(id);
    }
}
} // namespace sketch

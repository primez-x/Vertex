#include "sketch/physical_wall_room_review.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/mixed_wall_opening_removal.hpp"
#include "sketch/architecture.hpp"
#include "sketch/architectural_object_removal.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/phase_wall_profile_capture.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/wall_split.hpp"
#include "sketch/wall_merge.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
[[noreturn]] void invalid(const std::string& reason) { throw std::invalid_argument("Physical room review: "+reason); }
// Only this expected legacy qualification refusal permits trying the new
// producer. Malformed source, codec and native failures never become fallback.
struct WallDeletionReferenceRefusal final : std::invalid_argument {
    explicit WallDeletionReferenceRefusal(const std::string& reason)
        : std::invalid_argument("Physical room review: "+reason) {}
};
void keys(const Json& value,std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size()!=expected.size()) invalid("unsupported intent fields");
    for (const auto* key:expected) if (!value.contains(key)) invalid("missing intent field");
}
bool valid_id(const std::string& id) {
    return !id.empty() && id.size()<=128 && std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c==':';
    });
}
void id(const std::string& value) { if (!valid_id(value)) invalid("invalid identity"); }
void digest(const std::string& value) {
    if (value.size()!=64 || !std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    })) invalid("invalid source digest");
}
Json context_json(const DrawingContext& c) {
    return {{"property_id",c.property_id},{"building_id",c.building_id},{"floor_id",c.floor_id},{"layer_id",c.layer_id},{"level_id",c.level_id}};
}
DrawingContext context(const Json& value) {
    keys(value,{"property_id","building_id","floor_id","layer_id","level_id"});
    DrawingContext c{value.at("property_id").get<std::string>(),value.at("building_id").get<std::string>(),
        value.at("floor_id").get<std::string>(),value.at("layer_id").get<std::string>(),value.at("level_id").get<std::string>()};
    for (const auto* v:{&c.property_id,&c.building_id,&c.floor_id,&c.layer_id,&c.level_id}) if (!v->empty()) id(*v);
    return c;
}
void ids(const std::vector<std::string>& values) {
    if (values.size()>65536) invalid("reference/identity budget exceeded");
    std::set<std::string> unique;for (const auto& value:values) { id(value);if (!unique.insert(value).second) invalid("duplicate identity decision"); }
}
std::string relation_name(RoomRelationKind kind) {
    if (kind==RoomRelationKind::independent) return "independent";
    if (kind==RoomRelationKind::follows) return "follows";
    if (kind==RoomRelationKind::derived_from) return "derived_from";
    invalid("unsupported graph relation");
}
Json relation_json(const RoomRelation& relation) {
    return {{"source_id",relation.source_id},{"target_id",relation.target_id},{"kind",relation_name(relation.kind)}};
}
RoomRelation relation(const Json& value) {
    keys(value,{"source_id","target_id","kind"});
    RoomRelation r{value.at("source_id").get<std::string>(),value.at("target_id").get<std::string>(),RoomRelationKind::independent};
    id(r.source_id);id(r.target_id);const auto kind=value.at("kind").get<std::string>();
    if (kind=="follows") r.kind=RoomRelationKind::follows;
    else if (kind=="derived_from") r.kind=RoomRelationKind::derived_from;
    else if (kind!="independent") invalid("unsupported graph relation");
    return r;
}
bool mentions(const Json& value,const std::set<std::string>& tokens) {
    if (value.is_string()) return tokens.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key,child]:value.items())
            if (tokens.contains(key) || mentions(child,tokens)) return true;
    } else if (value.is_array()) {
        for (const auto& child:value) if (mentions(child,tokens)) return true;
    }
    return false;
}
void witness(const PhysicalWallSpace& space,Vec2 point) {
    constexpr double r=8*default_geometry_tolerance_metres;
    Boundary probe{{{point.x-r,point.y-r},{point.x+r,point.y-r},0},{{point.x+r,point.y-r},{point.x,point.y+r},0},
        {{point.x,point.y+r},{point.x-r,point.y-r},0}};
    auto holes=space.holes;holes.push_back(std::move(probe));
    if (const auto error=validate_boundary_holes(space.boundary,holes)) invalid("witness is not strictly within the chosen clear space: "+*error);
}
IdentifiedBoundary identified(const PhysicalWallSpace& space,const PhysicalWallRoomFreshDecision& decision) {
    if (decision.fresh_ids.segment_ids.size()!=space.boundary.size() || decision.fresh_ids.vertex_ids.size()!=space.boundary.size())
        invalid("every assigned edge requires explicit fresh segment and vertex identities");
    IdentifiedBoundary result{decision.room_id,"room_boundary",{}};
    for (std::size_t i=0;i<space.boundary.size();++i)
        result.segments.push_back({decision.fresh_ids.segment_ids[i],decision.fresh_ids.vertex_ids[i],
            decision.fresh_ids.vertex_ids[(i+1)%space.boundary.size()],space.boundary[i]});
    return result;
}
std::set<std::string> current_source_owners(const PhysicalWallSpaces& detection,const std::string& selected) {
    std::set<std::string> result;
    if (!selected.empty()) result.insert(selected);
    for (const auto& space:detection.spaces) for (const auto& source:space.source_lineage.at("physical_sources"))
        result.insert(source.at("owner_id").get<std::string>());
    return result;
}
void validate_active(const Entities& source,const std::set<std::string>& rooms,
    std::string_view owner_kind="retained room") {
    for (const auto& [entity_id,e]:source) {
        (void)entity_id;if (e.type!="model_phases") continue;
        const auto model=ModelPhases::from_json(e.properties.at("model"));const auto active=model.active_state();
        for (const auto& member:model.entity_ids()) if (rooms.contains(member) && (!active.contains(member) || active.at(member)==ModelPhase::demolished))
            invalid(std::string(owner_kind)+" is inactive in the semantic phase");
    }
}
std::optional<std::string> room_creation_registry(const Entities& source,
    const std::map<std::string,std::string,std::less<>>& wall_registries,
    const PhysicalWallSpace& space,const std::string& selected_wall_id) {
    std::set<std::string> registries;
    const auto admit_face=[&](const Json& face) {
        for (const auto& edge:face.at("edges")) for (const auto& use:edge.at("source_uses")) {
            const auto owner=use.at("owner_id").get<std::string>();id(owner);
            const auto actual=source.find(owner);
            if (actual==source.end() || actual->second.id!=owner || actual->second.type!="wall")
                invalid("new room support must resolve to an actual physical wall");
            const auto registry=wall_registries.find(owner);
            if (registry!=wall_registries.end()) registries.insert(registry->second);
        }
    };
    // Physical inventory also contains unrelated components on this plane.
    // Membership follows actual boundary support, including inline holes.
    admit_face(space.source_lineage.at("outer"));
    for (const auto& hole:space.source_lineage.at("holes")) admit_face(hole);
    if (registries.size()>1) invalid("new room support belongs to conflicting phase registries");
    if (!registries.empty()) return *registries.begin();
    // A seeded review may explicitly supply phase authority for otherwise
    // unregistered support. Context-only discovery has no invented seed.
    const auto selected=wall_registries.find(selected_wall_id);
    return selected==wall_registries.end() ? std::nullopt : std::optional<std::string>{selected->second};
}
void register_created_rooms(Entities& result,
    const std::map<std::string,std::vector<std::string>,std::less<>>& created) {
    for (const auto& [registry_id,room_ids]:created) {
        auto& raw=result.at(registry_id).properties.at("model");
        const auto original=ModelPhases::from_json(raw);
        auto entities=original.entity_ids(),baseline=original.baseline_ids();
        auto alternatives=original.alternatives();
        entities.insert(entities.end(),room_ids.begin(),room_ids.end());
        if (original.active_alternative()) {
            const auto target=std::find_if(alternatives.begin(),alternatives.end(),[&](const auto& alternative) {
                return alternative.id==*original.active_alternative();
            });
            if (target==alternatives.end()) invalid("new room saved alternative no longer exists");
            target->proposed_ids.insert(target->proposed_ids.end(),room_ids.begin(),room_ids.end());
        } else baseline.insert(baseline.end(),room_ids.begin(),room_ids.end());
        const auto expected=ModelPhases::create(std::move(entities),std::move(baseline),
            std::move(alternatives),original.active_alternative());
        // Append only the reviewed identities to actual raw membership. Keep
        // original registry payload, selection, other alternatives and order.
        for (const auto& room_id:room_ids) raw.at("entity_ids").push_back(room_id);
        if (original.active_alternative()) {
            for (auto& alternative:raw.at("alternatives"))
                if (alternative.at("id")==*original.active_alternative())
                    for (const auto& room_id:room_ids) alternative.at("proposed_ids").push_back(room_id);
        } else for (const auto& room_id:room_ids) raw.at("baseline_ids").push_back(room_id);
        if (ModelPhases::from_json(raw).to_json()!=expected.to_json()) invalid("new room phase membership differs from saved authority");
    }
}
Json retain_registry_metadata(const Json& original,const Json& canonical) {
    if (canonical.is_object()) {
        auto result=original.is_object()?original:Json::object();
        for (const auto& [key,value]:canonical.items())
            result[key]=retain_registry_metadata(original.is_object() && original.contains(key)?original.at(key):Json{},value);
        return result;
    }
    if (canonical.is_array() && !canonical.empty() && canonical.front().is_object() &&
        (canonical.front().contains("id") || canonical.front().contains("object_id"))) {
        auto result=Json::array();
        for (const auto& item:canonical) {
            const auto* identity=item.contains("id")?"id":"object_id";
            Json previous;
            if (original.is_array()) for (const auto& old:original)
                if (old.is_object() && old.contains(identity) && old.at(identity)==item.at(identity)) { previous=old;break; }
            result.push_back(retain_registry_metadata(previous,item));
        }
        return result;
    }
    return canonical;
}
Entities remove_known_object_memberships(Entities result,const std::set<std::string>& removed,
    bool preserve_presentation_namespaces=false) {
    if (removed.empty()) return result;
    const auto filter_ids=[&](Json& values) {
        values.erase(std::remove_if(values.begin(),values.end(),[&](const auto& value) {
            return removed.contains(value.template get<std::string>());
        }),values.end());
    };
    for (auto& [entity_id,entity]:result) {
        (void)entity_id;
        if (entity.type=="model_phases") {
            auto& model=entity.properties.at("model");
            (void)ModelPhases::from_json(model);
            filter_ids(model["entity_ids"]);filter_ids(model["baseline_ids"]);
            for (auto& alternative:model["alternatives"]) {
                filter_ids(alternative["demolished_ids"]);filter_ids(alternative["proposed_ids"]);
            }
            (void)ModelPhases::from_json(model);
        } else if (entity.type==kSheetViewEntityType) {
            const auto model=decode_sheet_view_entity(entity);auto views=model.views();bool changed=false;
            for (auto& view:views) {
                const auto count=view.object_ids.size();
                const auto restricted=view.restrict_to_objects || !view.object_ids.empty();
                std::erase_if(view.object_ids,[&](const auto& owner){return removed.contains(owner);});
                if (count!=view.object_ids.size()) { view.restrict_to_objects=restricted;changed=true; }
                const auto overlays=view.overlays.size();
                std::erase_if(view.overlays,[&](const auto& overlay) {
                    return (!overlay.object_id.empty() && removed.contains(overlay.object_id)) ||
                        (overlay.dimension_binding && removed.contains(overlay.dimension_binding->object_id));
                });
                changed=changed || overlays!=view.overlays.size();
                if (view.presentation.appearance) {
                    auto& objects=view.presentation.appearance->objects;const auto appearances=objects.size();
                    std::erase_if(objects,[&](const auto& object){return removed.contains(object.object_id);});
                    changed=changed || appearances!=objects.size();
                }
            }
            if (changed) {
                const auto replacement=SheetViewModel::create(std::move(views),model.sheets(),
                    model.to_json().at("schedule_ids").get<std::vector<std::string>>(),model.sheet_order());
                entity.properties["model"]=retain_registry_metadata(entity.properties.at("model"),replacement.to_json());
                validate_sheet_view_entity(entity);
            }
        } else if (entity.type==kAnnotationEntityType) {
            validate_annotation_entity(entity);
            if (!entity.properties.at("state").contains("overrides")) continue;
            auto& overrides=entity.properties.at("state").at("overrides");
            overrides.erase(std::remove_if(overrides.begin(),overrides.end(),[&](const auto& record) {
                if (preserve_presentation_namespaces && record.at("target_kind")=="output_view") return false;
                return removed.contains(record.at("target_id").template get<std::string>());
            }),overrides.end());
            validate_annotation_entity(entity);
        }
    }
    return result;
}
void refuse_unresolved_wall_deletion_references(const Entities& original,const Entities& candidate,
    const std::set<std::string>& removed,bool complete_hosted_removal=false) {
    // Receipt containers may deliberately retain historical source identities.
    // Admit only supported, independently validated and unchanged evidence;
    // vendor metadata elsewhere never acquires that exemption by its name.
    if (const auto unsupported=validate_boundary_integrity(candidate)) invalid(*unsupported);
    const auto organization=organize_project(candidate);
    std::set<std::string> retained_output_views;
    if (complete_hosted_removal) for (const auto& [owner,entity]:candidate) {
        (void)owner;
        if (entity.type!=kSheetViewEntityType) continue;
        const auto model=decode_sheet_view_entity(entity);
        for (const auto& view:model.views()) retained_output_views.insert(view.id);
    }
    std::map<std::string,AssemblyModel,std::less<>> catalog_models;
    const auto catalog_model=[&](const std::string& catalog_id)->const AssemblyModel& {
        const auto previous=catalog_models.find(catalog_id);
        if (previous!=catalog_models.end()) return previous->second;
        const auto catalog=original.find(catalog_id);
        if (catalog==original.end() || catalog->second.type!="assembly_model") invalid("retained material assignment lacks actual catalog");
        return catalog_models.emplace(catalog_id,AssemblyModel::from_json(catalog->second.properties.at("model"))).first->second;
    };
    for (const auto& [entity_id,entity]:candidate) {
        auto properties=entity.properties,extensions=entity.extensions;
        const auto previous=original.find(entity_id);
        const auto unchanged=[](const Json& before,const Json& after,const char* field) {
            return before.is_object() && after.is_object() && before.contains(field) && after.contains(field) &&
                before.at(field).dump()==after.at(field).dump();
        };
        if (previous!=original.end() && previous->second.type==entity.type) {
            const bool identified_owner=can_recognize_boundary_entity_type(entity.type) &&
                inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1;
            if (identified_owner) {
                if (unchanged(previous->second.properties,properties,"boundary_authoring")) properties.erase("boundary_authoring");
                if (unchanged(previous->second.extensions,extensions,"boundary_geometry_derivation"))
                    extensions.erase("boundary_geometry_derivation");
                if (entity.type=="measurement_boundary" && unchanged(previous->second.properties,properties,"wall_measurement_source")) {
                    (void)exterior_wall_measurement_source_ids(entity);
                    properties.erase("wall_measurement_source");
                }
                if (is_physical_wall_room(entity) && unchanged(previous->second.extensions,extensions,"physical_wall_room")) {
                    const auto drawing_context=organization.drawing_context(entity_id);
                    if (!drawing_context || !drawing_context->complete()) invalid("historical room evidence has unresolved context");
                    (void)validate_retained_physical_wall_room_lineage(entity,*drawing_context);
                    extensions.erase("physical_wall_room");
                }
            }
            if (entity.type=="wall") {
                if (unchanged(previous->second.extensions,extensions,"wall_split_archive") &&
                    extensions.at("wall_split_archive").value("version",0)==1) {
                    validate_wall_split_archive(entity);extensions.erase("wall_split_archive");
                }
                if (unchanged(previous->second.extensions,extensions,"wall_merge_archive") &&
                    extensions.at("wall_merge_archive").value("version",0)==1) {
                    validate_wall_merge_archive(entity);extensions.erase("wall_merge_archive");
                }
            }
        }
        if (complete_hosted_removal && entity.type=="assembly_model") {
            // These validated catalog-local slots are declarations, never
            // document owner references. Preserve their authoritative bytes.
            (void)catalog_model(entity_id);
            auto& model=properties.at("model");
            for (auto& row:model.at("materials")) row.erase("id");
            for (auto& row:model.at("types")) {
                row.erase("id");row.erase("materials");
                if (row.contains("profiles")) for (auto& child:row.at("profiles")) {
                    child.erase("id");child.erase("material_slot");
                }
                if (row.contains("parts")) for (auto& child:row.at("parts")) {
                    child.erase("id");child.erase("type_id");child.erase("material_overrides");
                }
            }
            for (auto& row:model.at("instances")) {
                row.erase("id");row.erase("type_id");row.erase("material_overrides");
                if (row.contains("nested_overrides")) for (auto& child:row.at("nested_overrides")) {
                    child.erase("part_path");child.erase("material_overrides");
                }
            }
        }
        if (complete_hosted_removal) {
            if (properties.contains("material_assignment")) {
                auto& assignment=properties.at("material_assignment");
                if (!assignment.is_object() || !assignment.at("version").is_number_integer() || assignment.at("version")!=1)
                    invalid("unsupported retained material assignment");
                const auto& model=catalog_model(assignment.at("catalog_id").get<std::string>());
                const auto material_id=assignment.at("material_id").get<std::string>();
                if (std::none_of(model.materials().begin(),model.materials().end(),[&](const auto& row){return row.id==material_id;}))
                    invalid("retained material assignment lacks actual definition");
                assignment.erase("material_id");
            }
            if (entity.type=="wall" && properties.contains("layers")) {
                Wall wall;std::string error;
                if (!read_document_wall(entity,{},wall,error)) invalid(error);
                for (auto& layer:properties.at("layers")) {
                    layer.erase("id");
                    if (layer.contains("material") && !layer.at("material").is_null()) layer.at("material").erase("material_id");
                }
            } else if (entity.type==kSheetViewEntityType) {
                (void)decode_sheet_view_entity(entity);
                auto& model=properties.at("model");
                for (auto& view:model.at("views")) {
                    view.erase("id");
                    if (view.contains("overlays")) for (auto& overlay:view.at("overlays")) overlay.erase("id");
                }
                // The validated sheet graph owns these local declarations and
                // references. Their spelling never grants document-owner authority.
                model.erase("sheet_order");
                for (auto& sheet:model.at("sheets")) {
                    sheet.erase("id");
                    if (sheet.contains("revisions")) for (auto& revision:sheet.at("revisions")) revision.erase("id");
                    if (sheet.contains("viewports")) for (auto& viewport:sheet.at("viewports")) {
                        viewport.erase("id");viewport.erase("view_id");
                    }
                    if (sheet.contains("callouts")) for (auto& callout:sheet.at("callouts")) {
                        callout.erase("id");callout.erase("target_sheet_id");callout.erase("target_viewport_id");
                    }
                    if (sheet.contains("schedules")) for (auto& schedule:sheet.at("schedules")) schedule.erase("id");
                }
            } else if (entity.type==kAnnotationEntityType) {
                validate_annotation_entity(entity);
                auto& state=properties.at("state");
                if (state.contains("overrides")) for (auto& record:state.at("overrides")) {
                    if (record.at("target_kind")!="output_view") continue;
                    const auto target=record.at("target_id").get<std::string>();
                    if (retained_output_views.contains(target)) record.erase("target_id");
                }
            } else if (entity.type=="model_phases") {
                (void)ModelPhases::from_json(entity.properties.at("model"));
                for (auto& alternative:properties.at("model").at("alternatives")) alternative.erase("id");
                // Saved selection is local to the admitted alternatives.
                properties.at("model").erase("active_alternative");
            }
        }
        if (mentions(properties,removed) || mentions(extensions,removed))
            throw WallDeletionReferenceRefusal("surviving object "+entity_id+" has an unsupported reference to a deleted wall or attached object");
    }
}
std::vector<EntityChange> physical_wall_deletion_changes(const Entities& source,const std::vector<std::string>& wall_ids) {
    if (wall_ids.empty() || wall_ids.size()>128) invalid("deletion requires one to 128 physical walls");
    std::set<std::string> removed;
    for (const auto& wall_id:wall_ids) {
        id(wall_id);
        const auto wall=source.find(wall_id);
        if (wall==source.end() || wall->second.type!="wall" || wall->second.required)
            invalid("deletion requires removable existing physical walls");
        if (!removed.insert(wall_id).second) invalid("duplicate wall deletion identity");
    }
    const auto wall_roots=removed;
    for (const auto& [entity_id,entity]:source) {
        if (entity.type!="door" && entity.type!="window") continue;
        std::string host,error;
        if (!read_document_wall_id(entity,host,error)) invalid(error);
        if (wall_roots.contains(host)) removed.insert(entity_id);
    }
    for (const auto& [entity_id,entity]:source) {
        if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=decode_boundary_dimension_entity(entity);
            if (decoded.supported() && removed.contains(decoded.dimension->boundary_id)) removed.insert(entity_id);
        } else if (entity.type=="constraint") {
            const auto decoded=decode_constraint_entity(entity);
            if (decoded.supported() && std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                [&](const auto& binding){return removed.contains(binding.owner_id);})) removed.insert(entity_id);
        }
    }
    auto candidate=source;
    for (const auto& entity_id:removed) {
        if (source.at(entity_id).required) invalid("a required attached object prevents wall deletion");
        candidate.erase(entity_id);
    }
    candidate=remove_known_object_memberships(std::move(candidate),removed);
    refuse_unresolved_wall_deletion_references(source,candidate,removed);
    std::vector<EntityChange> changes;
    for (const auto& [entity_id,entity]:source) {
        const auto after=candidate.find(entity_id);
        if (after==candidate.end()) changes.push_back(EntityChange::erase(entity_id));
        else if (entity!=after->second || entity.properties.dump()!=after->second.properties.dump() ||
            entity.extensions.dump()!=after->second.extensions.dump()) changes.push_back(EntityChange::upsert(after->second));
    }
    return changes;
}
// Complete removal is a separate producer. The legacy producer above remains
// the authority for every historical raw singleton and grouped v31 proof.
struct WallDeletionSourceBudget {
    static constexpr std::size_t node_limit=4*1024*1024,byte_limit=64*1024*1024;
    std::size_t nodes{},bytes{};
    void text(const std::string& value) {
        if (value.size()>byte_limit-bytes) invalid("complete deletion source byte budget exceeded");
        bytes+=value.size();
    }
    void read(const Json& root) {
        std::vector<std::pair<const Json*,std::size_t>> pending{{&root,0}};
        while (!pending.empty()) {
            const auto [value,depth]=pending.back();pending.pop_back();
            if (depth>64 || ++nodes>node_limit) invalid("complete deletion source node/nesting budget exceeded");
            if (value->is_number_float() && !std::isfinite(value->get<double>())) invalid("nonfinite complete deletion source scalar");
            if (value->is_string()) text(value->get_ref<const std::string&>());
            if (value->is_binary()) {
                if (value->get_binary().size()>byte_limit-bytes) invalid("complete deletion binary budget exceeded");
                bytes+=value->get_binary().size();
            }
            if (!value->is_structured()) continue;
            if (value->size()>node_limit-nodes || pending.size()>node_limit-nodes-value->size())
                invalid("complete deletion pending-node budget exceeded");
            if (value->is_object()) for (const auto& [key,child]:value->items()) {
                text(key);pending.emplace_back(&child,depth+1);
            } else for (const auto& child:*value) pending.emplace_back(&child,depth+1);
        }
    }
};
void bound_complete_wall_deletion_source(const Entities& source) {
    if (source.size()>65536) invalid("complete deletion source entity budget exceeded");
    WallDeletionSourceBudget budget;std::size_t phase_work{},catalog_rows{};
    for (const auto& [entity_id,entity]:source) {
        id(entity_id);
        if (entity.id!=entity_id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("complete deletion requires actual identified entity envelopes");
        budget.text(entity_id);budget.text(entity.type);budget.read(entity.properties);budget.read(entity.extensions);
        if (entity.type=="model_phases") {
            const auto& model=entity.properties.at("model");
            const auto& members=model.at("entity_ids");const auto& alternatives=model.at("alternatives");
            if (!members.is_array() || members.size()>65536 || !alternatives.is_array() || alternatives.size()>4096 ||
                members.size()>(2000000-phase_work)/(alternatives.size()+1))
                invalid("complete deletion phase inventory/work budget exceeded");
            phase_work+=members.size()*(alternatives.size()+1);
        } else if (entity.type=="assembly_model") {
            const auto& model=entity.properties.at("model");
            for (const auto* key:{"materials","types","instances"}) {
                const auto& rows=model.at(key);
                if (!rows.is_array() || rows.size()>65536-catalog_rows)
                    invalid("complete deletion catalog inventory budget exceeded");
                catalog_rows+=rows.size();
            }
        }
    }
}
bool unqualified_wall_component_reference(const Json& root,const std::set<std::string>& locals,const Entities& source) {
    if (root.is_object()) {
        if (root.contains("instance_id") && root.at("instance_id").is_string() &&
            locals.contains(root.at("instance_id").get<std::string>())) {
            std::optional<std::string> qualified;
            for (const auto* key:{"catalog_id","assembly_catalog_id"}) if (root.contains(key)) {
                if (!root.at(key).is_string()) return true;
                const auto catalog=root.at(key).get<std::string>();const auto found=source.find(catalog);
                if (found==source.end() || found->second.type!="assembly_model" || (qualified && *qualified!=catalog)) return true;
                qualified=catalog;
            }
            if (!qualified) return true;
        }
        for (const auto& child:root) if (unqualified_wall_component_reference(child,locals,source)) return true;
    } else if (root.is_array())
        for (const auto& child:root) if (unqualified_wall_component_reference(child,locals,source)) return true;
    return false;
}
void refuse_retired_wall_overlay_references(const Entities& source,const Entities& candidate) {
    using OverlayKeys=std::map<std::pair<std::string,std::string>,std::set<std::string>>;
    OverlayKeys retired;
    for (const auto& [entity_id,entity]:source) if (entity.type==kSheetViewEntityType) {
        const auto before=decode_sheet_view_entity(entity);const auto& after_entity=candidate.at(entity_id);
        const auto after=decode_sheet_view_entity(after_entity);
        for (const auto& view:before.views()) {
            const auto surviving=std::find_if(after.views().begin(),after.views().end(),[&](const auto& row){return row.id==view.id;});
            if (surviving==after.views().end()) invalid("wall cleanup unexpectedly removed a saved view");
            for (const auto& overlay:view.overlays)
                if (std::none_of(surviving->overlays.begin(),surviving->overlays.end(),[&](const auto& row){return row.id==overlay.id;}))
                    retired[{entity_id,view.id}].insert(overlay.id);
        }
    }
    for (const auto& [entity_id,entity]:candidate) {
        for (const auto* root:{&entity.properties,&entity.extensions}) {
            std::vector<const Json*> pending{root};
            while (!pending.empty()) {
                const auto* value=pending.back();pending.pop_back();
                if (value->is_object()) {
                    if (value->contains("view_id") && value->at("view_id").is_string() &&
                        value->contains("overlay_id") && value->at("overlay_id").is_string()) {
                        auto owner=entity_id;
                        for (const auto* key:{"sheet_view_entity_id","sheet_view_id","entity_id"})
                            if (value->contains(key) && value->at(key).is_string()) { owner=value->at(key).get<std::string>();break; }
                        const auto found=retired.find({owner,value->at("view_id").get<std::string>()});
                        if (found!=retired.end() && found->second.contains(value->at("overlay_id").get<std::string>()))
                            invalid("retained qualified reference to a deleted wall overlay");
                    }
                    for (const auto& child:*value) pending.push_back(&child);
                } else if (value->is_array()) for (const auto& child:*value) pending.push_back(&child);
            }
        }
        if (entity.type!=kSheetViewEntityType) continue;
        for (auto view:entity.properties.at("model").at("views")) {
            const auto children=retired.find({entity_id,view.at("id").get<std::string>()});
            if (children==retired.end()) continue;
            view.erase("id");view.erase("object_ids");
            auto& presentation=view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row:presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& row:view.at("overlays")) {
                row.erase("id");row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                    row.at("dimension_binding").erase("object_id");
            }
            if (mentions(view,children->second)) invalid("saved view retains an opaque reference to a deleted wall overlay");
        }
    }
}
std::vector<EntityChange> complete_physical_wall_deletion_changes(const Entities& source,const std::vector<std::string>& wall_ids,
    bool complete_join_removal=false,const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities={},
    bool complete_opening_hosted_removal=false,bool complete_hosted_catalog_consequences=false) {
    if (complete_opening_hosted_removal && !complete_join_removal)
        invalid("opening-hosted completion requires hosted and join completion");
    if (!complete_join_removal && !additional_join_identities.empty()) invalid("fresh join identities require join completion");
    bound_complete_wall_deletion_source(source);
    if (wall_ids.empty() || wall_ids.size()>128) invalid("deletion requires one to 128 physical walls");
    const auto scope=constraint_phase_scope(source);
    const auto organization=organize_project(source);
    std::map<std::string,ModelPhases,std::less<>> phases;
    std::map<std::string,std::string,std::less<>> memberships;
    for (const auto& registry:scope.registries) {
        phases.emplace(registry.registry_id,ModelPhases::from_json(source.at(registry.registry_id).properties.at("model")));
        for (const auto& member:registry.registered_entity_ids)
            if (!memberships.emplace(member,registry.registry_id).second) invalid("overlapping complete deletion phase ownership");
    }
    std::set<std::string> sole_active_proposals,protected_carriers;
    if (complete_hosted_catalog_consequences) for (const auto& [registry,model]:phases) {
        (void)registry;
        if (!model.active_alternative()) continue;
        std::set<std::string> protected_proposals;
        for (const auto& alternative:model.alternatives()) {
            protected_carriers.insert(alternative.proposed_ids.begin(),alternative.proposed_ids.end());
            protected_carriers.insert(alternative.demolished_ids.begin(),alternative.demolished_ids.end());
            protected_proposals.insert(alternative.demolished_ids.begin(),alternative.demolished_ids.end());
            if (alternative.id==*model.active_alternative())
                sole_active_proposals.insert(alternative.proposed_ids.begin(),alternative.proposed_ids.end());
            else protected_proposals.insert(alternative.proposed_ids.begin(),alternative.proposed_ids.end());
        }
        for (const auto& owner:model.baseline_ids()) sole_active_proposals.erase(owner);
        for (const auto& owner:protected_proposals) sole_active_proposals.erase(owner);
    }
    std::set<std::string> retained_baseline_catalogs;
    const auto mutable_owner=[&](const std::string& owner) {
        const auto found=source.find(owner);
        if (found==source.end() || found->second.required) invalid("required or absent complete deletion owner: "+owner);
        const auto& entity=found->second;
        const bool scoped=entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node=organization.nodes.find(owner);
        if (entity.type=="assembly_model" || entity.type=="door" || entity.type=="window") {
            // Legacy host/catalog carriers are not placeable organization
            // nodes. Resolve their actual reference fields directly.
            for (const auto& [key,type]:{std::pair{"property_id","property"}, {"building_id","building"},
                {"floor_id","floor"}, {"layer_id","layer"}, {"wall_id","wall"}}) {
                if (!entity.properties.contains(key)) continue;
                const auto& reference=entity.properties.at(key);
                if (!reference.is_string()) invalid("malformed complete deletion carrier context: "+owner);
                const auto target=source.find(reference.get<std::string>());
                if (target==source.end() || target->second.type!=type)
                    invalid("unresolved complete deletion carrier context: "+owner);
            }
        } else if (scoped && (node==organization.nodes.end() || !node->second.issues.empty()))
            invalid("complete deletion owner has unresolved actual drawing context: "+owner);
        if (scope.inactive_owner_ids.contains(owner)) invalid("complete deletion owner is inactive: "+owner);
        const auto member=memberships.find(owner);if (member==memberships.end()) return;
        const auto& model=phases.at(member->second);
        const auto contains=[&](const auto& values) { return std::find(values.begin(),values.end(),owner)!=values.end(); };
        if (contains(model.baseline_ids())) {
            // Only a separately proved, physically retained catalog can borrow
            // row consequence authority. All preceding envelope/context,
            // required, inactive and actual-registry checks still apply.
            if (retained_baseline_catalogs.contains(owner)) return;
            if (model.active_alternative() || !model.alternatives().empty())
                invalid("shared baseline owner requires typed phase demolition: "+owner);
            return;
        }
        if (!model.active_alternative()) invalid("complete deletion owner does not participate in saved baseline: "+owner);
        std::size_t proposals{};
        for (const auto& alternative:model.alternatives()) {
            if (contains(alternative.demolished_ids)) invalid("complete deletion owner has protected demolition membership: "+owner);
            if (!contains(alternative.proposed_ids)) continue;
            ++proposals;
            if (alternative.id!=*model.active_alternative()) invalid("complete deletion owner belongs to another alternative: "+owner);
        }
        if (proposals!=1) invalid("complete deletion owner lacks sole saved proposal membership: "+owner);
    };
    std::set<std::string> roots,removed;
    for (const auto& wall_id:wall_ids) {
        id(wall_id);mutable_owner(wall_id);
        if (source.at(wall_id).type!="wall") invalid("complete deletion requires actual physical wall roots");
        if (!roots.insert(wall_id).second) invalid("duplicate wall deletion identity");
    }
    removed=roots;
    std::map<std::string,std::vector<const Entity*>,std::less<>> openings;
    for (const auto& [entity_id,entity]:source) {
        if (entity.type!="opening" && entity.type!="door" && entity.type!="window") continue;
        std::string host,error;
        if (!read_document_wall_id(entity,host,error)) invalid(error);
        if (!roots.contains(host)) continue;
        mutable_owner(entity_id);removed.insert(entity_id);openings[host].push_back(&entity);
        if (removed.size()>4096) invalid("complete deletion attached owner budget exceeded");
    }
    for (const auto& [entity_id,entity]:source) {
        bool attached=false;
        if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=decode_boundary_dimension_entity(entity);
            attached=decoded.supported() && removed.contains(decoded.dimension->boundary_id);
        } else if (entity.type=="constraint") {
            const auto decoded=decode_constraint_entity(entity);
            attached=decoded.supported() && std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                [&](const auto& binding){return removed.contains(binding.owner_id);});
            if (attached && !constraint_participates(*decoded.constraint,scope))
                invalid("attached constraint belongs to retained inactive geometry: "+entity_id);
        }
        if (attached) {
            mutable_owner(entity_id);removed.insert(entity_id);
            if (removed.size()>4096) invalid("complete deletion attached reference budget exceeded");
        }
    }
    std::map<std::string,AssemblyModel,std::less<>> catalogs;
    std::map<std::string,std::map<std::string,std::size_t,std::less<>>,std::less<>> instance_indices;
    std::map<std::string,std::set<std::string>,std::less<>> retired_rows;
    std::vector<std::pair<std::string,std::string>> components;
    bool opening_hosted_rows{};
    std::set<std::string> locals;
    const auto retained_baseline_catalog_row=[&](const Entity& catalog,const std::string& host_id) {
        if (!complete_hosted_catalog_consequences) return false;
        const auto carrier=memberships.find(catalog.id);
        if (carrier==memberships.end()) return false;
        const auto& model=phases.at(carrier->second);
        const auto contains=[](const auto& values,const std::string& owner) {
            return std::find(values.begin(),values.end(),owner)!=values.end();
        };
        if (!model.active_alternative() || !contains(model.baseline_ids(),catalog.id)) return false;
        if (catalog.required || scope.inactive_owner_ids.contains(catalog.id) || protected_carriers.contains(catalog.id))
            invalid("complete wall consequence requires an active nonrequired catalog: "+catalog.id);
        auto wall_id=host_id;
        const auto& host=source.at(host_id);
        if (host.type=="opening") {
            std::string error;
            if (!complete_opening_hosted_removal || !removed.contains(host_id) ||
                !read_document_wall_id(host,wall_id,error))
                invalid("complete wall catalog consequence lacks an actual removed semantic opening");
            const auto opening_member=memberships.find(host_id);
            if (opening_member!=memberships.end() && (opening_member->second!=carrier->second ||
                !sole_active_proposals.contains(host_id)))
                invalid("complete wall catalog consequence has foreign opening ownership: "+host_id);
        } else if (host.type!="wall") invalid("complete wall catalog consequence requires an actual wall host");
        const auto wall_member=memberships.find(wall_id);
        if (!roots.contains(wall_id) || wall_member==memberships.end() || wall_member->second!=carrier->second ||
            source.at(wall_id).type!="wall" || source.at(wall_id).required || scope.inactive_owner_ids.contains(wall_id))
            invalid("complete wall catalog consequence has absent, protected or foreign actual wall ownership: "+wall_id);
        if (!sole_active_proposals.contains(wall_id))
            invalid("complete wall catalog consequence requires actual proposed wall ownership: "+wall_id);
        return true;
    };
    for (const auto& [entity_id,entity]:source) if (entity.type=="assembly_model") {
        const auto& model=catalogs.emplace(entity_id,AssemblyModel::from_json(entity.properties.at("model"))).first->second;
        for (std::size_t index=0;index<model.instances().size();++index) {
            const auto& row=model.instances()[index];instance_indices[entity_id].emplace(row.id,index);
            if (!row.placement) continue;
            const auto& host_id=row.placement->host_entity_id;
            const auto host=source.find(host_id);
            const bool opening_host=complete_opening_hosted_removal && removed.contains(host_id) &&
                host!=source.end() && host->second.type=="opening";
            if (!roots.contains(host_id) && !opening_host) continue;
            opening_hosted_rows=opening_hosted_rows || opening_host;
            const bool retained_carrier=retained_baseline_catalog_row(entity,host_id);
            if (retained_carrier) retained_baseline_catalogs.insert(entity_id);
            mutable_owner(entity_id);
            if (!retained_carrier && memberships.contains(entity_id) && (!memberships.contains(host_id) ||
                memberships.at(entity_id)!=memberships.at(host_id)))
                invalid("hosted catalog and removed wall have foreign phase ownership");
            if (components.size()>=4096) invalid("complete deletion hosted instance budget exceeded");
            components.emplace_back(entity_id,row.id);locals.insert(row.id);retired_rows[entity_id].insert(row.id);
        }
    }
    std::sort(components.begin(),components.end());
    if (complete_join_removal) for (const auto& [catalog,model]:catalogs) for (const auto& row:model.instances()) {
        if (!row.placement) continue;
        const auto host=source.find(row.placement->host_entity_id);
        if (host!=source.end() && host->second.type=="wall_join")
            invalid("catalog placement on a wall join has no actual Document host contract: "+catalog+"/"+row.id);
    }
    const auto aliases=embedded_assembly_presentation_ids(source);
    auto cleanup_names=removed;
    for (const auto& key:components) cleanup_names.insert(aliases.at(key));
    // These aliases are derived presentation consequences of qualified rows,
    // never selectable wall roots or catalog retirement authority.
    for (const auto& [entity_id,entity]:source) if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded=decode_boundary_dimension_entity(entity);
        if (!decoded.supported() || !cleanup_names.contains(decoded.dimension->boundary_id)) continue;
        mutable_owner(entity_id);removed.insert(entity_id);cleanup_names.insert(entity_id);
        if (removed.size()>4096) invalid("complete deletion attached reference budget exceeded");
    }
    for (const auto& [entity_id,entity]:source) {
        (void)entity_id;
        if (unqualified_wall_component_reference(entity.properties,locals,source) ||
            unqualified_wall_component_reference(entity.extensions,locals,source))
            invalid("affected unqualified component reference has no retirement codec");
    }
    // Decode and bound all selected source geometry and expansion together,
    // before either producer can call native builders. Source hosts stay whole.
    std::size_t work{};std::map<std::string,std::size_t,std::less<>> costs;
    std::map<std::string,Wall,std::less<>> walls;
    struct OpeningSource {
        std::string wall_id;
        std::size_t index{};
        OpeningAssembly assembly;
        std::optional<DoorOperation> operation;
    };
    std::vector<OpeningSource> manufactured_openings;
    const auto add=[&](std::size_t count) {
        if (count>262144-work) invalid("aggregate complete wall/hosted geometry work budget exceeded");
        work+=count;
    };
    for (const auto& wall_id:roots) {
        Wall wall;std::string error;
        if (!read_document_wall(resolve_vertical_placement(source,source.at(wall_id)),openings[wall_id],wall,error)) invalid(error);
        if (wall.layers.size()>1024) invalid("complete deletion wall layer budget exceeded");
        validate_wall_semantics(wall);
        const auto cost=complete_join_removal ? (1+wall.layers.size())*(1+wall.openings.size()) :
            1+wall.layers.size()+wall.openings.size();
        costs.emplace(wall_id,cost);add(cost*3);
        const auto material=[&](const std::string& catalog,const std::string& local) {
            const auto found=catalogs.find(catalog);
            if (found==catalogs.end() || std::none_of(found->second.materials().begin(),found->second.materials().end(),
                [&](const auto& row){return row.id==local;})) invalid("removed wall material lacks actual catalog definition");
        };
        for (const auto& layer:wall.layers) if (layer.material) material(layer.material->catalog_id,layer.material->material_id);
        if (source.at(wall_id).properties.contains("material_assignment")) {
            const auto& assignment=source.at(wall_id).properties.at("material_assignment");
            if (!assignment.is_object() || !assignment.at("version").is_number_integer() || assignment.at("version")!=1)
                invalid("unsupported removed wall material assignment");
            material(assignment.at("catalog_id").get<std::string>(),assignment.at("material_id").get<std::string>());
        }
        for (std::size_t index=0;index<openings[wall_id].size();++index) {
            const auto& entity=*openings[wall_id][index];const auto& p=entity.properties;
            std::optional<OpeningAssembly> assembly;std::optional<DoorOperation> operation;
            if (p.contains("opening_kind")) {
                if (!p.at("opening_kind").is_string()) invalid("malformed removed opening family");
                const auto family=parse_opening_assembly_kind(p.at("opening_kind").get<std::string>());
                if (!family && p.at("opening_kind")!="opening") invalid("unsupported removed opening family");
                if (family) assembly=default_opening_assembly(*family);
            }
            if (p.contains("opening_assembly")) {
                if (entity.type!="opening") invalid("manufactured assembly requires an actual semantic opening");
                const auto actual=parse_opening_assembly(p.at("opening_assembly"));
                if (!assembly || assembly->kind!=actual.kind) invalid("removed opening assembly differs from its actual family");
                assembly=actual;
            }
            if (p.contains("door_operation")) {
                if (entity.type!="opening") invalid("door operation requires an actual semantic opening");
                operation=decode_door_operation(p.at("door_operation"));
                if (!assembly || assembly->kind!=OpeningAssemblyKind::door) invalid("door operation has a non-door opening family");
            }
            if (p.contains("material_assignment")) {
                const auto& assignment=p.at("material_assignment");
                if (!assignment.is_object() || !assignment.at("version").is_number_integer() || assignment.at("version")!=1)
                    invalid("unsupported removed opening material assignment");
                material(assignment.at("catalog_id").get<std::string>(),assignment.at("material_id").get<std::string>());
            }
            if (assembly) {
                // Current manufactured families have at most three panels and
                // a fixed frame; reserve their complete native construction.
                // Manufacture admission can rebuild the entire cut/layered
                // host and compare its parts. Charge that per opening in the
                // new lane; preserve the historical v35 work contract.
                add(complete_join_removal ? 32*(1+cost) : 32);
                manufactured_openings.push_back({wall_id,index,*assembly,operation});
            }
        }
        walls.emplace(wall_id,std::move(wall));
    }
    AssemblyExpansionBudget expansion_budget;
    for (const auto& [catalog,local]:components) {
        const auto& model=catalogs.at(catalog);
        const auto& row=model.instances().at(instance_indices.at(catalog).at(local));
        const auto expansion=model.expand(row,expansion_budget);
        // The opening-host leaf reserves its actual layered wall with every
        // active sibling cut, manufactured body and each legacy world copy.
        if (expansion.profiles.empty() && source.at(row.placement->host_entity_id).type!="opening")
            add(costs.at(row.placement->host_entity_id));
    }
    add(expansion_budget.consumed_nodes);add(expansion_budget.consumed_profile_segments);
    Entities join_candidate;
    if (complete_join_removal) {
        // Bound the combined hosted and join lane before either leaf enters
        // kernels. The join leaf independently admits the complete actual
        // source, including every original member and semantic opening.
        std::set<std::string> join_walls;
        for (const auto& [owner,entity]:source) if (entity.type=="wall_join" &&
            (mentions(entity.properties,roots) || mentions(entity.extensions,roots))) {
            const auto join=parse_wall_join(entity.properties,owner);
            if (join.wall_ids.size()>4096) invalid("complete deletion join member budget exceeded");
            add(2*join.wall_ids.size()*join.wall_ids.size());
            join_walls.insert(join.wall_ids.begin(),join.wall_ids.end());
        }
        if (join_walls.size()>4096) invalid("complete deletion joined wall expansion budget exceeded");
        std::map<std::string,std::size_t,std::less<>> joined_openings;
        for (const auto& [owner,entity]:source) if (entity.type=="opening" && !scope.inactive_owner_ids.contains(owner)) {
            std::string host,error;
            if (!read_document_wall_id(entity,host,error)) invalid(error);
            if (join_walls.contains(host)) ++joined_openings[host];
        }
        for (const auto& owner:join_walls) {
            const auto actual=source.find(owner);
            if (actual==source.end() || actual->second.type!="wall") invalid("joined member is not an actual wall");
            const auto& p=actual->second.properties;
            const auto layers=p.contains("layers") ? p.at("layers").size() : 0;
            if (layers>32 || joined_openings[owner]>128) invalid("complete deletion joined source geometry budget exceeded");
            add(4*(1+layers)*(1+joined_openings[owner]));
        }
        if (opening_hosted_rows || (complete_hosted_catalog_consequences && !components.empty()))
            preflight_architectural_object_removal(source,{},components,work,complete_hosted_catalog_consequences);
        join_candidate=replay_physical_wall_join_removal(source,wall_ids,additional_join_identities);
        for (const auto& [owner,entity]:source) if (entity.type=="wall_join" && !join_candidate.contains(owner)) {
            mutable_owner(owner);removed.insert(owner);cleanup_names.insert(owner);
        }
        // Only physically retired joins acquire removal authority. References
        // to retained/split join owners remain their exact original bytes.
        for (const auto& [owner,entity]:source) {
            bool attached=false;
            if (can_recognize_boundary_dimension_entity_type(entity.type)) {
                const auto decoded=decode_boundary_dimension_entity(entity);
                attached=decoded.supported() && removed.contains(decoded.dimension->boundary_id);
            } else if (entity.type=="constraint") {
                const auto decoded=decode_constraint_entity(entity);
                attached=decoded.supported() && std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                    [&](const auto& binding){return removed.contains(binding.owner_id);});
                if (attached && !constraint_participates(*decoded.constraint,scope))
                    invalid("retired join constraint belongs to retained inactive geometry: "+owner);
            }
            if (attached) { mutable_owner(owner);removed.insert(owner);cleanup_names.insert(owner); }
            if (removed.size()>4096) invalid("complete deletion joined reference budget exceeded");
        }
    }
    // Analytical preflight uses a complete source copy, never a native source.
    auto preflight=complete_join_removal ? join_candidate : source;
    for (const auto& [catalog,retired]:retired_rows) {
        auto& rows=preflight.at(catalog).properties.at("model").at("instances");
        rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row){return retired.contains(row.at("id").template get<std::string>());}),rows.end());
    }
    for (const auto& owner:removed) preflight.erase(owner);
    preflight=remove_known_object_memberships(std::move(preflight),cleanup_names,true);
    refuse_unresolved_wall_deletion_references(source,preflight,removed,true);
    refuse_retired_wall_overlay_references(source,preflight);
    const auto preserve_aliases=[&](const Entities& result) {
        const auto remaining=embedded_assembly_presentation_ids(result);
        for (const auto& [key,alias]:aliases) {
            const auto retired=retired_rows.find(key.first);
            if (retired!=retired_rows.end() && retired->second.contains(key.second)) continue;
            const auto found=remaining.find(key);
            if (found==remaining.end() || found->second!=alias)
                invalid("complete wall deletion changes a surviving component presentation alias");
        }
    };
    const auto preserve_retained_catalogs=[&](const Entities& result) {
        if (retained_baseline_catalogs.empty()) return;
        for (const auto& catalog:retained_baseline_catalogs) {
            const auto after=result.find(catalog);
            if (after==result.end()) invalid("complete wall consequence erased a retained catalog: "+catalog);
            auto expected=source.at(catalog);
            auto& rows=expected.properties.at("model").at("instances");
            const auto& retired=retired_rows.at(catalog);
            rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
                return retired.contains(row.at("id").template get<std::string>());
            }),rows.end());
            if (after->second!=expected || after->second.properties.dump()!=expected.properties.dump() ||
                after->second.extensions.dump()!=expected.extensions.dump())
                invalid("complete wall consequence changed raw retained catalog beyond admitted rows: "+catalog);
        }
        // Compare the actual raw membership projection for all retained
        // carriers together. Other owners may retire; carrier registration,
        // saved choice and every alternative membership remain exact.
        const auto membership_projection=[&](const Json& raw) {
            Json result=raw;
            const auto keep=[&](Json& values) {
                values.erase(std::remove_if(values.begin(),values.end(),[&](const auto& value) {
                    return !retained_baseline_catalogs.contains(value.template get<std::string>());
                }),values.end());
            };
            keep(result.at("entity_ids"));keep(result.at("baseline_ids"));
            for (auto& alternative:result.at("alternatives")) {
                keep(alternative.at("proposed_ids"));keep(alternative.at("demolished_ids"));
            }
            return result;
        };
        for (const auto& [registry,model]:phases) {
            (void)model;
            const auto after=result.find(registry);
            if (after==result.end() || after->second.type!="model_phases" ||
                membership_projection(source.at(registry).properties.at("model")).dump()!=
                    membership_projection(after->second.properties.at("model")).dump())
                invalid("complete wall consequence changed retained catalog phase membership: "+registry);
        }
    };
    preserve_aliases(preflight);
    preserve_retained_catalogs(preflight);
    for (const auto& [entity_id,entity]:source) {
        const auto after=preflight.find(entity_id);
        if (after==preflight.end() || entity!=after->second || entity.properties.dump()!=after->second.properties.dump() ||
            entity.extensions.dump()!=after->second.extensions.dump()) mutable_owner(entity_id);
    }
    auto candidate=components.empty() ? source : replay_architectural_object_removal(source,{},components,
        opening_hosted_rows,opening_hosted_rows ? work : 0,complete_hosted_catalog_consequences);
    if (complete_join_removal) candidate=components.empty() ? join_candidate :
        compose_ordinary_architectural_removal_candidates(source,{join_candidate,candidate},
            complete_hosted_catalog_consequences,complete_hosted_catalog_consequences);
    for (const auto& owner:removed) candidate.erase(owner);
    candidate=remove_known_object_memberships(std::move(candidate),removed,true);
    refuse_unresolved_wall_deletion_references(source,candidate,removed,true);
    refuse_retired_wall_overlay_references(source,candidate);
    preserve_aliases(candidate);
    preserve_retained_catalogs(candidate);
    bound_complete_wall_deletion_source(candidate);
    if (constraint_phase_scope(candidate).inactive_owner_ids!=scope.inactive_owner_ids)
        invalid("complete deletion changes retained inactive ownership");
    std::vector<EntityChange> changes;
    for (const auto& [entity_id,entity]:source) {
        const auto after=candidate.find(entity_id);
        const bool changed=after==candidate.end() || entity!=after->second ||
            entity.properties.dump()!=after->second.properties.dump() || entity.extensions.dump()!=after->second.extensions.dump();
        if (!changed) continue;
        mutable_owner(entity_id);
        if (after==candidate.end()) changes.push_back(EntityChange::erase(entity_id));
        else changes.push_back(EntityChange::upsert(after->second));
    }
    if (complete_join_removal) for (const auto& [owner,entity]:candidate) if (!source.contains(owner)) {
        if (entity.type!="wall_join") invalid("complete deletion created an unrelated source owner");
        changes.push_back(EntityChange::upsert(entity));
    }
    for (const auto& [wall_id,wall]:walls) { (void)wall_id;(void)make_wall(wall); }
    for (const auto& opening:manufactured_openings) {
        const auto& wall=walls.at(opening.wall_id);
        (void)make_opening_assembly_geometry(wall,wall.openings.at(opening.index),opening.assembly,opening.operation);
    }
    return changes;
}
void require_room_review_batch_size(std::size_t count) {
    if (count<2 || count>32) invalid("batch review requires two to thirty-two explicit intents");
}
struct RoomReviewBatchGuard {
    std::vector<std::pair<DrawingContext,double>> reviewed_planes;
    std::set<std::string> reviewed_rooms;
    std::set<std::string> used_tokens;

    explicit RoomReviewBatchGuard(const Entities& source) {
        // Keep these tokens reserved after retirement or replacement. A later
        // stage must not borrow an identity that vanished from its entity map.
        for (const auto& [entity_id,entity]:source) {
            used_tokens.insert(entity_id);
            if (!can_recognize_boundary_entity_type(entity.type) ||
                inspect_boundary_entity_version(entity).format!=BoundaryEntityFormat::identified_v1) continue;
            for (const auto& edge:decode_identified_boundary_entity(entity).segments) {
                used_tokens.insert(edge.segment_id);
                used_tokens.insert(edge.start_vertex_id);
                used_tokens.insert(edge.end_vertex_id);
            }
        }
    }
    void admit(const PhysicalWallRoomReviewIntent& intent) {
        for (const auto& [context,elevation]:reviewed_planes)
            if (context==intent.context &&
                std::abs(elevation-intent.effective_elevation_m)<=default_geometry_tolerance_metres)
                invalid("batch contains duplicate drawing context and effective plane");
        reviewed_planes.emplace_back(intent.context,intent.effective_elevation_m);
        for (const auto& decision:intent.retained)
            if (!reviewed_rooms.insert(decision.room_id).second)
                invalid("batch retained room decisions overlap");
        const auto reserve=[&](const std::string& token) {
            if (!used_tokens.insert(token).second)
                invalid("batch fresh identity was already occupied, created or retired: "+token);
        };
        for (const auto& decision:intent.fresh) {
            if (decision.disposition==PhysicalWallRoomFreshDisposition::unclassified) continue;
            if (decision.disposition==PhysicalWallRoomFreshDisposition::create) reserve(decision.room_id);
            for (const auto& token:decision.fresh_ids.segment_ids) reserve(token);
            for (const auto& token:decision.fresh_ids.vertex_ids) reserve(token);
        }
        for (const auto& decision:intent.retained)
            for (const auto& token:decision.replacement_dimension_ids) reserve(token);
    }
};
} // namespace

PhysicalWallRoomReviewIntent decode_physical_wall_room_review_intent(const Json& value) {
    try {
        if (value.dump().size()>16*1024*1024) invalid("intent exceeds evidence budget");
        if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
            (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3))
            invalid("unsupported intent version");
        const bool active_scope=value.at("version")==3;
        if (active_scope)
            keys(value,{"version","selected_wall_id","source_snapshot_digest","source_authoring_digest","source_saved_revision","source_entities_digest","context","effective_elevation_m",
                "retained","fresh","removed_reference_ids","kept_reference_ids","relationship_removals","active_phase_room_scope","context_plane_selection"});
        else
            keys(value,{"version","selected_wall_id","source_snapshot_digest","source_authoring_digest","source_saved_revision","source_entities_digest","context","effective_elevation_m",
                "retained","fresh","removed_reference_ids","kept_reference_ids","relationship_removals"});
        PhysicalWallRoomReviewIntent result;
        result.active_phase_room_scope=active_scope;
        if (active_scope) {
            if (!value.at("active_phase_room_scope").is_boolean() || value.at("active_phase_room_scope")!=true ||
                !value.at("context_plane_selection").is_boolean()) invalid("version three requires explicit ordinary active room scope and selection mode");
            result.context_plane_selection=value.at("context_plane_selection").get<bool>();
        } else result.context_plane_selection=value.at("version")==2;
        result.selected_wall_id=value.at("selected_wall_id").get<std::string>();
        if (result.context_plane_selection) {
            if (!result.selected_wall_id.empty()) invalid("context/plane intent cannot contain a selected wall");
        } else id(result.selected_wall_id);
        result.source_snapshot_digest=value.at("source_snapshot_digest").get<std::string>();digest(result.source_snapshot_digest);
        result.source_authoring_digest=value.at("source_authoring_digest").get<std::string>();digest(result.source_authoring_digest);
        const auto& saved=value.at("source_saved_revision");
        if (!saved.is_null()) {
            if ((!saved.is_number_integer() && !saved.is_number_unsigned()) ||
                (saved.is_number_integer() && saved.get<std::int64_t>()<0)) invalid("invalid captured save revision");
            result.source_saved_revision=saved.get<Revision>();
        }
        result.source_entities_digest=value.at("source_entities_digest").get<std::string>();digest(result.source_entities_digest);
        result.context=context(value.at("context"));if (!result.context.complete()) invalid("incomplete drawing context");
        if (!value.at("effective_elevation_m").is_number()) invalid("invalid elevation");
        result.effective_elevation_m=value.at("effective_elevation_m").get<double>();if (!std::isfinite(result.effective_elevation_m)) invalid("nonfinite elevation");
        for (const auto* name:{"retained","fresh","removed_reference_ids","kept_reference_ids","relationship_removals"})
            if (!value.at(name).is_array() || value.at(name).size()>2048) invalid("decision collection exceeds budget or is malformed");
        for (const auto& row:value.at("retained")) {
            keys(row,{"room_id","expected_descriptor_digest","disposition","child_mapping","replacement_dimension_ids"});
            PhysicalWallRoomRetainedDecision d;d.room_id=row.at("room_id").get<std::string>();id(d.room_id);
            d.expected_descriptor_digest=row.at("expected_descriptor_digest").get<std::string>();digest(d.expected_descriptor_digest);
            const auto disposition=row.at("disposition").get<std::string>();
            if (disposition=="retire") d.disposition=PhysicalWallRoomRetainedDisposition::retire;
            else if (disposition!="retain") invalid("unsupported retained disposition");
            d.child_mapping=row.at("child_mapping");if (!d.child_mapping.is_object()) invalid("malformed child mapping");
            d.replacement_dimension_ids=row.at("replacement_dimension_ids").get<std::vector<std::string>>();ids(d.replacement_dimension_ids);
            if (d.disposition==PhysicalWallRoomRetainedDisposition::retire && !d.child_mapping.empty()) invalid("retirement cannot map children");
            if (d.disposition==PhysicalWallRoomRetainedDisposition::retire && !d.replacement_dimension_ids.empty()) invalid("retirement cannot regenerate dimensions");
            result.retained.push_back(std::move(d));
        }
        for (const auto& row:value.at("fresh")) {
            keys(row,{"candidate_index","reviewed_source_lineage","disposition","room_id","interior_witness","segment_ids","vertex_ids","name","classification","context"});
            PhysicalWallRoomFreshDecision d;
            if (!row.at("candidate_index").is_number_integer() || row.at("candidate_index")<0 || row.at("candidate_index")>=2048) invalid("invalid candidate index");
            d.candidate_index=row.at("candidate_index").get<std::size_t>();d.reviewed_source_lineage=row.at("reviewed_source_lineage");
            if (!d.reviewed_source_lineage.is_object()) invalid("malformed reviewed lineage");
            const auto disposition=row.at("disposition").get<std::string>();
            if (disposition=="retained") d.disposition=PhysicalWallRoomFreshDisposition::retained;
            else if (disposition=="create") d.disposition=PhysicalWallRoomFreshDisposition::create;
            else if (disposition!="unclassified") invalid("unsupported fresh disposition");
            d.room_id=row.at("room_id").get<std::string>();
            const auto& point=row.at("interior_witness");if (!point.is_array() || point.size()!=2 || !point[0].is_number() || !point[1].is_number()) invalid("malformed witness");
            d.interior_witness={point[0].get<double>(),point[1].get<double>()};
            if (!std::isfinite(d.interior_witness.x) || !std::isfinite(d.interior_witness.y)) invalid("nonfinite witness");
            d.fresh_ids.segment_ids=row.at("segment_ids").get<std::vector<std::string>>();ids(d.fresh_ids.segment_ids);
            d.fresh_ids.vertex_ids=row.at("vertex_ids").get<std::vector<std::string>>();ids(d.fresh_ids.vertex_ids);
            d.name=row.at("name").get<std::string>();d.classification=row.at("classification").get<std::string>();d.context=context(row.at("context"));
            if (d.disposition==PhysicalWallRoomFreshDisposition::unclassified) {
                if (!d.room_id.empty() || !d.fresh_ids.segment_ids.empty() || !d.fresh_ids.vertex_ids.empty()) invalid("unclassified space has identity decisions");
            } else id(d.room_id);
            if (d.disposition==PhysicalWallRoomFreshDisposition::create) {
                const auto text=[](const std::string& s) { return !s.empty() && s.size()<=4096 && s.find_first_not_of(" \t\r\n")!=std::string::npos; };
                if (!text(d.name) || !text(d.classification) || d.context!=result.context) invalid("creation requires explicit name, classification and matching context");
            } else if (!d.name.empty() || !d.classification.empty() || d.context!=DrawingContext{}) invalid("noncreation decision cannot transfer metadata");
            result.fresh.push_back(std::move(d));
        }
        result.removed_reference_ids=value.at("removed_reference_ids").get<std::vector<std::string>>();ids(result.removed_reference_ids);
        result.kept_reference_ids=value.at("kept_reference_ids").get<std::vector<std::string>>();ids(result.kept_reference_ids);
        for (const auto& kept:result.kept_reference_ids)
            if (std::find(result.removed_reference_ids.begin(),result.removed_reference_ids.end(),kept)!=result.removed_reference_ids.end())
                invalid("reference cannot be both kept and removed");
        for (const auto& row:value.at("relationship_removals")) {
            keys(row,{"entity_id","removed_room_ids","acknowledged_relations"});
            PhysicalWallRoomRelationshipRemoval d;d.entity_id=row.at("entity_id").get<std::string>();id(d.entity_id);
            d.removed_room_ids=row.at("removed_room_ids").get<std::vector<std::string>>();ids(d.removed_room_ids);
            if (d.removed_room_ids.empty() || !row.at("acknowledged_relations").is_array() || row.at("acknowledged_relations").size()>65536) invalid("invalid graph removal decisions");
            std::set<std::string> unique;
            for (const auto& r:row.at("acknowledged_relations")) {
                if (!unique.insert(r.dump()).second) invalid("duplicate graph relation acknowledgement");
                d.acknowledged_relations.push_back(relation(r));
            }
            result.relationship_removals.push_back(std::move(d));
        }
        return result;
    } catch (const Json::exception&) { invalid("malformed intent value types"); }
}

Json encode_physical_wall_room_review_intent(const PhysicalWallRoomReviewIntent& intent) {
    Json retained=Json::array(),fresh=Json::array(),relationships=Json::array();
    for (const auto& d:intent.retained) {
        std::string action;
        if (d.disposition==PhysicalWallRoomRetainedDisposition::retain) action="retain";
        else if (d.disposition==PhysicalWallRoomRetainedDisposition::retire) action="retire";
        else invalid("unsupported retained disposition");
        retained.push_back({{"room_id",d.room_id},{"expected_descriptor_digest",d.expected_descriptor_digest},{"disposition",action},{"child_mapping",d.child_mapping},
            {"replacement_dimension_ids",d.replacement_dimension_ids}});
    }
    for (const auto& d:intent.fresh) {
        std::string action;
        if (d.disposition==PhysicalWallRoomFreshDisposition::retained) action="retained";
        else if (d.disposition==PhysicalWallRoomFreshDisposition::create) action="create";
        else if (d.disposition==PhysicalWallRoomFreshDisposition::unclassified) action="unclassified";
        else invalid("unsupported fresh disposition");
        fresh.push_back({{"candidate_index",d.candidate_index},{"reviewed_source_lineage",d.reviewed_source_lineage},{"disposition",action},
            {"room_id",d.room_id},{"interior_witness",{d.interior_witness.x,d.interior_witness.y}},
            {"segment_ids",d.fresh_ids.segment_ids},{"vertex_ids",d.fresh_ids.vertex_ids},{"name",d.name},{"classification",d.classification},{"context",context_json(d.context)}});
    }
    for (const auto& d:intent.relationship_removals) {
        Json rows=Json::array();for (const auto& r:d.acknowledged_relations) rows.push_back(relation_json(r));
        relationships.push_back({{"entity_id",d.entity_id},{"removed_room_ids",d.removed_room_ids},{"acknowledged_relations",std::move(rows)}});
    }
    Json result{{"version",intent.active_phase_room_scope ? 3 : (intent.context_plane_selection ? 2 : 1)},{"selected_wall_id",intent.selected_wall_id},{"source_snapshot_digest",intent.source_snapshot_digest},
        {"source_authoring_digest",intent.source_authoring_digest},
        {"source_saved_revision",intent.source_saved_revision ? Json(*intent.source_saved_revision) : Json(nullptr)},
        {"source_entities_digest",intent.source_entities_digest},{"context",context_json(intent.context)},{"effective_elevation_m",intent.effective_elevation_m},
        {"retained",std::move(retained)},{"fresh",std::move(fresh)},{"removed_reference_ids",intent.removed_reference_ids},
        {"kept_reference_ids",intent.kept_reference_ids},{"relationship_removals",std::move(relationships)}};
    if (intent.active_phase_room_scope) {
        result["active_phase_room_scope"]=true;
        result["context_plane_selection"]=intent.context_plane_selection;
    }
    (void)decode_physical_wall_room_review_intent(result);return result;
}

ReplayedPhysicalWallRoomReview replay_physical_wall_room_review(const Entities& source,const Json& encoded,
    bool active_phase_constraints) {
    const auto intent=decode_physical_wall_room_review_intent(encoded);
    if (entity_map_digest(source)!=intent.source_entities_digest) invalid("preceding entity map differs from reviewed source");
    const auto constraint_scope=active_phase_constraints
        ? std::optional<ConstraintPhaseScope>{constraint_phase_scope(source)} : std::nullopt;
    const auto detection=intent.context_plane_selection
        ? detect_physical_wall_spaces(source,intent.context,intent.effective_elevation_m)
        : detect_physical_wall_spaces(source,intent.selected_wall_id);
    if (detection.context!=intent.context) invalid("source drawing context changed");
    double elevation=intent.effective_elevation_m;
    if (!intent.context_plane_selection) {
        const auto selected=resolve_vertical_placement(source,source.at(intent.selected_wall_id));
        elevation=selected.properties.at("elevation_m").get<double>();
        if (elevation!=intent.effective_elevation_m) invalid("source effective plane changed");
    }
    const auto organization=organize_project(source);const auto current_owners=current_source_owners(detection,intent.selected_wall_id);
    std::optional<std::set<std::string,std::less<>>> active_rooms;
    std::map<std::string,std::string,std::less<>> wall_registries;
    if (intent.active_phase_room_scope) {
        const auto admitted=active_physical_wall_room_ids(source);
        active_rooms.emplace(admitted.begin(),admitted.end());
        if (std::none_of(source.begin(),source.end(),[](const auto& entry){return entry.second.type=="model_phases";}))
            invalid("active room scope requires actual saved phase authority");
        // The shared active roster already admitted every registry/member and
        // unique physical ownership before this index is constructed.
        for (const auto& [registry_id,registry]:source) {
            if (registry.type!="model_phases") continue;
            const auto model=ModelPhases::from_json(registry.properties.at("model"));
            for (const auto& member:model.entity_ids()) if (source.at(member).type=="wall")
                wall_registries.emplace(member,registry_id);
        }
    }
    std::set<std::string> expected_rooms,retiring,affected_tokens,occupied;
    std::size_t retained_bytes=0,retained_contacts=0;
    for (const auto& [entity_id,e]:source) {
        occupied.insert(entity_id);
        if (can_recognize_boundary_entity_type(e.type) && inspect_boundary_entity_version(e).format==BoundaryEntityFormat::identified_v1) {
            for (const auto& edge:decode_identified_boundary_entity(e).segments) {
                occupied.insert(edge.segment_id);occupied.insert(edge.start_vertex_id);occupied.insert(edge.end_vertex_id);
            }
        }
        if (!is_physical_wall_room(e)) continue;
        // Older intents deliberately keep their original collect-all dialect.
        // Version three excludes inactive originals before context, descriptor
        // or plane evidence admission and cannot assign or retire those owners.
        if (active_rooms && !active_rooms->contains(entity_id)) continue;
        const auto c=organization.drawing_context(entity_id);
        if (!c || !c->complete()) invalid("retained room has unresolved drawing context");
        if (*c!=intent.context) continue;
        const auto descriptor=decode_physical_wall_room_descriptor(e);
        const auto bytes=e.properties.dump().size()+e.extensions.dump().size();
        if (bytes>16*1024*1024-retained_bytes) invalid("retained evidence exceeds aggregate budget");
        retained_bytes+=bytes;
        std::size_t edge_count=decode_identified_boundary_entity(e).segments.size();
        for (const auto& hole:descriptor.holes) edge_count+=hole.size();
        if (edge_count>16384) invalid("retained geometry exceeds edge budget");
        const auto contacts=edge_count ? edge_count*(edge_count-1) : 0;
        if (contacts>65536-retained_contacts) invalid("retained geometry exceeds aggregate contact budget");
        retained_contacts+=contacts;
        const auto captured=validate_retained_physical_wall_room_lineage(e,*c);
        if (std::abs(captured.effective_elevation_m-elevation)>default_geometry_tolerance_metres) {
            if (std::any_of(captured.source_owner_ids.begin(),captured.source_owner_ids.end(),[&](const auto& v){return current_owners.contains(v);}))
                invalid("surviving source identity moved between effective planes");
            continue;
        }
        expected_rooms.insert(entity_id);
        for (const auto& edge:decode_identified_boundary_entity(e).segments) {
            affected_tokens.insert(edge.segment_id);affected_tokens.insert(edge.start_vertex_id);affected_tokens.insert(edge.end_vertex_id);
        }
    }
    if (expected_rooms.size()>2048 || detection.spaces.size()>2048) invalid("room review exceeds budget");
    validate_active(source,expected_rooms);
    std::map<std::string,const PhysicalWallRoomRetainedDecision*,std::less<>> old;
    for (const auto& d:intent.retained) {
        if (!expected_rooms.contains(d.room_id) || !old.emplace(d.room_id,&d).second) invalid("duplicate or out-of-context retained decision");
        if (physical_wall_room_descriptor_digest(source.at(d.room_id))!=d.expected_descriptor_digest) invalid("retained descriptor changed");
        if (d.disposition==PhysicalWallRoomRetainedDisposition::retire) { retiring.insert(d.room_id);affected_tokens.insert(d.room_id); }
    }
    if (old.size()!=expected_rooms.size()) invalid("every retained room requires an explicit complete disposition");
    std::map<std::size_t,const PhysicalWallRoomFreshDecision*> fresh;
    std::set<std::string> assigned,retained_assigned;
    for (const auto& d:intent.fresh) {
        if (d.candidate_index>=detection.spaces.size() || !fresh.emplace(d.candidate_index,&d).second) invalid("duplicate or unknown fresh space decision");
        const auto& space=detection.spaces[d.candidate_index];
        if (d.reviewed_source_lineage!=space.source_lineage) invalid("reviewed fresh lineage is no longer exact");
        if (d.disposition==PhysicalWallRoomFreshDisposition::unclassified) continue;
        if (!assigned.insert(d.room_id).second) invalid("one room identity cannot own multiple fresh spaces");
        if (d.disposition==PhysicalWallRoomFreshDisposition::retained) {
            if (!old.contains(d.room_id) || old.at(d.room_id)->disposition!=PhysicalWallRoomRetainedDisposition::retain) invalid("fresh assignment lacks matching retained decision");
            retained_assigned.insert(d.room_id);
        } else if (!occupied.insert(d.room_id).second) invalid("new room identity is already occupied");
        witness(space,d.interior_witness);(void)identified(space,d);
        for (const auto* group:{&d.fresh_ids.segment_ids,&d.fresh_ids.vertex_ids}) for (const auto& child:*group)
            if (!occupied.insert(child).second) invalid("fresh child identity is already occupied");
    }
    if (fresh.size()!=detection.spaces.size()) invalid("every fresh space requires an explicit complete disposition");
    for (const auto& [room_id,d]:old) if (d->disposition==PhysicalWallRoomRetainedDisposition::retain && !retained_assigned.contains(room_id))
        invalid("retained identity has no explicitly assigned fresh space");
    Entities result=source;std::vector<BoundaryGeometryEdit> retained_edits;std::vector<std::string> created_room_ids;
    std::set<std::string> removed(intent.removed_reference_ids.begin(),intent.removed_reference_ids.end());
    const std::set<std::string> kept(intent.kept_reference_ids.begin(),intent.kept_reference_ids.end());
    std::set<std::string> expected_references,preserved_constraints;
    for (const auto& [reference_id,e]:source) {
        if (can_recognize_boundary_dimension_entity_type(e.type)) {
            const auto decoded=decode_boundary_dimension_entity(e);
            if (decoded.supported() && expected_rooms.contains(decoded.dimension->boundary_id)) expected_references.insert(reference_id);
        } else if (e.type=="constraint") {
            const auto decoded=decode_constraint_entity(e);
            if (decoded.supported() && std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                [&](const auto& binding){return expected_rooms.contains(binding.owner_id);})) {
                expected_references.insert(reference_id);
                if (constraint_scope && !constraint_participates(*decoded.constraint,*constraint_scope))
                    preserved_constraints.insert(reference_id);
            }
        }
    }
    auto reviewed_references=removed;
    reviewed_references.insert(kept.begin(),kept.end());
    if (reviewed_references!=expected_references) invalid("every affected reference requires an explicit Keep or Remove decision");
    for (const auto& reference_id:preserved_constraints)
        if (removed.contains(reference_id)) invalid("constraint attached to an inactive design must remain unchanged: "+reference_id);
    // Include every identity that this review can retire, not only room and
    // boundary-child IDs. Unknown incoming references must never be stranded.
    affected_tokens.insert(removed.begin(),removed.end());
    for (const auto& [reference_id,e]:source) {
        if (!kept.contains(reference_id) || !can_recognize_boundary_dimension_entity_type(e.type)) continue;
        const auto decoded=decode_boundary_dimension_entity(e);
        if (!decoded.supported()) continue;
        const auto decision=old.find(decoded.dimension->boundary_id);
        if (decision!=old.end() && decision->second->disposition==PhysicalWallRoomRetainedDisposition::retain &&
            decoded.dimension->kind==BoundaryDimensionKind::segment_length &&
            decoded.dimension->placement==BoundaryDimensionPlacement::automatic)
            affected_tokens.insert(reference_id);
    }
    for (const auto& [room_id,d]:old) {
        (void)room_id;
        for (const auto& id:d->replacement_dimension_ids)
            if (!occupied.insert(id).second) invalid("replacement dimension identity is already occupied");
    }
    for (const auto& reference_id:removed) {
        const auto found=source.find(reference_id);if (found==source.end()) invalid("removed reference is missing");
        bool affected=false;
        if (can_recognize_boundary_dimension_entity_type(found->second.type)) {
            const auto decoded=decode_boundary_dimension_entity(found->second);
            if (!decoded.supported()) invalid("unknown dimension cannot be removed by room review");
            affected=expected_rooms.contains(decoded.dimension->boundary_id);
        } else if (found->second.type=="constraint") {
            const auto decoded=decode_constraint_entity(found->second);
            if (!decoded.supported()) invalid("unknown constraint cannot be removed by room review");
            affected=std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),[&](const auto& b){return expected_rooms.contains(b.owner_id);});
        }
        if (!affected) invalid("removal must name a supported affected dimension or endpoint constraint");
        result.erase(reference_id);
    }
    std::set<std::string> graph_decisions;
    for (const auto& d:intent.relationship_removals) {
        if (!graph_decisions.insert(d.entity_id).second) invalid("duplicate relationship graph decision");
        const auto found=result.find(d.entity_id);if (found==result.end() || found->second.type!="room_relationships") invalid("graph removal target is not a relationship model");
        const auto& model=found->second.properties.at("model");
        if (room_relationship_model_version(model)>2) invalid("unknown relationship model cannot be rewritten");
        const auto graph=RoomRelationshipSnapshot::from_json(model);
        std::set<std::string> remove_rooms(d.removed_room_ids.begin(),d.removed_room_ids.end()),present;
        for (const auto& r:graph.references()) if (remove_rooms.contains(r.id)) present.insert(r.id);
        if (present!=remove_rooms || !std::all_of(remove_rooms.begin(),remove_rooms.end(),[&](const auto& v){return retiring.contains(v);})) invalid("graph removals must name present retired room identities");
        std::vector<RoomReference> references;std::vector<RoomRelation> relations;std::set<std::string> incident,acknowledged;
        for (const auto& r:graph.references()) if (!remove_rooms.contains(r.id)) references.push_back(r);
        for (const auto& r:graph.relations()) {
            if (remove_rooms.contains(r.source_id) || remove_rooms.contains(r.target_id)) incident.insert(relation_json(r).dump());
            else relations.push_back(r);
        }
        for (const auto& r:d.acknowledged_relations) acknowledged.insert(relation_json(r).dump());
        if (incident!=acknowledged) invalid("every removed relationship row requires exact acknowledgement");
        auto properties=found->second.properties;properties.erase("model");
        if (mentions(properties,affected_tokens) || mentions(found->second.extensions,affected_tokens)) invalid("unsupported relationship metadata reference");
        found->second.properties["model"]=RoomRelationshipSnapshot::create(std::move(references),std::move(relations)).to_json();
    }
    if (intent.context_plane_selection || intent.active_phase_room_scope) {
        // Explicit retirement removes known live memberships. Original
        // evidence and opaque metadata remain for the reference scan below.
        auto retired_members=retiring;retired_members.insert(removed.begin(),removed.end());
        result=remove_known_object_memberships(std::move(result),retired_members);
    }
    for (const auto& [entity_id,e]:result) {
        if (expected_rooms.contains(entity_id)) {
            if (!retiring.contains(entity_id)) {
                auto opaque_properties=e.properties;
                if (opaque_properties.contains("segments")) {
                    for (auto segment:opaque_properties.at("segments")) {
                        for (const auto* key:{"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"})
                            segment.erase(key);
                        if (mentions(segment,affected_tokens)) invalid("retained owner has unsupported segment metadata reference");
                    }
                }
                opaque_properties.erase("segments");
                // These validated receipts deliberately preserve historical
                // analytical identities; they are not live opaque references.
                opaque_properties.erase("boundary_authoring");
                auto opaque_extensions=e.extensions;
                opaque_extensions.erase("boundary_geometry_derivation");
                opaque_extensions.erase("physical_wall_room");
                if (mentions(opaque_properties,affected_tokens) || mentions(opaque_extensions,affected_tokens))
                    invalid("retained owner has unsupported metadata reference to a retired identity");
            }
            continue;
        }
        if (can_recognize_boundary_dimension_entity_type(e.type)) {
            const auto d=decode_boundary_dimension_entity(e);
            if (!d.supported()) { if (mentions(e.properties,affected_tokens) || mentions(e.extensions,affected_tokens)) invalid("unknown affected dimension");continue; }
            if (retiring.contains(d.dimension->boundary_id)) invalid("retired room dimension needs explicit removal");
            if (mentions(e.properties,retiring)) invalid("unsupported dimension property reference to a retired room");
            if (mentions(e.extensions,affected_tokens)) invalid("unsupported dimension extension reference");
            auto extra=e.properties;
            if (extra.contains("target") && extra.at("target").is_object()) {
                for (const auto* field:{"entity_id","segment_id","segment_ids","second_segment_id","vertex_id"}) extra["target"].erase(field);
            }
            extra.erase("boundary_id");
            if (mentions(extra,affected_tokens)) invalid("unsupported dimension metadata reference to a retired child");
            continue;
        }
        if (e.type=="constraint") {
            const auto d=decode_constraint_entity(e);
            if (!d.supported()) { if (mentions(e.properties,affected_tokens) || mentions(e.extensions,affected_tokens)) invalid("unknown affected constraint");continue; }
            if (std::any_of(d.constraint->bindings.begin(),d.constraint->bindings.end(),[&](const auto& b){return retiring.contains(b.owner_id);})) invalid("retired room constraint needs explicit removal");
            if (mentions(e.properties,retiring)) invalid("unsupported constraint property reference to a retired room");
            if (mentions(e.extensions,affected_tokens)) invalid("unsupported constraint extension reference");
            auto extra=e.properties;extra.erase("entity_ids");extra.erase("wall_ids");
            if (extra.contains("bindings") && extra.at("bindings").is_array()) for (auto& binding:extra["bindings"])
                for (const auto* field:{"owner_id","feature","segment_id","vertex_id","role"}) binding.erase(field);
            if (mentions(extra,affected_tokens)) invalid("unsupported constraint metadata reference to a retired child");
            continue;
        }
        if (mentions(e.properties,affected_tokens) || mentions(e.extensions,affected_tokens)) invalid("unsupported dependent reference in "+entity_id);
    }
    // Retire all reviewed old owners before lower retained repair admission, so
    // a deliberately merged destination cannot conflict with a retiring owner.
    for (const auto& room_id:retiring) result.erase(room_id);
    std::set<std::string> retained_owners;
    for (const auto& [room_id,d]:old)
        if (d->disposition==PhysicalWallRoomRetainedDisposition::retain) retained_owners.insert(room_id);
    std::map<std::string,PhysicalWallSpaces,std::less<>> seeded_detections;
    std::map<std::string,std::vector<std::string>,std::less<>> created_by_registry;
    const auto source_owner=[&](const PhysicalWallSpace& space) {
        if (!intent.context_plane_selection) return intent.selected_wall_id;
        // Descriptors retain a real active source owner, never the deleted wall
        // or an empty selection. The fresh detector admitted this inventory.
        const auto owner=space.source_lineage.at("physical_sources").at(0).at("owner_id").get<std::string>();
        id(owner);
        auto cached=seeded_detections.find(owner);
        if (cached==seeded_detections.end()) {
            const auto found=source.find(owner);
            if (found==source.end() || found->second.id!=owner || found->second.type!="wall" ||
                organization.drawing_context(owner)!=std::optional<DrawingContext>{intent.context})
                invalid("fresh physical source is missing or belongs to another drawing context");
            const auto placed=resolve_vertical_placement(source,found->second);
            Wall wall;std::string error;
            if (!read_document_wall(placed,{},wall,error)) invalid("fresh physical source: "+error);
            if (!std::isfinite(wall.elevation) ||
                std::abs(wall.elevation-intent.effective_elevation_m)>default_geometry_tolerance_metres)
                invalid("fresh physical source belongs to another effective plane");
            validate_active(source,{owner},"fresh physical source");
            cached=seeded_detections.emplace(owner,detect_physical_wall_spaces(source,owner)).first;
        }
        if (std::none_of(cached->second.spaces.begin(),cached->second.spaces.end(),[&](const auto& candidate) {
            return candidate.source_lineage==space.source_lineage;
        })) invalid("fresh physical source cannot reproduce the exact reviewed lineage");
        return owner;
    };
    for (const auto& [index,d]:fresh) {
        if (d->disposition==PhysicalWallRoomFreshDisposition::unclassified) continue;
        const auto& space=detection.spaces[index];const auto replacement=identified(space,*d);
        const auto selected_wall_id=source_owner(space);
        if (d->disposition==PhysicalWallRoomFreshDisposition::retained) {
            BoundaryGeometryEdit edit;edit.boundary_id=d->room_id;edit.target_id=d->room_id;
            edit.kind=BoundaryGeometryEditKind::redefine_boundary;edit.fresh_topology=true;
            edit.replacement_segments=encode_identified_boundary_entity(replacement).properties.at("segments");
            edit.replacement_child_mapping=old.at(d->room_id)->child_mapping;
            edit.replacement_dimension_ids=old.at(d->room_id)->replacement_dimension_ids;
            edit.physical_wall_room_repair=PhysicalWallRoomRepairIntent{selected_wall_id,d->interior_witness,
                d->reviewed_source_lineage,old.at(d->room_id)->expected_descriptor_digest};
            result=edited_boundary_entities_for_room_review(result,edit,retained_owners);
            retained_edits.push_back(std::move(edit));
        } else {
            if (active_rooms) {
                const auto registry=room_creation_registry(source,wall_registries,space,intent.selected_wall_id);
                if (registry) created_by_registry[*registry].push_back(d->room_id);
            }
            const auto& c=d->context;
            Entity room=encode_identified_boundary_entity(replacement);
            room.properties.update({{"property_id",c.property_id},{"building_id",c.building_id},{"floor_id",c.floor_id},{"layer_id",c.layer_id},
                {"name",d->name},{"classification",d->classification},{"measurement_classification",d->classification},
                {"factor",1.0},{"factor_expression","1"},{"factor_numerator",1},{"factor_denominator",1}});
            room.extensions["physical_wall_room"]=encode_physical_wall_room_descriptor({selected_wall_id,space.source_lineage,space.holes});
            result.emplace(room.id,std::move(room));
            created_room_ids.push_back(d->room_id);
        }
    }
    if (active_rooms) {
        register_created_rooms(result,created_by_registry);
        // Re-admit the resulting actual inventory, including fresh physical
        // owners, without changing any evaluated or saved phase selection.
        (void)active_physical_wall_room_ids(result);
    }
    for (const auto& reference_id:preserved_constraints) {
        const auto found=result.find(reference_id);
        const auto& original=source.at(reference_id);
        if (found==result.end() || found->second!=original ||
            found->second.properties.dump()!=original.properties.dump() ||
            found->second.extensions.dump()!=original.extensions.dump())
            invalid("room redraw would rewrite a constraint attached to an inactive design: "+reference_id);
    }
    if (const auto error=validate_boundary_integrity(result)) invalid(*error);
    for (const auto& id:kept) {
        if (!result.contains(id)) {
            // Explicitly kept automatic edge dimensions may have been replaced
            // by the reviewed full-edge set with fresh lifetime-safe identities.
            const auto decoded=decode_boundary_dimension_entity(source.at(id));
            if (!decoded.supported() || decoded.dimension->placement!=BoundaryDimensionPlacement::automatic ||
                decoded.dimension->kind!=BoundaryDimensionKind::segment_length ||
                old.at(decoded.dimension->boundary_id)->replacement_dimension_ids.empty())
                invalid("kept reference disappeared from the reviewed candidate");
        } else if (can_recognize_boundary_dimension_entity_type(result.at(id).type)) {
            const auto decoded=decode_boundary_dimension_entity(result.at(id));
            (void)resolve_boundary_dimension(*decoded.dimension,result);
        }
    }
    if (active_rooms) for (const auto& [room_id,room]:source) {
        if (!is_physical_wall_room(room) || active_rooms->contains(room_id)) continue;
        const auto preserved=result.find(room_id);
        if (preserved==result.end() || preserved->second!=room ||
            preserved->second.properties.dump()!=room.properties.dump() ||
            preserved->second.extensions.dump()!=room.extensions.dump())
            invalid("ordinary active room review changed an inactive original owner");
    }
    return {std::move(result),std::move(retained_edits),std::move(created_room_ids),{retiring.begin(),retiring.end()}};
}

Entities replay_physical_wall_room_review_entities(const Entities& source,const Json& encoded,bool active_phase_constraints) {
    return replay_physical_wall_room_review(source,encoded,active_phase_constraints).entities;
}

ReplayedPhysicalWallRoomReview replay_physical_wall_room_review_batch(const Entities& source,
    const std::vector<Json>& intents,bool active_phase_constraints) {
    require_room_review_batch_size(intents.size());
    RoomReviewBatchGuard guard(source);
    ReplayedPhysicalWallRoomReview result;result.entities=source;
    for (const auto& encoded:intents) {
        guard.admit(decode_physical_wall_room_review_intent(encoded));
        auto stage=replay_physical_wall_room_review(result.entities,encoded,active_phase_constraints);
        result.entities=std::move(stage.entities);
        result.retained_edits.insert(result.retained_edits.end(),stage.retained_edits.begin(),stage.retained_edits.end());
        result.created_room_ids.insert(result.created_room_ids.end(),stage.created_room_ids.begin(),stage.created_room_ids.end());
        result.retired_room_ids.insert(result.retired_room_ids.end(),stage.retired_room_ids.begin(),stage.retired_room_ids.end());
    }
    return result;
}

PreparedPhysicalWallRoomReview prepare_physical_wall_room_review(const DocumentSnapshot& source,
    const PhysicalWallRoomCorrespondenceReport& report,const PhysicalWallRoomReviewIntent& intent) {
    if (!source.is_editable()) invalid("captured document is read-only");
    if (!physical_wall_room_correspondence_is_current(report,source) || intent.source_snapshot_digest!=document_snapshot_digest(source) ||
        intent.source_authoring_digest!=document_authoring_source_digest_v2(source) ||
        intent.source_saved_revision!=source.saved_revision_optional())
        invalid("complete captured snapshot changed");
    if (report.context_plane_selection!=intent.context_plane_selection || report.selected_wall_id!=intent.selected_wall_id ||
        report.context!=intent.context || report.effective_elevation_m!=intent.effective_elevation_m)
        invalid("intent does not belong to the displayed context/plane report");
    auto captured_intent=intent;
    // Scope comes from the current displayed ordinary report, never from a
    // caller's unchecked marker or an explicit destination-phase report.
    captured_intent.active_phase_room_scope=report.active_phase_room_scope;
    auto encoded=encode_physical_wall_room_review_intent(captured_intent);
    auto replayed=replay_physical_wall_room_review(source.entities(),encoded,source.uses_active_phase_constraints());
    return {std::move(replayed),std::move(encoded)};
}

namespace {
bool profile_upsert(const std::vector<EntityChange>& changes) {
    return changes.size()==1 && changes.front().kind==EntityChangeKind::upsert &&
        changes.front().entity.type=="wall" && changes.front().entity_id==changes.front().entity.id;
}
bool exact_entity(const Entity& left,const Entity& right) {
    return left==right && left.properties.dump()==right.properties.dump() && left.extensions.dump()==right.extensions.dump();
}
bool exact_entities(const Entities& left,const Entities& right) {
    if (left.size()!=right.size()) return false;
    for (const auto& [id,entity]:left) {
        const auto found=right.find(id);
        if (found==right.end() || !exact_entity(entity,found->second)) return false;
    }
    return true;
}
bool exact_assets(const std::map<std::string,Asset,std::less<>>& left,
    const std::map<std::string,Asset,std::less<>>& right) {
    if (left.size()!=right.size()) return false;
    for (const auto& [id,asset]:left) {
        const auto found=right.find(id);
        if (found==right.end() || asset!=found->second || asset.metadata.dump()!=found->second.metadata.dump()) return false;
    }
    return true;
}
Json room_review_geometry_proof(const DocumentSnapshot& source,const Command& geometry_command,
    const Json& retained_geometry_proof=Json(nullptr)) {
    if (!retained_geometry_proof.is_null()) {
        if (retained_geometry_proof.is_object() &&
            retained_geometry_proof.value("kind",std::string{})=="mixed_wall_opening_deletion") {
            const auto decoded=decode_mixed_wall_opening_deletion_review_proof(retained_geometry_proof);
            if (decoded.command.expected_revision!=source.revision() ||
                command_to_json(Command{decoded.command}).dump()!=command_to_json(geometry_command).dump())
                invalid("explicit mixed wall/opening proof differs from the captured raw geometry command");
            const auto admitted=encode_mixed_wall_opening_deletion_review_proof(source,decoded.intent,geometry_command);
            if (admitted.dump()!=retained_geometry_proof.dump())
                invalid("explicit mixed wall/opening proof differs from complete captured-source admission");
            return retained_geometry_proof;
        }
        const auto decoded=decode_mixed_wall_deletion_review_proof(retained_geometry_proof);
        if (decoded.command.expected_revision!=source.revision() ||
            command_to_json(Command{decoded.command}).dump()!=command_to_json(geometry_command).dump())
            invalid("explicit mixed wall proof differs from the captured raw geometry command");
        const auto admitted=encode_mixed_wall_deletion_review_proof(source,decoded.intent,geometry_command,
            decoded.complete_opening_hosted_removal);
        if (admitted.dump()!=retained_geometry_proof.dump())
            invalid("explicit mixed wall proof differs from complete captured-source admission");
        return retained_geometry_proof;
    }
    if (is_physical_wall_room_deletion_review_command(geometry_command))
        return encode_physical_wall_deletion_review_proof(source,geometry_command);
    if (is_physical_wall_room_profile_review_command(geometry_command)) return command_to_json(geometry_command);
    if (is_physical_wall_room_rigid_review_command(geometry_command)) return command_to_json(geometry_command);
    if (is_physical_wall_room_joint_review_command(geometry_command)) return command_to_json(geometry_command);
    if (is_physical_wall_room_active_constraint_review_command(geometry_command)) return command_to_json(geometry_command);
    const auto* geometry=std::get_if<ApplyBoundaryConstraintChanges>(&geometry_command);
    if (!geometry || geometry->wall_edits.empty()) invalid("review requires a direct command with explicit wall edits");
    // Inspect typed lanes as well as the serialized discriminator: retained or
    // stripped specialized fields cannot borrow an ordinary wall-edit envelope.
    if (geometry->wall_split || geometry->wall_merge || geometry->exterior_corner_move ||
        geometry->exterior_segment_resize || geometry->exterior_segment_arc ||
        geometry->rigid_wall_transform_completion || geometry->rigid_group_completion || geometry->rigid_group_transform ||
        geometry->joint_translation_completion || geometry->joint_translation ||
        geometry->room_review_completion || !geometry->room_review_intent.is_null() ||
        geometry->room_review_geometry_completion || !geometry->room_review_geometry_proof.is_null() ||
        geometry->room_review_batch_completion || !geometry->room_review_additional_intents.empty() ||
        geometry->selection_completion || !geometry->selection_entity_changes.empty() ||
        geometry->dimension_placement_completion || !geometry->dimension_placement_moves.empty() ||
        geometry->wall_dimension_completion || geometry->disto_measurement_completion || geometry->disto_measurement ||
        geometry->supplemental_asset_reference_completion || !geometry->supplemental_asset_changes.empty())
        invalid("wall review cannot borrow another specialized intent or command wrapper");
    const auto proof=command_to_json(geometry_command);
    const auto version=proof.at("version");
    if (proof.at("kind")!="apply_boundary_constraint_changes" || proof.dump().size()>1024*1024)
        invalid("review requires one bounded direct wall geometry proof");
    if (geometry->curve_construction_completion) {
        if (version!=23) invalid("review requires an unwrapped curve construction command");
    } else {
        if (version!=2 && version!=3 && version!=4 && version!=5 && version!=6 && version!=7 && version!=11)
            invalid("review requires an unwrapped ordinary wall-edit command");
        if (std::any_of(geometry->wall_edits.begin(),geometry->wall_edits.end(),[](const auto& edit) {
            return (edit.version!=1 && edit.version!=2 && edit.version!=3) || edit.rigid_transform ||
                edit.curve_construction || edit.wall_classification;
        })) invalid("ordinary wall review requires endpoint or length wall proofs only");
    }
    if (command_to_json(command_from_json(proof))!=proof)
        invalid("wall geometry proof is not canonical");
    return proof;
}
void require_room_review_curve(const DocumentSnapshot& source,const Command& curve_command) {
    if (!source.is_editable()) invalid("captured document is read-only");
    const auto* curve=std::get_if<ApplyBoundaryConstraintChanges>(&curve_command);
    if (!curve || !curve->curve_construction_completion) invalid("review requires a direct curve construction command");
}
Json plain_room_review_proof(const ApplyBoundaryConstraintChanges& command) {
    if (!command.room_review_completion || command.room_review_intent.is_null() ||
        !command.boundary_edits.empty() || !command.entity_changes.empty() || !command.wall_edits.empty() ||
        !command.physical_entity_changes.empty() || !command.exterior_source_edits.empty() ||
        !command.supplemental_entity_changes.empty() || !command.supplemental_asset_changes.empty() ||
        !command.measured_stroke_edits.empty() || !command.dimension_placement_moves.empty() ||
        !command.selection_entity_changes.empty() ||
        command.exterior_source_completion || command.supplemental_source_completion ||
        command.supplemental_asset_reference_completion || command.rigid_wall_transform_completion ||
        command.measured_source_completion || command.dimension_placement_completion ||
        command.rigid_group_completion || command.rigid_group_transform || command.wall_split || command.wall_merge ||
        command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc ||
        command.joint_translation_completion || command.joint_translation || command.wall_dimension_completion ||
        command.curve_construction_completion || command.disto_measurement_completion || command.disto_measurement ||
        command.selection_completion || command.room_review_geometry_completion || !command.room_review_geometry_proof.is_null() ||
        command.room_review_batch_completion || !command.room_review_additional_intents.empty())
        invalid("staged batch decisions require a plain room-only command");
    const auto proof=command_to_json(Command{command});
    keys(proof,{"version","kind","expected_revision","message","room_review_completion","room_review_intent"});
    const auto intent=decode_physical_wall_room_review_intent(command.room_review_intent);
    if (proof.at("version")!=(intent.context_plane_selection ? 29 : 18) || proof.at("kind")!="apply_boundary_constraint_changes" ||
        command_to_json(command_from_json(proof)).dump()!=proof.dump())
        invalid("staged batch decisions require an exact plain room-review envelope for their intent version");
    return proof;
}
} // namespace

bool is_physical_wall_room_rigid_review_command(const Command& command) {
    try {
        const auto* geometry=std::get_if<ApplyBoundaryConstraintChanges>(&command);
        if (!geometry || geometry->wall_edits.empty()) return false;
        // Refuse every other typed intent before entering the codec. In
        // particular, an outer room proof must never recursively use this
        // predicate while serializing or decoding its geometry child.
        if (geometry->wall_split || geometry->wall_merge || geometry->exterior_corner_move ||
            geometry->exterior_segment_resize || geometry->exterior_segment_arc ||
            geometry->rigid_group_completion || geometry->rigid_group_transform ||
            geometry->joint_translation_completion || geometry->joint_translation ||
            geometry->room_review_completion || !geometry->room_review_intent.is_null() ||
            geometry->room_review_geometry_completion || !geometry->room_review_geometry_proof.is_null() ||
            geometry->room_review_batch_completion || !geometry->room_review_additional_intents.empty() ||
            geometry->selection_completion || !geometry->selection_entity_changes.empty() ||
            geometry->dimension_placement_completion || !geometry->dimension_placement_moves.empty() ||
            geometry->disto_measurement_completion || geometry->disto_measurement ||
            geometry->curve_construction_completion || geometry->supplemental_asset_reference_completion ||
            !geometry->supplemental_asset_changes.empty()) return false;
        bool qualified_rigid_wall=false;
        for (const auto& edit:geometry->wall_edits) {
            if (edit.version<1 || edit.version>5 || edit.curve_construction || edit.wall_classification) return false;
            const bool rigid=edit.version==4 || edit.version==5;
            if (rigid!=edit.rigid_transform.has_value()) return false;
            qualified_rigid_wall=qualified_rigid_wall || rigid;
        }
        // Measured-only commands cannot acquire physical-room authority from
        // an envelope marker or from unrelated source-completion payloads.
        if (!qualified_rigid_wall) return false;
        const auto proof=command_to_json(command);
        if (proof.dump().size()>1024*1024 || proof.at("kind")!="apply_boundary_constraint_changes") return false;
        const auto& version=proof.at("version");
        if (version!=10 && version!=11 && version!=21) return false;
        if (version==21) {
            const auto& child=proof.at("proof");
            if (child.at("kind")!="apply_boundary_constraint_changes" ||
                (child.at("version")!=10 && child.at("version")!=11)) return false;
        }
        // The existing wall-edit codec validates curved/straight rigid proof
        // meanings, and v21 validates one shared wall/stroke operator. Direct
        // v10/v11 decoding never consults room-review admission.
        return command_to_json(command_from_json(proof)).dump()==proof.dump();
    } catch (const std::exception&) { return false; }
}

bool is_physical_wall_room_active_constraint_review_command(const Command& command) {
    try {
        const auto* geometry=std::get_if<ApplyBoundaryConstraintChanges>(&command);
        if (!geometry || !geometry->phase_constraint_authoring_completion ||
            geometry->phase_constraint_authoring_intent.is_null() || geometry->selection_completion ||
            !geometry->selection_entity_changes.empty()) return false;
        const auto proof=command_to_json(command);
        if (proof.at("kind")!="apply_boundary_constraint_changes") return false;
        if (proof.at("version")!=34 &&
            (proof.at("version")!=19 || proof.at("proof").at("version")!=34)) return false;
        return command_to_json(command_from_json(proof)).dump()==proof.dump();
    } catch (const std::exception&) { return false; }
}

bool is_physical_wall_room_joint_review_command(const Command& command) {
    try {
        const auto* geometry=std::get_if<ApplyBoundaryConstraintChanges>(&command);
        if (!geometry || geometry->wall_edits.empty() || !geometry->joint_translation_completion ||
            !geometry->joint_translation || geometry->joint_translation->partial_wall_ids.empty()) return false;
        // Inspect typed markers before invoking the codec: a competing wrapper
        // must not recursively borrow room authority or disappear from proof.
        if (geometry->wall_split || geometry->wall_merge || geometry->exterior_corner_move ||
            geometry->exterior_segment_resize || geometry->exterior_segment_arc ||
            geometry->rigid_group_completion || geometry->rigid_group_transform ||
            geometry->room_review_completion || !geometry->room_review_intent.is_null() ||
            geometry->room_review_geometry_completion || !geometry->room_review_geometry_proof.is_null() ||
            geometry->room_review_batch_completion || !geometry->room_review_additional_intents.empty() ||
            geometry->selection_completion || !geometry->selection_entity_changes.empty() ||
            geometry->wall_dimension_completion || geometry->disto_measurement_completion || geometry->disto_measurement ||
            geometry->curve_construction_completion || geometry->supplemental_asset_reference_completion ||
            !geometry->supplemental_asset_changes.empty()) return false;
        if (std::any_of(geometry->wall_edits.begin(),geometry->wall_edits.end(),[](const auto& edit) {
            return edit.version<1 || edit.version>5 || edit.curve_construction || edit.wall_classification;
        })) return false;
        // Do not strip the joint intent or reconstruct a common operator here.
        // Existing v17 admission owns every lower geometry/dimension receipt
        // and the separately source-qualified presentation consequences.
        const auto proof=command_to_json(command);
        if (proof.dump().size()>1024*1024 || proof.at("kind")!="apply_boundary_constraint_changes" ||
            proof.at("version")!=17) return false;
        return command_to_json(command_from_json(proof)).dump()==proof.dump();
    } catch (const std::exception&) { return false; }
}

bool is_physical_wall_room_deletion_review_command(const Command& command) {
    try {
        const auto* ordinary=std::get_if<ApplyEntityChanges>(&command);
        if (!ordinary || !ordinary->asset_changes.empty() || ordinary->entity_changes.empty() ||
            ordinary->entity_changes.size()>65536 ||
            std::none_of(ordinary->entity_changes.begin(),ordinary->entity_changes.end(),[](const auto& change) {
                return change.kind==EntityChangeKind::erase;
            })) return false;
        const auto proof=command_to_json(command);
        return proof.dump().size()<=1024*1024 && proof.at("version")==1 && proof.at("kind")=="apply_entity_changes" &&
            command_to_json(command_from_json(proof)).dump()==proof.dump();
    } catch (const std::exception&) { return false; }
}

namespace {
// Encoder inspection is a native join admission too. Reserve the hosted and
// join work together before asking the helper to derive its canonical slots.
// This is analytical source admission, never a modified native source map.
void bound_wall_deletion_join_inference(const Entities& source,const std::vector<std::string>& wall_ids,
    bool complete_opening_hosted_removal=false,bool complete_hosted_catalog_consequences=false) {
    bound_complete_wall_deletion_source(source);
    const std::set<std::string> roots(wall_ids.begin(),wall_ids.end());
    std::set<std::string> joined_walls;
    std::size_t work{};
    const auto add=[&](std::size_t count) {
        if (count>262144-work) invalid("aggregate wall deletion join inference work budget exceeded");
        work+=count;
    };
    for (const auto& [owner,entity]:source) if (entity.type=="wall_join" &&
        (mentions(entity.properties,roots) || mentions(entity.extensions,roots))) {
        const auto join=parse_wall_join(entity.properties,owner);
        if (join.wall_ids.size()>4096) invalid("join inference member budget exceeded");
        add(2*join.wall_ids.size()*join.wall_ids.size());
        joined_walls.insert(join.wall_ids.begin(),join.wall_ids.end());
    }
    if (joined_walls.size()>4096) invalid("join inference wall expansion budget exceeded");
    const auto scope=constraint_phase_scope(source);
    std::map<std::string,std::size_t,std::less<>> joined_openings,removed_openings,manufactured_openings;
    std::set<std::string> removed_semantic_openings;
    for (const auto& [owner,entity]:source) {
        if (entity.type!="opening" && entity.type!="door" && entity.type!="window") continue;
        std::string host,error;
        if (!read_document_wall_id(entity,host,error)) invalid(error);
        if (entity.type=="opening" && !scope.inactive_owner_ids.contains(owner) && joined_walls.contains(host)) ++joined_openings[host];
        if (roots.contains(host)) {
            ++removed_openings[host];
            if (entity.type=="opening") removed_semantic_openings.insert(owner);
            if (entity.properties.contains("opening_kind")) {
                const auto& kind=entity.properties.at("opening_kind");
                if (!kind.is_string()) invalid("malformed join inference opening kind");
                if (parse_opening_assembly_kind(kind.get<std::string>())) ++manufactured_openings[host];
            }
        }
    }
    std::map<std::string,std::size_t,std::less<>> costs;
    for (const auto& owner:roots) {
        const auto actual=source.find(owner);
        if (actual==source.end() || actual->second.type!="wall") invalid("join inference requires actual walls");
        const auto& p=actual->second.properties;
        const auto layers=p.contains("layers") ? p.at("layers").size() : 0;
        if (layers>1024) invalid("join inference wall layer budget exceeded");
        const auto cost=(1+layers)*(1+removed_openings[owner]);costs.emplace(owner,cost);add(3*cost);
        add(32*(1+cost)*manufactured_openings[owner]);
    }
    for (const auto& owner:joined_walls) {
        const auto actual=source.find(owner);
        if (actual==source.end() || actual->second.type!="wall") invalid("join inference member is not an actual wall");
        const auto& p=actual->second.properties;
        const auto layers=p.contains("layers") ? p.at("layers").size() : 0;
        if (layers>32 || joined_openings[owner]>128) invalid("join inference source geometry budget exceeded");
        add(4*(1+layers)*(1+joined_openings[owner]));
    }
    AssemblyExpansionBudget expansion_budget;
    std::vector<std::pair<std::string,std::string>> components;
    bool opening_hosted_rows{};
    for (const auto& [owner,entity]:source) if (entity.type=="assembly_model") {
        (void)owner;
        const auto model=AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& row:model.instances()) if (row.placement) {
            const auto host=source.find(row.placement->host_entity_id);
            if (host!=source.end() && host->second.type=="wall_join")
                invalid("catalog placement on a wall join has no actual Document host contract");
            const bool opening_host=complete_opening_hosted_removal &&
                removed_semantic_openings.contains(row.placement->host_entity_id);
            if (!roots.contains(row.placement->host_entity_id) && !opening_host) continue;
            opening_hosted_rows=opening_hosted_rows || opening_host;
            components.emplace_back(owner,row.id);
            if (model.expand(row,expansion_budget).profiles.empty() && !opening_host)
                add(costs.at(row.placement->host_entity_id));
        }
    }
    add(expansion_budget.consumed_nodes);add(expansion_budget.consumed_profile_segments);
    if (opening_hosted_rows || (complete_hosted_catalog_consequences && !components.empty()))
        preflight_architectural_object_removal(source,{},components,work,complete_hosted_catalog_consequences);
}
PhysicalWallJoinRemovalAdditionalIdentities decode_wall_deletion_join_identities(const Json& value) {
    if (!value.is_object() || value.size()>128) invalid("unsupported additional join identity map");
    PhysicalWallJoinRemovalAdditionalIdentities result;
    std::set<std::string> destinations;
    for (const auto& [owner,rows]:value.items()) {
        id(owner);
        if (!rows.is_array() || rows.empty() || rows.size()>4096-destinations.size())
            invalid("additional join identity slots must be bounded and nonempty");
        auto values=rows.get<std::vector<std::string>>();
        for (const auto& destination:values) {
            id(destination);
            if (destination==owner || !destinations.insert(destination).second)
                invalid("additional join identity destinations repeat or borrow their owner");
        }
        result.emplace(owner,std::move(values));
    }
    for (const auto& [owner,rows]:result) {
        (void)rows;
        if (destinations.contains(owner)) invalid("additional join identity keys and destinations overlap");
    }
    if (Json(result).dump()!=value.dump()) invalid("additional join identity map is not canonical");
    return result;
}
PhysicalWallJoinRemovalAdditionalIdentities infer_wall_deletion_join_identities(
    const Entities& source,const std::vector<std::string>& wall_ids,const ApplyEntityChanges& ordinary,
    bool complete_opening_hosted_removal=false) {
    bound_wall_deletion_join_inference(source,wall_ids,complete_opening_hosted_removal);
    const auto plan=inspect_physical_wall_join_removal(source,wall_ids);
    if (!plan.ready()) for (const auto& diagnostic:plan.diagnostics) if (diagnostic.blocking) invalid(diagnostic.reason);
    if (plan.source_join_ids.empty() && !complete_opening_hosted_removal)
        invalid("join completion requires actual affected source joins");
    std::map<std::string,WallJoin,std::less<>> fresh;
    for (const auto& change:ordinary.entity_changes) if (change.kind==EntityChangeKind::upsert && !source.contains(change.entity.id)) {
        if (change.entity.type!="wall_join" || !fresh.emplace(change.entity.id,parse_wall_join(change.entity.properties,change.entity.id)).second)
            invalid("join completion has foreign or duplicate fresh owner changes");
    }
    PhysicalWallJoinRemovalAdditionalIdentities result;
    std::set<std::string> used;
    for (const auto& [owner,count]:plan.additional_identity_counts) {
        auto& destinations=result[owner];bool first=true;
        for (const auto& component:plan.surviving_join_components.at(owner)) if (component.size()>=2) {
            if (first) { first=false;continue; }
            std::optional<std::string> matched;
            for (const auto& [destination,join]:fresh) if (join.wall_ids==component) {
                if (matched || used.contains(destination)) invalid("additional join cohort is ambiguous or repeated");
                matched=destination;
            }
            if (!matched) invalid("missing actual fresh join member cohort");
            used.insert(*matched);destinations.push_back(*matched);
        }
        if (destinations.size()!=count) invalid("additional join slots differ from source-derived cohorts");
    }
    if (used.size()!=fresh.size()) invalid("join completion has unnecessary fresh join identities");
    return decode_wall_deletion_join_identities(Json(result));
}
bool retired_opening_hosted_rows(const Entities& source,const Entities& candidate,const std::vector<std::string>& wall_ids) {
    const std::set<std::string> roots(wall_ids.begin(),wall_ids.end());
    for (const auto& [catalog,entity]:source) if (entity.type=="assembly_model") {
        const auto after=candidate.find(catalog);
        // Catalog erasure never qualifies as retirement of its actual rows.
        if (after==candidate.end() || after->second.type!="assembly_model") continue;
        // An unchanged unrelated catalog must not add codec requirements to
        // historical raw/grouped capture merely because v38 now exists.
        if (entity.properties.dump()==after->second.properties.dump()) continue;
        const auto model=AssemblyModel::from_json(entity.properties.at("model"));
        const auto remaining=AssemblyModel::from_json(after->second.properties.at("model"));
        for (const auto& row:model.instances()) if (row.placement) {
            const auto host=source.find(row.placement->host_entity_id);
            if (host==source.end() || host->second.type!="opening" || candidate.contains(host->first)) continue;
            std::string wall_id,error;
            if (!read_document_wall_id(host->second,wall_id,error)) invalid(error);
            if (!roots.contains(wall_id)) continue;
            if (std::none_of(remaining.instances().begin(),remaining.instances().end(),
                [&](const auto& survivor){return survivor.id==row.id;})) return true;
        }
    }
    return false;
}
} // namespace

void preflight_physical_walls_deletion_join_inference(const Entities& actual,const std::vector<std::string>& wall_ids,
    bool complete_opening_hosted_removal,bool complete_hosted_catalog_consequences) {
    if (wall_ids.empty() || wall_ids.size()>128) invalid("join preflight requires one to 128 physical walls");
    auto roots=wall_ids;std::sort(roots.begin(),roots.end());ids(roots);
    bound_wall_deletion_join_inference(actual,roots,complete_opening_hosted_removal,complete_hosted_catalog_consequences);
}

Command decode_physical_wall_deletion_review_proof(const Json& proof) {
    const bool opening_hosts=proof.is_object() && proof.contains("version") && proof.at("version")==38;
    const bool joins=opening_hosts || (proof.is_object() && proof.contains("version") && proof.at("version")==36);
    const bool complete=joins || (proof.is_object() && proof.contains("version") && proof.at("version")==35);
    if (opening_hosts) keys(proof,{"version","kind","expected_revision","message","wall_ids","proof","complete_hosted_removal",
        "complete_join_removal","additional_join_identities","complete_opening_hosted_removal"});
    else if (joins) keys(proof,{"version","kind","expected_revision","message","wall_ids","proof","complete_hosted_removal",
        "complete_join_removal","additional_join_identities"});
    else if (complete) keys(proof,{"version","kind","expected_revision","message","wall_ids","proof","complete_hosted_removal"});
    else keys(proof,{"version","kind","expected_revision","message","wall_ids","proof"});
    if (proof.dump().size()>1024*1024 || !proof.at("version").is_number_integer() ||
        proof.at("version")!=(opening_hosts ? 38 : joins ? 36 : complete ? 35 : 31) || proof.at("kind")!="physical_wall_deletion" ||
        (complete && (!proof.at("complete_hosted_removal").is_boolean() || proof.at("complete_hosted_removal")!=true)) ||
        (joins && (!proof.at("complete_join_removal").is_boolean() || proof.at("complete_join_removal")!=true)) ||
        (opening_hosts && (!proof.at("complete_opening_hosted_removal").is_boolean() || proof.at("complete_opening_hosted_removal")!=true)) ||
        !proof.at("wall_ids").is_array() || proof.at("wall_ids").size()<(complete ? 1u : 2u) || proof.at("wall_ids").size()>128)
        invalid("unsupported grouped wall deletion proof");
    const auto wall_ids=proof.at("wall_ids").get<std::vector<std::string>>();
    ids(wall_ids);
    if (!std::is_sorted(wall_ids.begin(),wall_ids.end())) invalid("grouped wall identities must be sorted");
    const auto& raw_proof=proof.at("proof");
    keys(raw_proof,{"version","kind","expected_revision","message","entity_changes","asset_changes"});
    if (raw_proof.at("version")!=1 || raw_proof.at("kind")!="apply_entity_changes")
        invalid("grouped deletion requires a raw version-one child");
    auto command=command_from_json(raw_proof);
    if (!is_physical_wall_room_deletion_review_command(command) || command_to_json(command).dump()!=raw_proof.dump())
        invalid("grouped deletion requires a bounded canonical asset-free child");
    const auto& ordinary=std::get<ApplyEntityChanges>(command);
    const auto additional=joins ? decode_wall_deletion_join_identities(proof.at("additional_join_identities")) :
        PhysicalWallJoinRemovalAdditionalIdentities{};
    std::set<std::string> erased,changed,declared_fresh,upserted_fresh;
    for (const auto& [owner,rows]:additional) { (void)owner;declared_fresh.insert(rows.begin(),rows.end()); }
    for (const auto& change:ordinary.entity_changes) {
        const auto owner=change.kind==EntityChangeKind::erase ? change.entity_id : change.entity.id;
        if (joins && !changed.insert(owner).second) invalid("join deletion child repeats or cross-owns an entity change");
        if (joins && declared_fresh.contains(owner)) {
            if (change.kind!=EntityChangeKind::upsert || change.entity.type!="wall_join")
                invalid("declared fresh join is not a typed join upsert");
            (void)parse_wall_join(change.entity.properties,owner);upserted_fresh.insert(owner);
        }
        if (change.kind==EntityChangeKind::erase) {
            const bool inserted=erased.insert(change.entity_id).second;
            if (complete && !inserted) invalid("wall deletion child repeats an erased identity");
        }
    }
    if (joins && upserted_fresh!=declared_fresh) invalid("declared fresh join is missing from the raw child");
    for (const auto& wall_id:wall_ids)
        if (!erased.contains(wall_id)) invalid("declared physical wall is not erased by the grouped child");
    Json canonical{{"version",opening_hosts ? 38 : joins ? 36 : complete ? 35 : 31},{"kind","physical_wall_deletion"},
        {"expected_revision",ordinary.expected_revision},{"message",ordinary.message},
        {"wall_ids",wall_ids},{"proof",command_to_json(command)}};
    if (complete) canonical["complete_hosted_removal"]=true;
    if (joins) { canonical["complete_join_removal"]=true;canonical["additional_join_identities"]=additional; }
    if (opening_hosts) canonical["complete_opening_hosted_removal"]=true;
    if (canonical.dump()!=proof.dump()) invalid("grouped wall deletion proof is not canonical or differs from its child");
    return command;
}

Json encode_physical_wall_deletion_review_proof(const DocumentSnapshot& source,const Command& command) {
    if (!is_physical_wall_room_deletion_review_command(command)) invalid("unsupported direct wall deletion command");
    const auto& ordinary=std::get<ApplyEntityChanges>(command);
    std::vector<std::string> wall_ids;
    for (const auto& change:ordinary.entity_changes) {
        if (change.kind!=EntityChangeKind::erase) continue;
        const auto before=source.entities().find(change.entity_id);
        if (before!=source.entities().end() && before->second.type=="wall") wall_ids.push_back(change.entity_id);
    }
    if (wall_ids.empty() || wall_ids.size()>128) invalid("wall deletion proof requires one to 128 original physical walls");
    std::sort(wall_ids.begin(),wall_ids.end());ids(wall_ids);
    const auto raw_proof=command_to_json(command);
    std::vector<std::string> fresh_joins;
    for (const auto& change:ordinary.entity_changes)
        if (change.kind==EntityChangeKind::upsert && change.entity.type=="wall_join" && !source.entities().contains(change.entity.id))
            fresh_joins.push_back(change.entity.id);
    validate_physical_wall_join_removal_identity_lifetime(source,fresh_joins);
    auto supplied_candidate=source.entities();
    for (const auto& change:ordinary.entity_changes) {
        if (change.kind==EntityChangeKind::erase) supplied_candidate.erase(change.entity_id);
        else supplied_candidate.insert_or_assign(change.entity.id,change.entity);
    }
    const bool opening_hosts=retired_opening_hosted_rows(source.entities(),supplied_candidate,wall_ids);
    if (opening_hosts) bound_wall_deletion_join_inference(source.entities(),wall_ids,true);
    bool legacy_exact=false;
    try {
        const ApplyEntityChanges legacy{ordinary.expected_revision,physical_wall_deletion_changes(source.entities(),wall_ids),{},ordinary.message};
        legacy_exact=command_to_json(Command{legacy}).dump()==raw_proof.dump();
    } catch (const WallDeletionReferenceRefusal&) {
        // The complete producer must still independently admit the entire
        // actual source and exactly reproduce every supplied child change.
    }
    bool complete_exact=false;
    if (!legacy_exact) try {
        const ApplyEntityChanges complete{ordinary.expected_revision,complete_physical_wall_deletion_changes(source.entities(),wall_ids),{},ordinary.message};
        complete_exact=command_to_json(Command{complete}).dump()==raw_proof.dump();
    } catch (const WallDeletionReferenceRefusal&) {
        // Only the expected unsupported-reference refusal permits the fully
        // admitted join lane; malformed or native source failures propagate.
    }
    PhysicalWallJoinRemovalAdditionalIdentities additional;
    bool joined_exact=false;
    bool opening_exact=false;
    if (!legacy_exact && !complete_exact) {
        additional=infer_wall_deletion_join_identities(source.entities(),wall_ids,ordinary,opening_hosts);
        validate_physical_wall_join_removal_identity_lifetime(source,additional);
        try {
            const ApplyEntityChanges joined{ordinary.expected_revision,
                complete_physical_wall_deletion_changes(source.entities(),wall_ids,true,additional),{},ordinary.message};
            joined_exact=command_to_json(Command{joined}).dump()==raw_proof.dump();
        } catch (const WallDeletionReferenceRefusal&) {
            if (!opening_hosts) throw;
        }
        if (!joined_exact && opening_hosts) {
            const ApplyEntityChanges opened{ordinary.expected_revision,
                complete_physical_wall_deletion_changes(source.entities(),wall_ids,true,additional,true),{},ordinary.message};
            opening_exact=command_to_json(Command{opened}).dump()==raw_proof.dump();
        }
        if (!joined_exact && !opening_exact)
            invalid("join-complete wall deletion differs from the whole original-source command");
    }
    if (legacy_exact && wall_ids.size()==1) return raw_proof;
    Json proof{{"version",legacy_exact ? 31 : complete_exact ? 35 : joined_exact ? 36 : 38},{"kind","physical_wall_deletion"},
        {"expected_revision",ordinary.expected_revision},{"message",ordinary.message},
        {"wall_ids",wall_ids},{"proof",raw_proof}};
    if (!legacy_exact) proof["complete_hosted_removal"]=true;
    if (!legacy_exact && !complete_exact) {
        proof["complete_join_removal"]=true;proof["additional_join_identities"]=additional;
    }
    if (opening_exact) proof["complete_opening_hosted_removal"]=true;
    (void)decode_physical_wall_deletion_review_proof(proof);
    return proof;
}

void validate_physical_wall_room_deletion_review_source(const Entities& source,const Entities& candidate,const Command& command,
    const Json& retained_proof,bool active_phase_constraints) {
    if (retained_proof.is_object() && retained_proof.value("kind",std::string{})=="mixed_wall_opening_deletion") {
        validate_mixed_wall_opening_deletion_review_source(source,candidate,command,retained_proof,active_phase_constraints);
        return;
    }
    if (retained_proof.is_object() && retained_proof.value("kind",std::string{})=="mixed_wall_deletion") {
        validate_mixed_wall_deletion_review_source(source,candidate,command,retained_proof);
        return;
    }
    if (!is_physical_wall_room_deletion_review_command(command)) invalid("unsupported direct wall deletion command");
    const auto& ordinary=std::get<ApplyEntityChanges>(command);
    std::vector<std::string> wall_ids;
    for (const auto& change:ordinary.entity_changes) {
        if (change.kind!=EntityChangeKind::erase) continue;
        const auto before=source.find(change.entity_id);
        if (before==source.end() || before->second.type!="wall") continue;
        wall_ids.push_back(change.entity_id);
    }
    if (wall_ids.empty()) invalid("wall deletion proof has no original physical wall");
    std::sort(wall_ids.begin(),wall_ids.end());ids(wall_ids);
    const auto raw_proof=command_to_json(command);
    bool complete_hosted_removal=false;
    bool complete_join_removal=false;
    bool complete_opening_hosted_removal=false;
    PhysicalWallJoinRemovalAdditionalIdentities additional;
    if (!retained_proof.is_null() && retained_proof.is_object() && retained_proof.value("kind",std::string{})=="physical_wall_deletion") {
        const auto decoded=decode_physical_wall_deletion_review_proof(retained_proof);
        if (command_to_json(decoded).dump()!=raw_proof.dump() ||
            retained_proof.at("wall_ids").get<std::vector<std::string>>()!=wall_ids)
            invalid("grouped deletion proof differs from the exact original wall erasures");
        complete_opening_hosted_removal=retained_proof.at("version")==38;
        complete_join_removal=complete_opening_hosted_removal || retained_proof.at("version")==36;
        complete_hosted_removal=complete_join_removal || retained_proof.at("version")==35;
        if (complete_join_removal) additional=decode_wall_deletion_join_identities(retained_proof.at("additional_join_identities"));
    } else {
        if (wall_ids.size()!=1) invalid("one wall deletion review cannot remove several physical walls");
        if (!retained_proof.is_null() && retained_proof.dump()!=raw_proof.dump())
            invalid("single wall deletion retained proof differs from its raw child");
    }
    if (complete_opening_hosted_removal && !retired_opening_hosted_rows(source,candidate,wall_ids))
        invalid("version38 requires actual retired qualified rows hosted on removed semantic openings");
    const ApplyEntityChanges expected{ordinary.expected_revision,
        complete_hosted_removal ? complete_physical_wall_deletion_changes(source,wall_ids,complete_join_removal,additional,
            complete_opening_hosted_removal) : physical_wall_deletion_changes(source,wall_ids),
        {},ordinary.message};
    if (complete_join_removal && !complete_opening_hosted_removal && std::none_of(expected.entity_changes.begin(),expected.entity_changes.end(),[&](const auto& change) {
        if (change.kind==EntityChangeKind::upsert) return change.entity.type=="wall_join";
        const auto actual=source.find(change.entity_id);
        return actual!=source.end() && actual->second.type=="wall_join";
    })) invalid("version36 requires actual changed or retired wall join consequences");
    if (command_to_json(Command{expected}).dump()!=command_to_json(command).dump())
        invalid("wall deletion contains unrelated changes or differs from exact attached-object cleanup");
    auto replayed=source;
    for (const auto& change:expected.entity_changes) {
        if (change.kind==EntityChangeKind::erase) replayed.erase(change.entity_id);
        else replayed.insert_or_assign(change.entity.id,change.entity);
    }
    if (complete_opening_hosted_removal && !retired_opening_hosted_rows(source,replayed,wall_ids))
        invalid("version38 requires actual retired qualified rows hosted on removed semantic openings");
    if (!exact_entities(candidate,replayed)) invalid("wall deletion candidate differs from its original source consequences");
}

Entities replay_complete_physical_walls_deletion(const Entities& actual,const std::vector<std::string>& wall_ids,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities,bool complete_opening_hosted_removal,
    bool complete_hosted_catalog_consequences) {
    if (wall_ids.empty() || wall_ids.size()>128) invalid("complete replay requires one to 128 physical walls");
    auto roots=wall_ids;std::sort(roots.begin(),roots.end());ids(roots);
    const auto changes=complete_physical_wall_deletion_changes(actual,roots,true,additional_join_identities,
        complete_opening_hosted_removal,complete_hosted_catalog_consequences);
    auto candidate=actual;
    for (const auto& change:changes) {
        if (change.kind==EntityChangeKind::erase) candidate.erase(change.entity_id);
        else candidate.insert_or_assign(change.entity.id,change.entity);
    }
    return candidate;
}

ApplyEntityChanges prepare_physical_wall_deletion(const DocumentSnapshot& source,std::string_view wall_id,bool complete_hosted_removal,
    bool complete_join_removal,const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities) {
    return prepare_physical_walls_deletion(source,{std::string(wall_id)},complete_hosted_removal,complete_join_removal,additional_join_identities);
}

ApplyEntityChanges prepare_physical_walls_deletion(const DocumentSnapshot& source,const std::vector<std::string>& wall_ids,bool complete_hosted_removal,
    bool complete_join_removal,const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities,
    bool complete_opening_hosted_removal) {
    if (!source.is_editable()) invalid("captured document is read-only");
    if (complete_join_removal && !complete_hosted_removal) invalid("join completion requires complete hosted removal");
    if (complete_opening_hosted_removal && (!complete_hosted_removal || !complete_join_removal))
        invalid("opening-hosted completion requires hosted and join completion");
    if (!complete_join_removal && !additional_join_identities.empty()) invalid("additional join identities require join completion");
    if (wall_ids.empty() || wall_ids.size()>128) invalid("deletion requires one to 128 physical walls");
    if (complete_join_removal)
        validate_physical_wall_join_removal_identity_lifetime(source,additional_join_identities);
    auto roots=wall_ids;std::sort(roots.begin(),roots.end());
    ApplyEntityChanges command{source.revision(),
        complete_hosted_removal ? complete_physical_wall_deletion_changes(source.entities(),roots,complete_join_removal,
            additional_join_identities,complete_opening_hosted_removal) : physical_wall_deletion_changes(source.entities(),roots),{},
        roots.size()==1 ? "Delete wall and attached objects" : "Delete walls and attached objects"};
    const auto proof=encode_physical_wall_deletion_review_proof(source,Command{command});
    const auto candidate=Document::preview_command(source,Command{command});
    validate_physical_wall_room_deletion_review_source(source.entities(),candidate.entities(),Command{command},proof,
        source.uses_active_phase_constraints());
    return command;
}

bool is_physical_wall_room_profile_review_command(const Command& command) {
    try {
        const auto* raw=std::get_if<ApplyEntityChanges>(&command);
        const auto* completed=std::get_if<ApplyBoundaryConstraintChanges>(&command);
        if (raw) {
            if (!profile_upsert(raw->entity_changes) || !raw->asset_changes.empty()) return false;
        } else if (completed) {
            if (!profile_upsert(completed->physical_entity_changes) || !completed->wall_edits.empty() ||
                !completed->boundary_edits.empty() || !completed->entity_changes.empty() ||
                !completed->exterior_source_completion || completed->exterior_source_edits.empty() ||
                !completed->supplemental_entity_changes.empty() || !completed->supplemental_asset_changes.empty() ||
                completed->supplemental_asset_reference_completion ||
                completed->wall_split || completed->wall_merge || completed->exterior_corner_move ||
                completed->exterior_segment_resize || completed->exterior_segment_arc ||
                completed->rigid_wall_transform_completion || completed->rigid_group_completion || completed->rigid_group_transform ||
                completed->joint_translation_completion || completed->joint_translation ||
                completed->measured_source_completion || !completed->measured_stroke_edits.empty() ||
                completed->dimension_placement_completion || !completed->dimension_placement_moves.empty() ||
                completed->wall_dimension_completion || completed->disto_measurement_completion || completed->disto_measurement ||
                completed->selection_completion || !completed->selection_entity_changes.empty() ||
                completed->curve_construction_completion || completed->room_review_completion || !completed->room_review_intent.is_null() ||
                completed->room_review_geometry_completion || !completed->room_review_geometry_proof.is_null() ||
                completed->room_review_batch_completion || !completed->room_review_additional_intents.empty()) return false;
        } else return false;
        const auto proof=command_to_json(command);
        const int version=raw ? 1 : completed->supplemental_source_completion ? 7 : 6;
        if (proof.dump().size()>1024*1024 || proof.at("version")!=version ||
            proof.at("kind")!=(raw?"apply_entity_changes":"apply_boundary_constraint_changes")) return false;
        return command_to_json(command_from_json(proof)).dump()==proof.dump();
    } catch (const std::exception&) { return false; }
}

void validate_physical_wall_room_profile_review_source(const Entities& source,const Entities& candidate,const Command& command) {
    if (!is_physical_wall_room_profile_review_command(command)) invalid("unsupported direct wall profile command");
    const auto* raw=std::get_if<ApplyEntityChanges>(&command);
    const auto& changes=raw?raw->entity_changes:std::get<ApplyBoundaryConstraintChanges>(command).physical_entity_changes;
    const auto& proposed=changes.front().entity;
    const auto previous=source.find(proposed.id),final=candidate.find(proposed.id);
    if (previous==source.end() || previous->second.type!="wall" || final==candidate.end() ||
        !exact_entity(final->second,proposed) || proposed.id!=previous->second.id ||
        proposed.type!=previous->second.type || proposed.required!=previous->second.required ||
        proposed.extensions.dump()!=previous->second.extensions.dump())
        invalid("profile change requires one existing physical source wall with exact retained identity and extensions");
    if (!previous->second.properties.is_object() || !proposed.properties.is_object()) invalid("malformed wall profile properties");
    const auto old_receipts=previous->second.properties.find("quantity_entries");
    const auto new_receipts=proposed.properties.find("quantity_entries");
    const bool receipt_delta=(old_receipts==previous->second.properties.end())!=(new_receipts==proposed.properties.end()) ||
        (old_receipts!=previous->second.properties.end() && new_receipts!=proposed.properties.end() &&
            old_receipts->dump()!=new_receipts->dump());
    if (receipt_delta) {
        const auto captured=capture_wall_profile_edit(previous->second,proposed);
        if (!captured || !exact_entity(proposed,replay_wall_profile_entity(previous->second,*captured)))
            invalid("profile quantity changes must equal independent typed replay with retained opaque receipts");
    }
    auto old_properties=previous->second.properties,new_properties=proposed.properties;
    for (const auto* field:{"height_m","height","thickness_m","thickness","layers",
        "top_plane","slope_rise_m","slope_rise"}) {
        old_properties.erase(field);new_properties.erase(field);
    }
    if (receipt_delta) {
        old_properties.erase("quantity_entries");new_properties.erase("quantity_entries");
    }
    if (old_properties.dump()!=new_properties.dump()) invalid("profile change cannot borrow geometry, context or other wall properties");
    if (previous->second.properties.dump()==proposed.properties.dump()) invalid("wall profile command must change a declared profile field");
    for (const auto& [id,entity]:source) {
        if (entity.type!="wall" || id==proposed.id) continue;
        const auto found=candidate.find(id);
        if (found==candidate.end() || !exact_entity(entity,found->second)) invalid("profile review cannot change another physical source wall");
    }
    for (const auto& [id,entity]:candidate)
        if (entity.type=="wall" && (!source.contains(id) || source.at(id).type!="wall"))
            invalid("profile review cannot create or replace a physical source wall");
    auto physical=source;physical.at(proposed.id)=proposed;
    if (raw) {
        if (!exact_entities(candidate,physical)) invalid("raw wall profile candidate contains unrelated changes");
        if (!exterior_wall_measurement_source_updates(source,candidate).empty())
            invalid("profile change requires completed exterior source redraws before room review");
    } else {
        const auto redraws=exterior_wall_measurement_source_updates(source,physical);
        auto expected=std::get<ApplyBoundaryConstraintChanges>(command);expected.exterior_source_edits=redraws;
        if (redraws.empty() || command_to_json(Command{expected}).dump()!=command_to_json(command).dump())
            invalid("profile exterior redraws do not match the captured physical sources");
        if (!exact_entities(candidate,edited_boundary_entities_batch(physical,redraws)))
            invalid("completed wall profile candidate differs from its exact source redraws");
    }
}

DocumentSnapshot preview_physical_wall_room_review_geometry(const DocumentSnapshot& source,const Command& geometry_command,
    const Json& retained_geometry_proof) {
    if (!source.is_editable()) invalid("captured document is read-only");
    const auto proof=room_review_geometry_proof(source,geometry_command,retained_geometry_proof);
    // The original child command owns all ordinary admission and consequences.
    auto derived=Document::preview_command(source,geometry_command);
    const bool deletion=is_physical_wall_room_deletion_review_command(geometry_command);
    if (deletion) validate_physical_wall_room_deletion_review_source(source.entities(),derived.entities(),geometry_command,proof,
        source.uses_active_phase_constraints());
    if (is_physical_wall_room_profile_review_command(geometry_command))
        validate_physical_wall_room_profile_review_source(source.entities(),derived.entities(),geometry_command);
    if (derived.assets()!=source.assets()) invalid("wall geometry review cannot change assets");
    for (const auto& [id,entity] : source.entities()) {
        if (entity.type!="wall") continue;
        const auto proposed=derived.entities().find(id);
        if (deletion && proposed==derived.entities().end()) continue;
        if (proposed==derived.entities().end() || proposed->second.type!="wall")
            invalid("wall geometry review cannot remove or replace a physical source wall");
    }
    for (const auto& [id,entity] : derived.entities())
        if (entity.type=="wall" && (!source.entities().contains(id) || source.entities().at(id).type!="wall"))
            invalid("wall geometry review cannot create a physical source wall");
    return derived;
}

PreparedPhysicalWallRoomReviewAfterGeometry prepare_physical_wall_room_review_after_geometry(const DocumentSnapshot& source,
    const Command& geometry_command,const PhysicalWallRoomCorrespondenceReport& report,const PhysicalWallRoomReviewIntent& intent,
    const Json& retained_geometry_proof) {
    const auto derived=preview_physical_wall_room_review_geometry(source,geometry_command,retained_geometry_proof);
    const auto prepared=prepare_physical_wall_room_review(derived,report,intent);
    auto retained_intent=decode_physical_wall_room_review_intent(prepared.intent);
    // The report was reviewed against the detached wall geometry. The final
    // single event must bind the actual original history/save state, while the
    // entity-map digest continues to bind that independently replayed geometry.
    retained_intent.source_snapshot_digest=document_snapshot_digest(source);
    retained_intent.source_authoring_digest=document_authoring_source_digest_v2(source);
    retained_intent.source_saved_revision=source.saved_revision_optional();
    ApplyBoundaryConstraintChanges command;
    command.expected_revision=source.revision();
    if (const auto* raw=std::get_if<ApplyEntityChanges>(&geometry_command)) command.message=raw->message;
    else command.message=std::get<ApplyBoundaryConstraintChanges>(geometry_command).message;
    command.room_review_completion=true;
    command.room_review_intent=encode_physical_wall_room_review_intent(retained_intent);
    command.room_review_geometry_completion=true;command.room_review_geometry_proof=room_review_geometry_proof(source,geometry_command,retained_geometry_proof);
    auto exact=Document::preview_command(source,command);
    if (exact.entities()!=prepared.entities || exact.assets()!=derived.assets())
        invalid("complete wall geometry and room preview differs from the prepared decisions");
    return {std::move(command),std::move(exact)};
}

PreparedPhysicalWallRoomReviewAfterGeometry prepare_physical_wall_room_review_batch_after_geometry(const DocumentSnapshot& source,
    const Command& geometry_command,const std::vector<ApplyBoundaryConstraintChanges>& staged_room_commands,
    const Json& retained_geometry_proof) {
    if (staged_room_commands.size()!=1 || retained_geometry_proof.is_null())
        require_room_review_batch_size(staged_room_commands.size());
    auto stage=preview_physical_wall_room_review_geometry(source,geometry_command,retained_geometry_proof);
    RoomReviewBatchGuard guard(stage.entities());
    std::vector<Json> retained_intents;retained_intents.reserve(staged_room_commands.size());
    const auto original_snapshot_digest=document_snapshot_digest(source);
    const auto original_authoring_digest=document_authoring_source_digest_v2(source);
    const auto original_saved_revision=source.saved_revision_optional();
    for (const auto& room_command:staged_room_commands) {
        (void)plain_room_review_proof(room_command);
        if (room_command.expected_revision!=stage.revision()) invalid("staged room review revision changed");
        auto intent=decode_physical_wall_room_review_intent(room_command.room_review_intent);
        guard.admit(intent);
        // Admission checks the complete actual virtual stage, including its
        // history and save fences, before rebinding the single final event.
        stage=Document::preview_command(stage,Command{room_command});
        intent.source_snapshot_digest=original_snapshot_digest;
        intent.source_authoring_digest=original_authoring_digest;
        intent.source_saved_revision=original_saved_revision;
        retained_intents.push_back(encode_physical_wall_room_review_intent(intent));
    }
    ApplyBoundaryConstraintChanges command;
    command.expected_revision=source.revision();
    if (const auto* raw=std::get_if<ApplyEntityChanges>(&geometry_command)) command.message=raw->message;
    else command.message=std::get<ApplyBoundaryConstraintChanges>(geometry_command).message;
    command.room_review_completion=true;
    command.room_review_intent=std::move(retained_intents.front());
    command.room_review_additional_intents.assign(retained_intents.begin()+1,retained_intents.end());
    command.room_review_batch_completion=staged_room_commands.size()>1;
    command.room_review_geometry_completion=true;
    command.room_review_geometry_proof=room_review_geometry_proof(source,geometry_command,retained_geometry_proof);
    auto exact=Document::preview_command(source,Command{command});
    if (!exact_entities(exact.entities(),stage.entities()) || !exact_assets(exact.assets(),stage.assets()))
        invalid("atomic wall geometry and room batch differs from the cumulative reviewed decisions");
    return {std::move(command),std::move(exact)};
}

DocumentSnapshot preview_physical_wall_room_review_curve(const DocumentSnapshot& source,const Command& curve_command) {
    require_room_review_curve(source,curve_command);
    return preview_physical_wall_room_review_geometry(source,curve_command);
}

PreparedPhysicalWallRoomReviewAfterCurve prepare_physical_wall_room_review_after_curve(const DocumentSnapshot& source,
    const Command& curve_command,const PhysicalWallRoomCorrespondenceReport& report,const PhysicalWallRoomReviewIntent& intent) {
    require_room_review_curve(source,curve_command);
    return prepare_physical_wall_room_review_after_geometry(source,curve_command,report,intent);
}
} // namespace sketch

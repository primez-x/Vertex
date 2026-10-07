#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/stair_semantics.hpp"
#include "sketch/vertical_levels.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Map = std::map<std::string, Entity, std::less<>>;
using Json = nlohmann::json;
constexpr std::size_t max_relevant_work = 2000000, max_json_nodes = 100000;
constexpr std::size_t max_phase_members = 100000, max_alternatives = 4096;
[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
enum class Form { opaque, stair_v1, stair_multi, rail_v1, rail_v2, landing_rail_v3 };
struct Budget { std::size_t json_nodes{}, stair_risers{}, phase_work{}; };

// Only recognized forms are interpreted. A future integer version or unknown
// string form is retained intact; malformed known markers never become legacy.
Form form(const Entity& e) {
    if (e.type != "stair" && e.type != "railing") return Form::opaque;
    const auto& p=e.properties;
    if (!p.is_object()) return Form::opaque;
    const bool has_form=p.contains("form"), has_version=p.contains("version");
    if (!has_form && !has_version) return Form::opaque;
    if (!has_form || !p.at("form").is_string())
        invalid("Stair/railing canonical form must be a string");
    const auto& name=p.at("form").get_ref<const std::string&>();
    const bool stair_name=name=="straight_stair_flight" || name=="multi_flight_stair";
    const bool rail_name=name=="straight_railing" || name=="stair_flight_railing" || name=="stair_landing_railing";
    if (!stair_name && !rail_name) return Form::opaque;
    if ((stair_name && e.type!="stair") || (rail_name && e.type!="railing"))
        invalid("Canonical stair/railing form disagrees with entity type");
    if (!has_version || !p.at("version").is_number_integer())
        invalid("Known stair/railing form requires an integer version");
    const auto& version=p.at("version");
    // Recognize each form's supported versions without interpreting future forms.
    if (name=="stair_landing_railing") {
        if(version>3) return Form::opaque;
        if(version==3) return Form::landing_rail_v3;
        invalid("Landing railing requires version 3");
    }
    if (name=="multi_flight_stair") {
        if (version>3) return Form::opaque;
        if (version==2 || version==3) return Form::stair_multi;
        invalid("Multi-flight stair requires version 2 or 3");
    }
    if (version>2) return Form::opaque;
    if (version==1 && name=="straight_stair_flight") return Form::stair_v1;
    if (version==1 && name=="straight_railing") return Form::rail_v1;
    if (version==2 && name=="stair_flight_railing") return Form::rail_v2;
    invalid("Known stair/railing form and version disagree");
}

// Iterative and bounded, including nesting, before decoding/layout allocation.
void bounded_json(const Json& value,Budget& budget) {
    std::vector<std::pair<const Json*,std::size_t>> pending{{&value,0}};
    std::size_t nodes=0;
    while (!pending.empty()) {
        const auto [node,depth]=pending.back(); pending.pop_back();
        if (++nodes>max_json_nodes || depth>64) invalid("Stair admission JSON limit exceeded");
        if (++budget.json_nodes>max_relevant_work) invalid("Stair admission aggregate JSON limit exceeded");
        if (node->is_string() && node->get_ref<const std::string&>().size()>65536)
            invalid("Stair admission string limit exceeded");
        if (node->is_structured()) {
            if (node->size()>max_json_nodes-nodes || pending.size()>max_json_nodes-node->size())
                invalid("Stair admission JSON limit exceeded");
            for (const auto& child:*node) pending.emplace_back(&child,depth+1);
        }
    }
}
void preflight(const Map& entities,Budget& budget,bool hosted_rails) {
    for (const auto& [id,e]:entities) {
        const auto kind=form(e);
        const bool known=kind!=Form::opaque;
        if (!known && !(hosted_rails && e.type=="model_phases")) continue;
        if (id.empty() || id.size()>128 || e.id!=id) invalid("Stair admission entity map identity mismatch");
        bounded_json(e.properties,budget);
        if (kind==Form::stair_v1 || kind==Form::stair_multi) {
            const auto& p=e.properties;
            const auto count=p.find("riser_count");
            if (count==p.end() || !count->is_number_integer() || *count<1 || *count>10000)
                invalid("Stair admission riser count is invalid");
            const auto risers=count->get<std::size_t>();
            if (risers>max_relevant_work-budget.stair_risers)
                invalid("Stair admission aggregate riser work limit exceeded");
            budget.stair_risers+=risers;
            if (p.contains("host")) invalid("Stair cannot carry railing host authority");
            if (kind==Form::stair_multi) {
                const auto fs=p.find("flights"), ls=p.find("landings");
                if (fs==p.end() || !fs->is_array() || fs->empty() || fs->size()>256 ||
                    ls==p.end() || !ls->is_array() || ls->size()!=fs->size()-1)
                    invalid("Invalid bounded stair topology");
            }
        } else if (kind==Form::rail_v1 || kind==Form::rail_v2 || kind==Form::landing_rail_v3) {
            if (e.properties.contains("flights") || e.properties.contains("landings"))
                invalid("Railing cannot carry stair topology authority");
        } else if (hosted_rails && e.type=="model_phases") {
            const auto& model=e.properties.at("model");
            for (const char* key:{"entity_ids","baseline_ids","alternatives"}) {
                if (!model.contains(key) || !model.at(key).is_array() ||
                    model.at(key).size()>(std::string_view(key)=="alternatives"?max_alternatives:max_phase_members))
                    invalid("Stair phase registry limit or shape invalid");
            }
            const auto contexts=model.at("alternatives").size()+1;
            const auto members=model.at("entity_ids").size();
            if (members>(max_relevant_work-budget.phase_work)/contexts)
                invalid("Stair phase state work limit exceeded");
            budget.phase_work+=members*contexts;
        }
    }
}
using Role=StairChildIdentityRole;
using ChildOwner=StairChildIdentityOwner;
using Children=std::map<std::string,ChildOwner,std::less<>>;
void add_children(Children& children,const StairFlight& stair,const Map& entities) {
    const auto add=[&](const std::string& id,Role role) {
        if (entities.contains(id)) invalid("Stair child identity collides with entity identity");
        if (!children.emplace(id,ChildOwner{stair.id,role}).second)
            invalid("Stair child identity is shared by multiple owners/roles");
    };
    for (const auto& f:stair.flights) add(f.id,Role::flight);
    for (const auto& l:stair.landings) add(l.id,Role::landing);
}
// Identity checks read only the bounded typed topology. Numeric geometry and
// layout are admitted separately by validate_stair_attachment_state for every
// retained map. Reconstructing 10,000 treads here would add no identity evidence.
void child_id(const std::string& id) {
    if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') ||
            c=='-' || c=='_' || c=='.' || c==':';
    })) invalid("Stair child identity must be portable and bounded");
}
bool has_topology(const Map& entities) {
    for (const auto& [id,e]:entities)
        if (e.type=="stair" && form(e)==Form::stair_multi) return true;
    return false;
}
Children identity_children(const Map& entities,std::size_t& child_work) {
    Children children;
    for (const auto& [id,e]:entities) {
        if (e.type!="stair") continue;
        const auto kind=form(e);
        if (kind!=Form::stair_v1 && kind!=Form::stair_multi) continue;
        child_id(id);
        if (e.id!=id) invalid("Stair admission entity map identity mismatch");
        const auto& p=e.properties;
        if (p.contains("host")) invalid("Stair cannot carry railing host authority");
        if (kind==Form::stair_v1) {
            if (p.contains("flights") || p.contains("landings"))
                invalid("Stair topology requires version 2 or 3");
            continue;
        }
        const auto fs=p.find("flights"), ls=p.find("landings");
        if (fs==p.end() || !fs->is_array() || fs->empty() || fs->size()>256 ||
            ls==p.end() || !ls->is_array() || ls->size()!=fs->size()-1)
            invalid("Invalid bounded stair identity topology");
        if (fs->size()+ls->size()>max_relevant_work-child_work)
            invalid("Stair retained child work limit exceeded");
        child_work+=fs->size()+ls->size();
        const auto add=[&](const Json& record,Role role) {
            if (!record.is_object() || !record.contains("id") || !record.at("id").is_string())
                invalid("Stair child record requires a string identity");
            const auto& child=record.at("id").get_ref<const std::string&>(); child_id(child);
            if (entities.contains(child)) invalid("Stair child identity collides with entity identity");
            if (!children.emplace(child,ChildOwner{id,role}).second)
                invalid("Stair child identity is shared by multiple owners/roles");
        };
        std::size_t sum=0;
        for (const auto& flight:*fs) {
            add(flight,Role::flight);
            const auto count=flight.find("riser_count");
            if (count==flight.end() || !count->is_number_integer() || *count<1 || *count>10000)
                invalid("Invalid stair identity flight count");
            sum+=count->get<std::size_t>();
            if (sum>10000) invalid("Invalid aggregate stair identity flight count");
        }
        const auto count=p.find("riser_count");
        if (count==p.end() || !count->is_number_integer() || *count!=sum)
            invalid("Stair identity topology aggregate count differs");
        for (const auto& landing:*ls) add(landing,Role::landing);
    }
    return children;
}
DrawingContext complete_context(const ProjectOrganization& organization,const Entity& e) {
    // Redundant explicit refs are authoring authority, not an invitation to infer
    // missing context from the host or copy it into the document.
    for (const auto* key:{"property_id","building_id","floor_id","layer_id"}) {
        const auto p=e.properties.find(key);
        if (p==e.properties.end() || !p->is_string() || p->get_ref<const std::string&>().empty())
            invalid("Hosted railing and stair require complete explicit organization");
    }
    const auto context=organization.drawing_context(e.id);
    if (!context || !context->complete()) invalid("Hosted railing/stair organization is invalid");
    return *context;
}
void explicit_phase(const Entity& rail,const Entity& host) {
    const auto r=rail.properties.find("phase_id"), h=host.properties.find("phase_id");
    const bool rp=r!=rail.properties.end(), hp=h!=host.properties.end();
    if ((rp && (!r->is_string() || r->get_ref<const std::string&>().empty())) ||
        (hp && (!h->is_string() || h->get_ref<const std::string&>().empty())))
        invalid("Hosted railing/stair explicit phase must be a nonempty string");
    if (rp!=hp || (rp && *r!=*h)) invalid("Hosted railing and stair explicit phases disagree");
}
bool compatible(ModelPhase rail,ModelPhase host) {
    switch (rail) {
    case ModelPhase::existing:return host==ModelPhase::existing;
    case ModelPhase::proposed:return host==ModelPhase::existing || host==ModelPhase::proposed;
    case ModelPhase::demolished:return host==ModelPhase::existing || host==ModelPhase::demolished;
    }
    return false;
}
using PhaseState=std::map<std::string,ModelPhase,std::less<>>;
struct PhaseRegistry { std::vector<PhaseState> states; };
using PhaseOwners=std::map<std::string,std::vector<std::size_t>,std::less<>>;
void validate_phase_pair(const std::vector<PhaseRegistry>& registries,const PhaseOwners& owners,
                         const Entity& rail,const Entity& host) {
    explicit_phase(rail,host);
    const auto r=owners.find(rail.id), h=owners.find(host.id);
    if (r==owners.end() && h==owners.end()) return;
    if (r==owners.end() || h==owners.end() || r->second.size()!=1 || h->second!=r->second)
        invalid("Hosted stair/rail phase registry is ambiguous");
    for (const auto& state:registries.at(r->second.front()).states) {
        const auto rs=state.find(rail.id), hs=state.find(host.id);
        if (rs!=state.end() && (hs==state.end() || !compatible(rs->second,hs->second)))
            invalid("Hosted railing participates without a compatible current stair phase");
    }
}
void coherent_lower_level(const Map& entities,const Entity& e,const StairFlight& stair,
                           const ProjectOrganization& organization) {
    if (!stair.level_connection || !e.properties.contains("vertical_placement") ||
        e.properties.at("vertical_placement").value("mode",std::string())!="level") return;
    const auto context=organization.drawing_context(e.id);
    if (!context || context->floor_id.empty()) invalid("Connected level stair needs a valid floor");
    const auto& floor=entities.at(context->floor_id);
    const auto binding=VerticalLevelBinding::from_json(floor.properties.at("vertical_level_binding"));
    if (binding.graph_entity_id!=stair.level_connection->graph_entity_id ||
        binding.level_id!=stair.level_connection->lower_level_id)
        invalid("Connected stair level placement must use its lower graph level");
}
} // namespace

void validate_stair_attachment_state(const Map& entities) {
    try {
        bool hosted_rails=false, known_objects=false;
        for (const auto& [id,e]:entities) {
            const auto kind=form(e);
            hosted_rails=hosted_rails || kind==Form::rail_v2 || kind==Form::landing_rail_v3;
            known_objects=known_objects || kind!=Form::opaque;
        }
        if (!known_objects) return;
        Budget budget; preflight(entities,budget,hosted_rails);
        Children children;
        std::map<std::string,StairFlight,std::less<>> stairs;
        std::vector<PhaseRegistry> phases;
        PhaseOwners phase_owners;
        std::optional<ProjectOrganization> organization;
        const auto org=[&]() -> const ProjectOrganization& {
            if (!organization) organization=organize_project(entities);
            return *organization;
        };
        for (const auto& [id,e]:entities) {
            const auto kind=form(e);
            if (kind==Form::stair_v1 || kind==Form::stair_multi) {
                // Resolve original host placement once, then decode the derived
                // copy. Never replace the source map with derived coordinates.
                const auto resolved=resolve_vertical_placement(entities,e);
                auto stair=decode_stair_properties(id,resolved.properties);
                add_children(children,stair,entities);
                if (kind==Form::stair_multi) {
                    if (stair.level_connection) coherent_lower_level(entities,e,stair,org());
                    stairs.emplace(id,std::move(stair));
                }
            } else if (hosted_rails && e.type=="model_phases") {
                const auto& model=e.properties.at("model");
                auto registry=ModelPhases::from_json(model);
                for (const auto& member:registry.entity_ids()) {
                    if (!entities.contains(member)) invalid("Stair phase registry has a missing member");
                    phase_owners[member].push_back(phases.size());
                }
                PhaseRegistry cached;
                cached.states.push_back(registry.state(std::nullopt));
                for (const auto& alternative:registry.alternatives())
                    cached.states.push_back(registry.state(alternative.id));
                phases.push_back(std::move(cached));
            }
        }
        for (const auto& [id,e]:entities) {
            const auto kind=form(e);
            if (kind!=Form::rail_v1 && kind!=Form::rail_v2 && kind!=Form::landing_rail_v3) continue;
            const auto rail=decode_railing_properties(id,e.properties);
            if (!rail.host && !rail.landing_host) continue;
            if (e.properties.contains("vertical_placement"))
                invalid("Hosted railing cannot own independent vertical placement");
            const auto s=stairs.find(rail.host?rail.host->stair_id:rail.landing_host->stair_id);
            if (s==stairs.end()) invalid("Hosted railing requires a current canonical multi-flight stair");
            const auto& host=entities.at(s->first);
            const auto rc=complete_context(org(),e), hc=complete_context(org(),host);
            if (rc.property_id!=hc.property_id || rc.building_id!=hc.building_id || rc.floor_id!=hc.floor_id)
                invalid("Hosted railing and stair organization disagree");
            validate_phase_pair(phases,phase_owners,e,host);
            (void)derive_hosted_railing_layout(rail,s->second);
        }
    } catch(const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed stair attachment state: ")+error.what());
    }
}

void validate_stair_identity_transition(const Map& before,const Map& after,
                                       std::span<const RevisionRecord> retained_history) {
    auto ledger=StairIdentityHistory::from_retained_history(retained_history);
    ledger.initialize_if_needed(retained_history,before,after);
    ledger.reserve_state(before);
    ledger.validate_transition(before,after);
}

StairIdentityHistory StairIdentityHistory::from_retained_history(
    std::span<const RevisionRecord> retained_history) {
    StairIdentityHistory result;
    const bool topology=std::any_of(retained_history.begin(),retained_history.end(),
        [](const auto& record) { return has_topology(record.entities); });
    if (!topology) return result;
    result.initialized_=true;
    for (const auto& record:retained_history) result.reserve_state(record.entities);
    return result;
}

void StairIdentityHistory::initialize_if_needed(std::span<const RevisionRecord> retained_history,
                                               const Map& before,const Map& after) {
    if (initialized_ || (!has_topology(before) && !has_topology(after))) return;
    // Prefix reconstruction happens once, at the first v2 topology. Publish
    // only after the complete retained prefix has passed identity reservation.
    StairIdentityHistory result;
    result.initialized_=true;
    for (const auto& record:retained_history) result.reserve_state(record.entities);
    result.reserve_state(before);
    *this=std::move(result);
}

void StairIdentityHistory::validate_transition(const Map& before,const Map& after) const {
    try {
        if (!initialized_) {
            if (has_topology(before) || has_topology(after))
                invalid("Stair identity ledger requires retained-prefix initialization");
            return;
        }
        std::size_t child_work=0;
        const auto live=identity_children(before,child_work), candidate=identity_children(after,child_work);
        for (const auto& [id,owner]:candidate) {
            const auto previous=live.find(id);
            if (previous!=live.end()) {
                if (previous->second!=owner) invalid("Live stair child owner/role cannot be changed");
                const auto retained=children_.find(id);
                if (retained!=children_.end() && retained->second!=owner)
                    invalid("Live stair child conflicts with its retained typed owner");
            } else if (children_.contains(id) || entity_ids_.contains(id) || before.contains(id)) {
                invalid("Introduced stair child identity was already retained");
            }
        }
        for (const auto& [id,e]:after) {
            if (children_.contains(id)) invalid("Retained stair child cannot become an entity identity");
            const auto kind=e.type=="stair"?form(e):Form::opaque;
            if ((kind==Form::stair_v1 || kind==Form::stair_multi) && stair_owner_ids_.contains(id)) {
                const auto old=before.find(id);
                const auto old_kind=old!=before.end() && old->second.type=="stair"
                    ?form(old->second):Form::opaque;
                if (old_kind!=Form::stair_v1 && old_kind!=Form::stair_multi)
                    invalid("Retired stair owner cannot be reused by an authored change");
            }
        }
    } catch(const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed stair identity state/history: ")+error.what());
    }
}

void StairIdentityHistory::reserve_state(const Map& state) {
    try {
        if (!initialized_ && !has_topology(state)) return;
        std::size_t child_work=0;
        const auto children=identity_children(state,child_work);
        std::size_t introduced=0;
        for (const auto& [id,owner]:children) {
            const auto old=children_.find(id);
            if (old!=children_.end()) {
                if (old->second!=owner) invalid("Retained stair child identity has conflicting owner/role");
            } else ++introduced;
            if (entity_ids_.contains(id)) invalid("Retained stair child identity collides with retained entity");
        }
        if (introduced>max_relevant_work-children_.size())
            invalid("Stair retained unique child limit exceeded");
        std::vector<std::string> stair_owners;
        for (const auto& [id,e]:state) {
            if (id.empty() || e.id!=id) invalid("Stair ledger entity map identity mismatch");
            if (children_.contains(id)) invalid("Retained stair child identity collides with retained entity");
            const auto kind=e.type=="stair"?form(e):Form::opaque;
            if (kind==Form::stair_v1 || kind==Form::stair_multi) stair_owners.push_back(id);
        }
        // Prepare rollback storage before mutation; these references stay valid
        // for the call. No full-ledger copy is needed for exception safety.
        std::vector<const std::string*> added_children, added_entities, added_owners;
        added_children.reserve(children.size());
        added_entities.reserve(state.size());
        added_owners.reserve(stair_owners.size());
        try {
            for (const auto& [id,owner]:children)
                if (children_.try_emplace(id,owner).second) added_children.push_back(&id);
            for (const auto& [id,e]:state)
                if (entity_ids_.insert(id).second) added_entities.push_back(&id);
            for (const auto& id:stair_owners)
                if (stair_owner_ids_.insert(id).second) added_owners.push_back(&id);
        } catch(...) {
            for (const auto* id:added_children) children_.erase(*id);
            for (const auto* id:added_entities) entity_ids_.erase(*id);
            for (const auto* id:added_owners) stair_owner_ids_.erase(*id);
            throw;
        }
        initialized_=true;
    } catch(const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed retained stair identity state: ")+error.what());
    }
}
} // namespace sketch

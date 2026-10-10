#include "sketch/architectural_drawing_removal.hpp"

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/wall_join_removal.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t selection_limit=1000, destination_limit=4096, byte_limit=1024*1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Architectural/drawing removal: "+reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            c=='-' || c=='_' || c=='.' || c==':';
    })) reject("identity requires 1..128 supported ASCII characters");
}
template<class T> void canonical(const std::vector<T>& rows) {
    if (!std::is_sorted(rows.begin(),rows.end()) || std::adjacent_find(rows.begin(),rows.end())!=rows.end())
        reject("selected roots and qualified components must be sorted and unique");
}
void fields(const Json& value,std::initializer_list<const char*> wanted) {
    if (!value.is_object() || value.size()!=wanted.size()) reject("intent has an unsupported field set");
    for (const auto* key:wanted) if (!value.contains(key)) reject("intent lacks a required field");
}
void wire_budget(const Json& value,std::size_t& nodes,std::size_t& bytes,std::size_t depth=0) {
    if (depth>8 || ++nodes>32768) reject("intent nesting/node budget exceeded");
    const auto text=[&](const std::string& string) {
        if (string.size()>byte_limit-bytes) reject("intent string/key budget exceeded");
        bytes+=string.size();
    };
    if (value.is_binary() || value.is_discarded()) reject("intent has an unsupported JSON value");
    if (value.is_string()) text(value.get_ref<const std::string&>());
    if (value.is_object()) for (const auto& [key,child]:value.items()) {
        text(key); wire_budget(child,nodes,bytes,depth+1);
    } else if (value.is_array()) for (const auto& child:value) wire_budget(child,nodes,bytes,depth+1);
}
bool has_architectural(const ArchitecturalDrawingRemovalIntent& intent) {
    return !intent.architectural.object_ids.empty() || !intent.architectural.components.empty();
}
bool has_drawing(const ArchitecturalDrawingRemovalIntent& intent) {
    return !intent.drawing.owner_ids.empty() || !intent.drawing.annotations.empty();
}
void bounded(const ArchitecturalDrawingRemovalIntent& intent) {
    const auto& architectural=intent.architectural;
    std::size_t count=0;
    for (const auto size:{architectural.object_ids.size(),architectural.components.size(),
            intent.drawing.owner_ids.size(),intent.drawing.annotations.size()}) {
        if (size>selection_limit-count) reject("selection exceeds 1000 aggregate roots/components/rows");
        count+=size;
    }
    if (!count) reject("selection is empty");
    canonical(architectural.object_ids); canonical(architectural.components);
    std::set<std::string,std::less<>> roots;
    for (const auto& id:architectural.object_ids) { identity(id); roots.insert(id); }
    for (const auto& id:intent.drawing.owner_ids) {
        identity(id);
        if (roots.contains(id)) reject("an owner occurs in both selection lanes");
    }
    for (const auto& [catalog,local]:architectural.components) { identity(catalog); identity(local); }
    if (!has_architectural(intent) && (intent.allow_manufactured_opening_hosts ||
        !architectural.roof_additional_identities.empty())) reject("architectural options require an actual architectural selection");
    if (architectural.roof_additional_identities.size()>destination_limit) reject("roof destination owner budget exceeded");
    std::set<std::string,std::less<>> fresh;
    for (const auto& [owner,ids]:architectural.roof_additional_identities) {
        identity(owner);
        if (ids.empty() || ids.size()>destination_limit-fresh.size()) reject("roof destination budget exceeded");
        for (const auto& id:ids) { identity(id); if (!fresh.insert(id).second) reject("roof destinations overlap"); }
    }
    for (const auto* token:{"version","architectural","drawing","allow_manufactured_opening_hosts",
            "object_ids","components","roof_additional_identities","catalog_id","instance_id",
            "owner_ids","annotations","owner_id","kind","child_id"})
        if (fresh.contains(token)) reject("roof destination borrows an intent field token");
    for (const auto& [owner,ids]:architectural.roof_additional_identities) {
        (void)ids;
        if (fresh.contains(owner)) reject("roof destination borrows a source slot owner");
    }
    for (const auto& id:roots) if (fresh.contains(id)) reject("roof destination borrows a selected owner");
    for (const auto& id:intent.drawing.owner_ids) if (fresh.contains(id)) reject("roof destination borrows a drawing owner");
    for (const auto& target:intent.drawing.annotations)
        if (fresh.contains(target.owner_id) || fresh.contains(target.child_id)) reject("roof destination borrows an annotation target");
    for (const auto& [catalog,local]:architectural.components)
        if (fresh.contains(catalog) || fresh.contains(local)) reject("roof destination borrows a qualified component");
    if (has_drawing(intent)) (void)encode_drawing_selection_removal_intent(intent.drawing);
}
bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
} // namespace

Json encode_architectural_drawing_removal_intent(const ArchitecturalDrawingRemovalIntent& intent) {
    bounded(intent);
    Json components=Json::array();
    for (const auto& [catalog,local]:intent.architectural.components)
        components.push_back({{"catalog_id",catalog},{"instance_id",local}});
    Json value={{"version",1},
        {"architectural",{{"object_ids",intent.architectural.object_ids},{"components",std::move(components)},
            {"roof_additional_identities",intent.architectural.roof_additional_identities}}},
        {"drawing",has_drawing(intent) ? encode_drawing_selection_removal_intent(intent.drawing) : Json(nullptr)},
        {"allow_manufactured_opening_hosts",intent.allow_manufactured_opening_hosts}};
    if (value.dump().size()>byte_limit) reject("intent exceeds one MiB");
    return value;
}

ArchitecturalDrawingRemovalIntent decode_architectural_drawing_removal_intent(const Json& value) {
    try {
        std::size_t nodes=0,bytes=0;
        wire_budget(value,nodes,bytes);
        if (value.dump().size()>byte_limit) reject("intent exceeds one MiB");
        fields(value,{"version","architectural","drawing","allow_manufactured_opening_hosts"});
        if (!value.at("version").is_number_integer() || value.at("version")!=1 ||
            !value.at("allow_manufactured_opening_hosts").is_boolean()) reject("intent version or options are invalid");
        const auto& architectural=value.at("architectural");
        fields(architectural,{"object_ids","components","roof_additional_identities"});
        const auto& objects=architectural.at("object_ids");
        const auto& components=architectural.at("components");
        const auto& destinations=architectural.at("roof_additional_identities");
        if (!objects.is_array() || objects.size()>selection_limit || !components.is_array() ||
            components.size()>selection_limit-objects.size() || !destinations.is_object() ||
            destinations.size()>destination_limit) reject("intent inventories are invalid");
        ArchitecturalDrawingRemovalIntent result;
        for (const auto& id:objects) {
            if (!id.is_string()) reject("selected owner must be an identity");
            result.architectural.object_ids.push_back(id.get<std::string>());
        }
        for (const auto& row:components) {
            fields(row,{"catalog_id","instance_id"});
            if (!row.at("catalog_id").is_string() || !row.at("instance_id").is_string())
                reject("component requires actual qualified identities");
            result.architectural.components.emplace_back(row.at("catalog_id").get<std::string>(),row.at("instance_id").get<std::string>());
        }
        std::size_t count=0;
        for (const auto& [owner,ids]:destinations.items()) {
            if (!ids.is_array() || ids.empty() || ids.size()>destination_limit-count) reject("roof destination inventory is invalid");
            count+=ids.size();
            auto& rows=result.architectural.roof_additional_identities[owner];
            for (const auto& id:ids) {
                if (!id.is_string()) reject("roof destination must be an identity");
                rows.push_back(id.get<std::string>());
            }
        }
        if (!value.at("drawing").is_null()) result.drawing=decode_drawing_selection_removal_intent(value.at("drawing"));
        result.allow_manufactured_opening_hosts=value.at("allow_manufactured_opening_hosts").get<bool>();
        if (encode_architectural_drawing_removal_intent(result).dump()!=value.dump()) reject("intent is not canonical");
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed intent: ")+error.what()); }
}

DrawingSelectionRemovalEntities replay_architectural_drawing_removal(const DocumentSnapshot& source,
    const ArchitecturalDrawingRemovalIntent& intent) {
    (void)encode_architectural_drawing_removal_intent(intent);
    if (!source.is_editable()) reject("captured source is read-only");
    validate_physical_wall_join_removal_identity_lifetime(source,intent.architectural.roof_additional_identities);
    auto candidate=has_architectural(intent) && has_drawing(intent)
        ? replay_drawing_selection_removal_with_architectural(source.entities(),intent.drawing,
            intent.architectural,intent.allow_manufactured_opening_hosts,source.uses_active_phase_constraints())
        : has_architectural(intent)
            ? replay_architectural_selection_removal(source.entities(),intent.architectural,intent.allow_manufactured_opening_hosts)
            : replay_drawing_selection_removal(source.entities(),intent.drawing,source.uses_active_phase_constraints());
    const auto original=embedded_assembly_presentation_ids(source.entities());
    for (const auto& [key,alias]:embedded_assembly_presentation_ids(candidate)) {
        const auto before=original.find(key);
        if (before==original.end() || before->second!=alias) reject("removal changed a surviving component alias");
    }
    return candidate;
}

ApplyEntityChanges prepare_architectural_drawing_removal(const DocumentSnapshot& source,
    const ArchitecturalDrawingRemovalIntent& intent,const std::string& message) {
    const auto expected=replay_architectural_drawing_removal(source,intent);
    ApplyEntityChanges command{source.revision(),{}, {},message};
    for (const auto& [id,entity]:source.entities()) {
        const auto after=expected.find(id);
        if (after==expected.end()) command.entity_changes.push_back(EntityChange::erase(id));
        else if (!exact(entity,after->second)) command.entity_changes.push_back(EntityChange::upsert(after->second));
    }
    for (const auto& [id,entity]:expected)
        if (!source.entities().contains(id)) command.entity_changes.push_back(EntityChange::upsert(entity));
    if (command.entity_changes.empty() || command.entity_changes.size()>4096) reject("complete removal change inventory is invalid");
    const auto candidate=Document::preview_command(source,Command{command});
    if (candidate.entities().size()!=expected.size()) reject("complete removal preview changed its inventory");
    for (const auto& [id,entity]:expected) {
        const auto actual=candidate.entities().find(id);
        if (actual==candidate.entities().end() || !exact(entity,actual->second)) reject("complete removal preview differs from actual-source reconstruction");
    }
    if (candidate.assets()!=source.assets()) reject("removal changed assets");
    return command;
}
} // namespace sketch

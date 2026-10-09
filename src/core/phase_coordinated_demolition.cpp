#include "sketch/phase_coordinated_demolition.hpp"

#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_stair_demolition.hpp"
#include "sketch/phase_stair_demolition_retirement.hpp"
#include "sketch/phase_structural_replacement.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t source_limit = 64 * 1024 * 1024;
constexpr std::size_t node_limit = 4 * 1024 * 1024;
constexpr std::array<const char*, 5> families{
    "opening_authoring", "roof_authoring", "slab_authoring", "structural_authoring", "stair_authoring"};

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Coordinated demolition: " + reason);
}

void identity(const std::string& value) {
    if (value.empty() || value.size() > 128 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("identity must contain 1..128 supported ASCII characters");
}

// Validate raw JSON before any dump, recursive family codec or native replay.
// The aggregate counters span all children or all actual source envelopes.
struct JsonBudget {
    std::size_t byte_limit;
    bool proof;
    std::size_t nodes{}, bytes{};

    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) invalid("aggregate JSON string byte budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) invalid("aggregate JSON node/nesting budget exceeded");
        if (value.is_binary() || value.is_discarded()) invalid("raw JSON cannot contain binary or discarded values");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) invalid("raw JSON number must be finite");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (proof && value.is_structured() && value.size() > 4096) invalid("proof collection budget exceeded");
        if (value.is_object()) {
            for (const auto& [key, child] : value.items()) {
                text(key);
                // Nested coordination is refused before a historical decoder
                // can enter either current or future coordinated recursion.
                if (proof && (key == "coordinated_replacements" || key == "coordinated_demolition"))
                    invalid("nested coordinated authoring is forbidden");
                read(child, depth + 1);
            }
        } else if (value.is_array()) {
            for (const auto& child : value) read(child, depth + 1);
        }
    }
};

void source_budget(const Entities& source) {
    if (source.size() > 250000) invalid("source entity budget exceeded");
    JsonBudget budget{source_limit, false};
    std::size_t serialized{};
    const auto reserve = [&](std::size_t bytes) {
        if (bytes > source_limit - serialized) invalid("serialized source map byte budget exceeded");
        serialized += bytes;
    };
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
        // Validate opaque type UTF-8 as well as all raw payloads. Reserve full
        // entity/map framing; numeric arrays also consume serialized bytes.
        reserve(2 * id.size() + Json(entity.type).dump().size() + 128);
        reserve(entity.properties.dump().size());
        reserve(entity.extensions.dump().size());
    }
}

void message_only(const ConstraintAuthoringIntent& intent) {
    if (intent.wall_resize || intent.wall_geometry_move || intent.wall_curve_construction ||
        intent.boundary_resize || intent.boundary_vertex_move || intent.exterior_corner_move ||
        intent.exterior_segment_resize || intent.exterior_segment_arc || intent.measured_stroke_resize ||
        intent.measured_stroke_vertex_move || intent.measured_stroke_transform || intent.joint_translation ||
        !intent.relation_mutations.empty() || intent.relation_anchor || !intent.relation_move_connected_walls)
        invalid("demolition permits only a semantic message without geometry or relationship authority");
    if (intent.message.size() > 4096 || intent.message.find('\0') != std::string::npos)
        invalid("semantic message budget exceeded");
    (void)Json(intent.message).dump();
}

void same_binding(const PhaseConstraintAuthoringIntent& child, const PhaseConstraintAuthoringIntent& enclosing) {
    if (child.expected_revision != enclosing.expected_revision ||
        child.source_snapshot_digest != enclosing.source_snapshot_digest ||
        child.source_authoring_digest != enclosing.source_authoring_digest ||
        child.source_entities_digest != enclosing.source_entities_digest ||
        child.source_saved_revision != enclosing.source_saved_revision ||
        child.phase_selections.dump() != enclosing.phase_selections.dump())
        invalid("child differs from the enclosing actual source binding and raw saved choices");
}

struct DemolitionChoice {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> roots;
};

DemolitionChoice choice(const PhaseConstraintAuthoringIntent& child, std::size_t family, const Json& wire) {
    const auto& version = wire.at("version");
    if (family == 0 && version == 3) {
        const auto leaf = decode_phase_opening_demolition_intent(child.opening_demolition);
        return {leaf.registry_id, leaf.alternative_id, leaf.opening_ids};
    }
    if (family == 1 && version == 4) {
        const auto leaf = decode_phase_roof_replacement_authoring(child.roof_replacement);
        if (!leaf.demolition || !leaf.roof_profiles.empty() || !leaf.roof_opening_edits.empty() ||
            !leaf.roof_edits.empty() || !leaf.ordinary_roof_edits.empty())
            invalid("roof child requires demolition without body/profile/opening/edit authority");
        return {leaf.registry_id, leaf.alternative_id, leaf.seed_roof_ids};
    }
    if (family == 2 && version == 6) {
        const auto leaf = decode_slab_demolition_intent(child.slab_demolition);
        return {leaf.registry_id, leaf.alternative_id, leaf.slab_ids};
    }
    if (family == 3 && version == 9) {
        const auto leaf = decode_phase_structural_replacement_authoring(child.structural_replacement);
        if (!leaf.demolition || !leaf.edits.empty() || !leaf.identities.empty() ||
            leaf.complete_hosted || !leaf.hosted_instance_identities.empty())
            invalid("structural child requires demolition without replacement or edit authority");
        return {leaf.registry_id, leaf.alternative_id, leaf.seed_object_ids};
    }
    if (family == 4 && version == 11) {
        const auto leaf = decode_stair_demolition_intent(child.stair_demolition);
        return {leaf.registry_id, leaf.alternative_id, leaf.selected_object_ids};
    }
    if (family == 4 && version == 13) {
        const auto leaf = decode_stair_demolition_retirement_intent(child.stair_demolition_retirement);
        return {leaf.registry_id, leaf.alternative_id, leaf.selected_object_ids};
    }
    invalid("family slot contains a foreign historical authoring dialect");
}

std::vector<PhaseConstraintAuthoringIntent> components(const Json& value, const PhaseConstraintAuthoringIntent& enclosing) {
    JsonBudget budget{proof_limit, true};
    budget.read(value);
    if (value.dump().size() > proof_limit) invalid("proof byte budget exceeded");
    if (!value.is_object() || value.size() != 6 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("inner version one requires exactly version and five family fields");
    for (const auto* key : families) if (!value.contains(key)) invalid("required family field is missing");
    message_only(enclosing.intent);
    // Root enforces outer exclusivity too; this public codec cannot validate a
    // mixed enclosure by ignoring a separately supplied historical operation.
    if (!enclosing.wall_replacement.is_null() || !enclosing.opening_demolition.is_null() ||
        !enclosing.roof_replacement.is_null() || !enclosing.slab_replacement.is_null() ||
        !enclosing.slab_demolition.is_null() || !enclosing.coordinated_replacements.is_null() ||
        !enclosing.structural_replacement.is_null() || !enclosing.stair_demolition.is_null() ||
        !enclosing.stair_replacement.is_null() || !enclosing.stair_demolition_retirement.is_null())
        invalid("enclosing coordinated demolition cannot borrow another operation");
    // Bind raw enclosing choices only after guarding their shape/number types.
    JsonBudget selections_budget{proof_limit, true};
    selections_budget.read(enclosing.phase_selections);
    if (enclosing.phase_selections.dump().size() > proof_limit) invalid("saved choice byte budget exceeded");

    std::vector<PhaseConstraintAuthoringIntent> result;
    std::optional<std::pair<std::string, std::string>> saved_choice;
    std::set<std::string, std::less<>> targets;
    for (std::size_t family = 0; family < families.size(); ++family) {
        const auto& wire = value.at(families[family]);
        if (wire.is_null()) continue;
        // Inspect the outer tag BEFORE entering the full historical codec.
        if (!wire.is_object() || !wire.contains("version") || !wire.at("version").is_number_integer() ||
            (family == 0 && wire.at("version") != 3) ||
            (family == 1 && wire.at("version") != 4) ||
            (family == 2 && wire.at("version") != 6) ||
            (family == 3 && wire.at("version") != 9) ||
            (family == 4 && wire.at("version") != 11 && wire.at("version") != 13))
            invalid("family slot requires its full historical demolition authoring envelope");
        auto child = decode_phase_constraint_authoring_intent(wire);
        if (encode_phase_constraint_authoring_intent(child).dump() != wire.dump())
            invalid("historical child envelope is not canonical");
        message_only(child.intent);
        same_binding(child, enclosing);
        const auto typed = choice(child, family, wire);
        identity(typed.registry_id); identity(typed.alternative_id);
        const auto current_choice = std::pair{typed.registry_id, typed.alternative_id};
        if (saved_choice && *saved_choice != current_choice)
            invalid("all families must name the same typed registry and alternative");
        saved_choice = current_choice;
        if (typed.roots.empty()) invalid("present demolition family cannot be empty");
        for (const auto& root : typed.roots) {
            identity(root);
            if (!targets.insert(root).second) invalid("demolition roots must be unique across all families");
            if (targets.size() > 4096) invalid("aggregate demolition root budget exceeded");
        }
        result.push_back(std::move(child));
    }
    if (result.size() < 2) invalid("coordinated demolition requires at least two distinct families");
    return result;
}
} // namespace

Json encode_phase_coordinated_demolition(const Json& value, const PhaseConstraintAuthoringIntent& enclosing) {
    try {
        (void)components(value, enclosing);
        return value;
    } catch (const Json::exception& error) {
        invalid(std::string("malformed proof: ") + error.what());
    }
}

std::vector<PhaseConstraintAuthoringIntent> phase_coordinated_demolition_components(
    const Json& value, const PhaseConstraintAuthoringIntent& enclosing) {
    try {
        return components(value, enclosing);
    } catch (const Json::exception& error) {
        invalid(std::string("malformed proof: ") + error.what());
    }
}

Entities replay_phase_coordinated_demolition(const Entities& source, const PhaseConstraintAuthoringIntent& enclosing) {
    try {
        source_budget(source);
        const auto leaves = components(enclosing.coordinated_demolition, enclosing);
        std::vector<Entities> candidates;
        candidates.reserve(leaves.size());
        for (const auto& leaf : leaves) {
            auto candidate = replay_phase_constraint_authoring(source, encode_phase_constraint_authoring_intent(leaf));
            source_budget(candidate);
            candidates.push_back(std::move(candidate));
        }
        auto result = compose_phase_demolition_candidates(source, candidates);
        source_budget(result);
        return result;
    } catch (const Json::exception& error) {
        invalid(std::string("malformed actual replay source/proof: ") + error.what());
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
} // namespace sketch

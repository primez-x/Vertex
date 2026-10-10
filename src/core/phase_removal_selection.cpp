#include "sketch/phase_removal_selection.hpp"

#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_coordinated_demolition.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_stair_demolition.hpp"
#include "sketch/phase_stair_demolition_retirement.hpp"
#include "sketch/phase_structural_replacement.hpp"
#include "sketch/phase_wall_demolition_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t selection_limit = 1000;
constexpr std::size_t semantic_depth_limit = 8;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Phase removal selection: " + reason);
}

// Conservative escaped-byte accounting precedes serialization and all codecs.
// Counters span the entire original enclosure, including nested room decisions.
struct ProofBudget {
    std::size_t bytes{}, nodes{};
    void reserve(std::size_t amount) {
        if (amount > proof_limit - bytes) invalid("proof byte budget exceeded");
        bytes += amount;
    }
    void text(const std::string& value) {
        if (value.size() > (proof_limit - bytes) / 6) invalid("proof string budget exceeded");
        reserve(value.size() * 6);
        reserve(2);
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 65536) invalid("proof nesting/node budget exceeded");
        reserve(32);
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>())))
            invalid("unsupported proof scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) {
            for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
        } else if (value.is_array()) {
            for (const auto& child : value) read(child, depth + 1);
        }
    }
};

bool exact(const Json& left, const Json& right) {
    return left == right && left.dump() == right.dump();
}

// Check typed fields before command_to_json can inspect a competing proof or
// silently project away an irrelevant completion flag. No payload copies occur.
void pure_outer34(const ApplyBoundaryConstraintChanges& c) {
    if (!c.phase_constraint_authoring_completion || c.phase_constraint_authoring_intent.is_null() ||
        c.message.size() > 1024 || !c.boundary_edits.empty() || !c.wall_edits.empty() || !c.entity_changes.empty() ||
        !c.physical_entity_changes.empty() || !c.exterior_source_edits.empty() || !c.supplemental_entity_changes.empty() ||
        !c.supplemental_asset_changes.empty() || !c.measured_stroke_edits.empty() || !c.dimension_placement_moves.empty() ||
        !c.selection_entity_changes.empty() || c.selection_completion || c.exterior_source_completion ||
        c.supplemental_source_completion || c.supplemental_asset_reference_completion || c.rigid_wall_transform_completion ||
        c.measured_source_completion || c.dimension_placement_completion || c.rigid_group_completion || c.rigid_group_transform ||
        c.wall_split || c.wall_merge || c.exterior_corner_move || c.exterior_segment_resize || c.exterior_segment_arc ||
        c.joint_translation_completion || c.joint_translation || c.room_review_completion || !c.room_review_intent.is_null() ||
        c.room_review_geometry_completion || !c.room_review_geometry_proof.is_null() || c.room_review_batch_completion ||
        !c.room_review_additional_intents.empty() || c.phase_room_review_completion || !c.phase_room_review_intent.is_null() ||
        c.wall_dimension_completion || c.curve_construction_completion || c.disto_measurement_completion || c.disto_measurement ||
        c.independent_drawing_removal_completion || !c.independent_drawing_removal_intent.is_null() ||
        c.wall_group_scale_completion || c.wall_group_scale || c.mixed_selection_removal_completion ||
        !c.mixed_selection_removal_intent.is_null() || c.ordinary_selection_removal_completion ||
        !c.ordinary_selection_removal_intent.is_null())
        invalid("requires an exclusive complete outer34 demolition command");
    ProofBudget budget;
    budget.reserve(256);
    budget.text(c.message);
    budget.read(c.phase_constraint_authoring_intent);
}

void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("invalid explicit selection identity");
}

struct Selection {
    std::set<std::string, std::less<>> objects;
    std::set<std::pair<std::string, std::string>> components;

    void count() const {
        if (objects.size() + components.size() > selection_limit) invalid("aggregate selection budget exceeded");
    }
    void add(const std::vector<std::string>& ids) {
        if (ids.size() > selection_limit) invalid("leaf selection budget exceeded");
        for (std::size_t i = 0; i < ids.size(); ++i) {
            identity(ids[i]);
            if (i && ids[i - 1] >= ids[i]) invalid("leaf object roots must be ascending and unique");
            if (!objects.insert(ids[i]).second) invalid("explicit object roots repeat across demolition lanes");
            count();
        }
    }
    void add(const std::vector<std::pair<std::string, std::string>>& keys) {
        if (keys.size() > selection_limit) invalid("leaf component budget exceeded");
        for (std::size_t i = 0; i < keys.size(); ++i) {
            identity(keys[i].first); identity(keys[i].second);
            if (i && keys[i - 1] >= keys[i]) invalid("leaf components must be ascending and unique");
            if (!components.insert(keys[i]).second) invalid("explicit component roots repeat across demolition lanes");
            count();
        }
    }
};

const Json& field(const Json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) invalid("required demolition field is missing");
    return value.at(key);
}

void preflight_ids(const Json& value, std::size_t& count) {
    if (!value.is_array() || value.size() > selection_limit - count)
        invalid("aggregate explicit selection budget exceeded");
    count += value.size();
}

void preflight_ordinary(const Json& value, std::size_t& count) {
    preflight_ids(field(value, "object_ids"), count);
    preflight_ids(field(value, "components"), count);
}

// A fixed grammar walk, never a search for identity-looking JSON keys. Count
// only known user-selection fields before any full codec allocates its leaves.
// The existing codecs subsequently validate every field, version and identity.
void preflight_selection(const Json& proof, std::size_t& count, std::size_t depth = 0) {
    if (depth > semantic_depth_limit) invalid("demolition enclosure depth exceeded");
    const auto& version = field(proof, "version");
    if (!version.is_number_integer()) invalid("requires a known demolition envelope version");
    if (version == 3) {
        preflight_ids(field(field(proof, "opening_demolition"), "opening_ids"), count);
    } else if (version == 6) {
        preflight_ids(field(field(proof, "slab_demolition"), "slab_ids"), count);
    } else if (version == 11) {
        preflight_ids(field(field(proof, "stair_demolition"), "selected_object_ids"), count);
    } else if (version == 13) {
        preflight_ids(field(field(proof, "stair_demolition_retirement"), "selected_object_ids"), count);
    } else if (version == 4 || version == 9) {
        const auto& leaf = field(proof, version == 4 ? "roof_replacement" : "structural_replacement");
        const auto& demolition = field(leaf, "demolition");
        if (!demolition.is_boolean() || demolition != true) invalid("replacement proof is not demolition");
        preflight_ids(field(leaf, version == 4 ? "seed_roof_ids" : "seed_object_ids"), count);
    } else if (version == 15) {
        const auto& coordinated = field(proof, "coordinated_demolition");
        const auto& inner_version = field(coordinated, "version");
        if (!inner_version.is_number_integer() || inner_version < 1 || inner_version > 7)
            invalid("unsupported coordinated demolition version");
        for (const auto* family : {"opening_authoring", "roof_authoring", "slab_authoring", "structural_authoring", "stair_authoring"}) {
            const auto& child = field(coordinated, family);
            if (!child.is_null()) preflight_selection(child, count, depth + 1);
        }
        if (inner_version != 1) {
            const auto& ordinary = field(coordinated, "ordinary_removal");
            if (!ordinary.is_null()) preflight_ordinary(ordinary, count);
        }
        if (inner_version >= 5) preflight_ids(field(coordinated, "ordinary_opening_ids"), count);
    } else if (version == 16) {
        const auto& wall = field(proof, "wall_demolition");
        const auto& inner_version = field(wall, "version");
        if (!inner_version.is_number_integer() || inner_version < 1 || inner_version > 3)
            invalid("unsupported wall demolition version");
        preflight_ids(field(field(wall, "wall_demolition"), "wall_ids"), count);
        if (inner_version != 1) preflight_ids(field(wall, "ordinary_wall_ids"), count);
        preflight_ordinary(field(wall, "ordinary"), count);
        preflight_ids(field(wall, "opening_ids"), count);
        const auto& other = field(wall, "other_authoring");
        if (!other.is_null()) preflight_selection(other, count, depth + 1);
    } else {
        invalid("unsupported phase selection leaf; requires explicit demolition authority");
    }
}

void extract(const PhaseConstraintAuthoringIntent& proof,
    const PhaseConstraintAuthoringIntent& actual_binding, Selection& selected, std::size_t depth) {
    if (depth > semantic_depth_limit) invalid("demolition enclosure depth exceeded");

    // Reconstruct the entire admitted phase envelope from the actual snapshot,
    // a message-only intent and exactly one recognized deletion operation.
    // Equality proves all source/history/save/phase choices and empty siblings.
    auto expected = actual_binding;
    expected.intent.message = proof.intent.message;
    if (!proof.opening_demolition.is_null()) {
        const auto leaf = decode_phase_opening_demolition_intent(proof.opening_demolition);
        expected.opening_demolition = encode_phase_opening_demolition_intent(leaf);
        selected.add(leaf.opening_ids);
    } else if (!proof.slab_demolition.is_null()) {
        const auto leaf = decode_slab_demolition_intent(proof.slab_demolition);
        expected.slab_demolition = encode_slab_demolition_intent(leaf);
        selected.add(leaf.slab_ids);
    } else if (!proof.stair_demolition.is_null()) {
        const auto leaf = decode_stair_demolition_intent(proof.stair_demolition);
        expected.stair_demolition = encode_stair_demolition_intent(leaf);
        selected.add(leaf.selected_object_ids);
    } else if (!proof.stair_demolition_retirement.is_null()) {
        const auto leaf = decode_stair_demolition_retirement_intent(proof.stair_demolition_retirement);
        expected.stair_demolition_retirement = encode_stair_demolition_retirement_intent(leaf);
        selected.add(leaf.selected_object_ids);
    } else if (!proof.structural_replacement.is_null()) {
        const auto leaf = decode_phase_structural_replacement_authoring(proof.structural_replacement);
        if (!leaf.demolition || !leaf.edits.empty() || !leaf.identities.empty() ||
            leaf.complete_hosted || !leaf.hosted_instance_identities.empty())
            invalid("structural proof must be demolition only");
        expected.structural_replacement = encode_phase_structural_replacement_authoring(leaf);
        selected.add(leaf.seed_object_ids);
    } else if (!proof.roof_replacement.is_null()) {
        const auto leaf = decode_phase_roof_replacement_authoring(proof.roof_replacement);
        if (!leaf.demolition || !leaf.roof_profiles.empty() || !leaf.roof_opening_edits.empty() ||
            !leaf.roof_edits.empty() || !leaf.ordinary_roof_edits.empty())
            invalid("roof proof must be demolition only");
        expected.roof_replacement = encode_phase_roof_replacement_authoring(leaf);
        selected.add(leaf.seed_roof_ids);
    } else if (!proof.coordinated_demolition.is_null()) {
        expected.coordinated_demolition = encode_phase_coordinated_demolition(proof.coordinated_demolition, proof);
        for (const auto& child : phase_coordinated_demolition_components(proof.coordinated_demolition, proof))
            extract(child, actual_binding, selected, depth + 1);
        if (const auto ordinary = phase_coordinated_demolition_ordinary_removal(proof.coordinated_demolition, proof)) {
            selected.add(ordinary->object_ids);
            selected.add(ordinary->components);
        }
        selected.add(phase_coordinated_demolition_ordinary_openings(proof.coordinated_demolition, proof));
    } else if (!proof.wall_demolition.is_null()) {
        const auto leaf = decode_phase_wall_demolition_authoring(proof.wall_demolition);
        expected.wall_demolition = encode_phase_wall_demolition_authoring(leaf);
        selected.add(leaf.wall_demolition.wall_ids);
        selected.add(leaf.ordinary_wall_ids);
        selected.add(leaf.ordinary.object_ids);
        selected.add(leaf.ordinary.components);
        selected.add(leaf.opening_ids);
        if (!leaf.other_authoring.is_null())
            extract(decode_phase_constraint_authoring_intent(leaf.other_authoring), actual_binding, selected, depth + 1);
    } else {
        invalid("unsupported phase selection leaf; requires explicit demolition authority");
    }
    if (!exact(encode_phase_constraint_authoring_intent(expected), encode_phase_constraint_authoring_intent(proof)))
        invalid("demolition proof differs from actual source or borrows sibling authoring authority");
}
} // namespace

void preflight_phase_removal_selection_proof(const Json& proof) {
    ProofBudget budget;budget.read(proof);
    std::size_t count=0;preflight_selection(proof,count);
}

ArchitecturalSelectionRemovalIntent phase_removal_selection_authority(
    const DocumentSnapshot& source, const ApplyBoundaryConstraintChanges& pure_phase_command) {
    pure_outer34(pure_phase_command);
    preflight_phase_removal_selection_proof(pure_phase_command.phase_constraint_authoring_intent);
    const auto wire = command_to_json(Command{pure_phase_command});
    const auto proof = decode_phase_constraint_authoring_intent(pure_phase_command.phase_constraint_authoring_intent);
    if (pure_phase_command.expected_revision != source.revision() ||
        pure_phase_command.message != proof.intent.message)
        invalid("outer command differs from actual revision or semantic message");
    const Json expected_wire{{"version", 34}, {"kind", "apply_boundary_constraint_changes"},
        {"expected_revision", source.revision()}, {"message", proof.intent.message},
        {"phase_constraint_authoring_completion", true},
        {"phase_constraint_authoring_intent", encode_phase_constraint_authoring_intent(proof)}};
    if (!exact(wire, expected_wire)) invalid("command must retain the exact canonical outer34 envelope");

    ConstraintAuthoringIntent empty;
    empty.message = proof.intent.message;
    const auto actual_binding = make_phase_constraint_authoring_intent(source, empty);
    Selection selected;
    extract(proof, actual_binding, selected, 0);
    if (selected.objects.empty() && selected.components.empty()) invalid("demolition selection cannot be empty");
    ArchitecturalSelectionRemovalIntent result;
    result.object_ids.assign(selected.objects.begin(), selected.objects.end());
    result.components.assign(selected.components.begin(), selected.components.end());
    return result;
}

} // namespace sketch

#pragma once

#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/phase_slab_profile_edit.hpp"
#include "sketch/phase_wall_profile_edit.hpp"
#include "sketch/structural_profile_edit.hpp"
#include "sketch/stair_profile_edit.hpp"

#include <set>

namespace sketch {

enum class SelectionPropertyFamily { wall, slab, hosted_opening, roof, column, beam, stair, railing };
enum class SelectionPropertyUnchangedPolicy { reject, retain };

struct SelectionPropertyTarget {
    std::string entity_id;
    SelectionPropertyFamily family;
};

// Every selected owner has exactly one typed intent, including retained
// same-value selections. The roster and each family lane are sorted by actual
// source entity ID. There is no raw candidate, arbitrary property map, identity
// generation, rehosting, family conversion or phase replacement authority.
struct SelectionPropertyEditRequest {
    std::vector<SelectionPropertyTarget> selected;
    std::vector<WallProfileEditIntent> walls;
    std::vector<SlabProfileEditIntent> slabs;
    std::vector<HostedOpeningProfileEditIntent> hosted_openings;
    std::vector<RoofProfileEditIntent> roofs;
    std::vector<StructuralProfileEditIntent> structural_profiles;
    std::vector<StairProfileEditIntent> stair_profiles;
    SelectionPropertyUnchangedPolicy unchanged_policy{SelectionPropertyUnchangedPolicy::reject};
};

// Sealed original-source preparation only. Its copied drawing snapshot retains
// history/save metadata, is read-only, and carries no Document admission token.
// Walls and hosted openings retain physical rooms for mandatory enclosing room
// review. The preview, request and leaf maps grant no command/publication power.
class SelectionPropertyEditPreparation final {
public:
    [[nodiscard]] const nlohmann::json& request() const noexcept;
    [[nodiscard]] const DocumentSnapshot& preview_snapshot() const noexcept;
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept;
    // Sorted actual wall roots and opening hosts, including invariant selections.
    // This roster identifies physical dependencies; it is not room review proof.
    [[nodiscard]] const std::set<std::string, std::less<>>& affected_wall_ids() const noexcept;
    // Complete wall/slab/opening/roof/structural/stair contributions, in that order when
    // nonempty. Every typed field derives from its actual original owner;
    // wall/opening contributions are staged authority, not independently
    // admitted physical hosts. Physical admission uses the complete composed
    // result. The wall contribution also retains exterior measurement updates
    // derived from original source plus that complete result. These read-only
    // maps are never substitute sources or publication authority.
    [[nodiscard]] const std::vector<std::map<std::string, Entity, std::less<>>>&
    original_source_leaf_candidates() const noexcept;

private:
    SelectionPropertyEditPreparation(const DocumentSnapshot&, nlohmann::json,
        std::map<std::string, Entity, std::less<>>,
        std::vector<std::map<std::string, Entity, std::less<>>>,
        std::set<std::string, std::less<>>);
    nlohmann::json request_;
    DocumentSnapshot preview_;
    std::vector<std::map<std::string, Entity, std::less<>>> leaves_;
    std::set<std::string, std::less<>> affected_walls_;
    std::string source_digest_;

    friend SelectionPropertyEditPreparation replay_selection_property_edit_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
};

// Closed canonical v1 fields: version, expected_revision, source_snapshot_digest,
// source_authoring_digest, source_entities_digest, source_saved_revision,
// selected, walls, slabs, hosted_openings, roofs, unchanged_policy. Selected rows
// are exactly {entity_id,family}; family uses wall/slab/hosted_opening/roof.
// V2 adds structural_profiles and column/beam families, and requires a nonempty
// structural lane exactly covering those declared owners. V1 retains its exact
// closed fields and historical family set. V2 is emitted only for that lane.
// V3 adds stair_profiles and stair/railing families, requires a nonempty stair
// lane, and always includes structural_profiles (which may be empty). It is
// emitted only for a nonempty stair lane. Each lane exactly covers its owners.
// V4 explicitly admits wall elevation receipt v4 and requires at least one such
// wall. It includes both structural_profiles and stair_profiles, either empty.
// V1..V3 reject elevation leaves. Other nested codecs retain their versions.
// V5 retains V4's exact fields and requires at least one slab receipt v3 with
// explicit hosted-elevation carry. Wall elevation is optional and still uses
// receipt v4. V1..V4 reject slab carry; V5 permits isolated slab carry without
// granting arbitrary catalog, placement or candidate-map authoring.
// Strict canonical round trips,
// 1..1000 complete targets, 4 MiB wire, 100000 values, depth 64. Structural and
// stair all-null intents explicitly retain their declared members. Historical typed
// lanes retain their own empty-intent rules. Duplicate/unsorted/missing targets
// and undeclared fields refuse.
[[nodiscard]] nlohmann::json validate_selection_property_edit_request(const nlohmann::json&);
[[nodiscard]] SelectionPropertyEditPreparation prepare_selection_property_edit(
    const DocumentSnapshot&, const SelectionPropertyEditRequest&);
[[nodiscard]] SelectionPropertyEditPreparation replay_selection_property_edit_preparation(
    const DocumentSnapshot&, const nlohmann::json&);

// Rechecks complete retained source/history/assets/navigation/names/save and
// editability state, not merely document ID/head. Does not certify preview or
// room review, and does not give a stale token any publication authority.
void require_selection_property_edit_source_current(
    const DocumentSnapshot&, const SelectionPropertyEditPreparation&);

} // namespace sketch

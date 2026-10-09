#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <utility>

namespace sketch {

// Pure actual-source removal for known stairs/railings, columns/beams and slabs.
// Derives attached rails, hosted catalog rows and known presentation dependents;
// refuses protected phase ownership and affected opaque/future references.
// Optional components are actual catalog/instance keys, never render aliases.
// Component-only selection is supported; ordinary wall/roof hosts are admitted
// through their existing codecs without acquiring physical deletion authority.
// The source is immutable. Document command admission retains source history.
// Complete catalog consequences admit only actual qualified placed rows on a
// sole active proposed wall, or its active semantic opening, in a retained
// baseline carrier of that same saved registry. Raw carrier bytes and phase
// membership remain exact except for the admitted placed-row retirement.
// The separate complete proposed-opening opt-in also admits a sole active
// proposed opening on its same-registry active existing wall. The opening and
// wall remain exact; this grants no physical or baseline-opening authority.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_architectural_object_removal(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<std::string>& selected_object_ids,
    const std::vector<std::pair<std::string, std::string>>& explicit_components = {},
    bool allow_manufactured_opening_hosts = false,
    std::size_t reserved_native_work = 0,
    bool complete_hosted_catalog_consequences = false,
    bool complete_proposed_opening_catalog_consequences = false);

// Source-only analytical admission for the opt-in opening-host lane. Includes
// both complete supported source/candidate inventories and component work in
// the same 262144 allowance as the caller's reserved wall/join work. No factory.
void preflight_architectural_object_removal(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<std::string>& selected_object_ids,
    const std::vector<std::pair<std::string, std::string>>& explicit_components,
    std::size_t reserved_native_work = 0,
    bool complete_hosted_catalog_consequences = false,
    bool complete_proposed_opening_catalog_consequences = false);

// Analytical protected-row eligibility for an already actual-source candidate.
// Requires the real saved choice, an absent qualified row, exact raw carrier
// filtering and unchanged registered opening/wall bodies and phase membership.
// Validation is shared across the bounded selection, not repeated per row.
// Returned keys grant no authority to other hosts or historical dialects.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> complete_proposed_opening_catalog_consequence_keys(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::map<std::string, Entity, std::less<>>& candidate,
    const std::vector<std::pair<std::string, std::string>>& requested_components,
    const std::string& registry_id, const std::string& alternative_id);

} // namespace sketch

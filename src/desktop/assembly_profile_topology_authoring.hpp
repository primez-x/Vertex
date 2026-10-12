#pragma once

#include "sketch/assembly_model.hpp"

#include <functional>

namespace sketch::desktop {

using AssemblyProfileTopologyIdentityGenerator = std::function<std::string()>;

[[nodiscard]] bool assembly_profiles_have_topology(const std::vector<AssemblyProfile>& profiles);

// Assign every ring/segment/vertex afresh, reserving the complete shared ID
// namespace of actual same-ID own AND hidden type profiles. The default retains
// Properties' UUID generation. A proposal may supply one captured generator
// shared across all of its profiles; it must return valid fresh local IDs.
void assign_fresh_profile_topology(AssemblyProfile& profile,
    const std::vector<AssemblyProfile>& own_profiles,
    const std::vector<AssemblyProfile>& type_profiles,
    const AssemblyProfileTopologyIdentityGenerator& identity = {});

// Compare the final map/XY/rings only, exactly as Properties does. Same-ID
// unchanged geometry retains exact actual topology (including anonymous rows);
// changed/new geometry receives fresh IDs. Extrusion/material changes alone do
// not replace topology. Roster and geometry authority remain the caller's job.
void collect_reviewed_profile_topology(std::vector<AssemblyProfile>& profiles,
    const std::vector<AssemblyProfile>& own_profiles,
    const std::vector<AssemblyProfile>& type_profiles,
    const AssemblyProfileTopologyIdentityGenerator& identity = {});

} // namespace sketch::desktop

#pragma once

#include "sketch/document.hpp"
#include "sketch/georeferencing_contract.hpp"

#include <string>

namespace sketch {

// Pro georeferencing is persisted as a typed Document entity so its declared
// CRS, supplied transform, observations, and offline resource manifest travel
// with the project and are validated at every document boundary.
inline constexpr const char* kGeoreferencingEntityType = "georeferencing";

[[nodiscard]] Entity make_georeferencing_entity(std::string id,
                                                 const GeoreferencingContract& contract);
[[nodiscard]] GeoreferencingContract decode_georeferencing_entity(const Entity& entity);
void validate_georeferencing_entity(const Entity& entity);

}  // namespace sketch

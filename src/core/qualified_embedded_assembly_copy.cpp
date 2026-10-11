#include "sketch/qualified_embedded_assembly_copy.hpp"

#include "sketch/document_digest.hpp"
#include "sketch/phase_assembly_catalog_edit_preparation.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_assembly_instance_property_edit.hpp"
#include "sketch/retained_authoring_identities.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sketch {
namespace {
using Json = nlohmann::json;
using QualifiedId = std::pair<std::string, std::string>;
using DimensionIds = std::map<std::string, std::string, std::less<>>;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
constexpr std::size_t target_limit = 1000;
constexpr std::size_t dimension_limit = 1000;
constexpr std::size_t fresh_identity_limit = target_limit + dimension_limit;
constexpr std::size_t membership_limit = 1000;
constexpr std::size_t v2_fresh_identity_limit = fresh_identity_limit + membership_limit;
constexpr std::size_t presentation_identity_limit = 4096;
constexpr Revision safe_revision_limit = 9'007'199'254'740'991ULL;

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Qualified embedded assembly copy: ") + reason);
}
// Complete raw admission precedes codecs, serialization, digests and copies.
struct Budget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t amount) {
        if (amount > byte_limit - bytes) reject("request byte budget exceeded");
        bytes += amount;
    }
    void text(std::string_view value) {
        add(2);
        for (std::size_t index = 0; index < value.size(); ++index) {
            const auto c = static_cast<unsigned char>(value[index]);
            if (c >= 0x80) {
                const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                if (!tail || tail >= value.size() - index) reject("invalid UTF-8 string");
                const auto first = static_cast<unsigned char>(value[index + 1]);
                if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
                    (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90))
                    reject("invalid UTF-8 string");
                for (std::size_t offset = 1; offset <= tail; ++offset) {
                    const auto next = static_cast<unsigned char>(value[index + offset]);
                    if (next < 0x80 || next > 0xbf) reject("invalid UTF-8 string");
                }
                add(tail + 1); index += tail;
            } else if (c == '"' || c == '\\' || c == '\b' || c == '\f' ||
                       c == '\n' || c == '\r' || c == '\t') add(2);
            else add(c < 0x20 ? 6 : 1);
        }
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 100000) reject("request node/nesting budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable request JSON");
        if (value.is_object()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) { text(key); add(1); read(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) read(child, depth + 1);
        } else if (value.is_string()) text(value.get_ref<const std::string&>());
        else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
void identity(std::string_view value) {
    if (value.empty() || value.size() > 128 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid bounded document identity");
}
void local_identity(std::string_view value) {
    if (value.empty() || value.size() > 4096 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }) ||
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c == ' '; }))
        reject("invalid bounded local source identity");
}
void overlay_source_identity(std::string_view value) {
    if (value.empty() || value.size() > 128) reject("original overlay identity exceeds bound");
}
const std::string& text(const Json& value) {
    if (!value.is_string()) reject("expected a string");
    return value.get_ref<const std::string&>();
}
void action_message(std::string_view value) {
    if (value.empty() || value.size() > 1024 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }) ||
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c == ' '; }))
        reject("message must be a bounded nonempty action");
}
Revision revision(const Json& value) {
    Revision result{};
    if (value.is_number_unsigned()) result = value.get<Revision>();
    else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        result = static_cast<Revision>(value.get<std::int64_t>());
    else reject("invalid source revision");
    if (result > safe_revision_limit) reject("source revision is not a safe integer");
    return result;
}
void digest(const Json& value) {
    const auto& hash = text(value);
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("invalid source digest");
}
// The typed geometry producer consumes doubles. Refuse lossy integer inputs,
// but retain the admitted JSON category, including authored float negative zero.
Json encoded_number(double number, const Json& admitted) {
    if (admitted.is_number_unsigned()) {
        if (number < 0.0 || number >= 18446744073709551616.0) reject("lossy transform number");
        return Json(static_cast<std::uint64_t>(number));
    }
    if (admitted.is_number_integer()) {
        if (number < -9223372036854775808.0 || number >= 9223372036854775808.0) reject("lossy transform number");
        return Json(static_cast<std::int64_t>(number));
    }
    return Json(number);
}
double number(const Json& value) {
    if (!value.is_number()) reject("transform value must be numeric");
    const double result = value.get<double>();
    if (!std::isfinite(result) || encoded_number(result, value).dump() != value.dump())
        reject("transform value must be finite and lossless");
    return result;
}
Vec3 point(const Json& value) {
    if (!value.is_array() || value.size() != 3) reject("transform point must contain exactly XYZ");
    return {number(value.at(0)), number(value.at(1)), number(value.at(2))};
}
bool flag(const Json& value) {
    if (!value.is_boolean()) reject("transform flags must be booleans");
    return value.get<bool>();
}
void admit_transform(const ArchitecturalGroupTransform& value) {
    if (!std::isfinite(value.pivot.x) || !std::isfinite(value.pivot.y) || !std::isfinite(value.pivot.z) ||
        !std::isfinite(value.offset.x) || !std::isfinite(value.offset.y) || !std::isfinite(value.offset.z) ||
        !std::isfinite(value.rotation_z_radians) || !std::isfinite(value.scale) || value.scale <= 0.0)
        reject("transform must be finite with positive scale");
}
ArchitecturalGroupTransform transform(const Json& value) {
    fields(value, {"pivot", "offset", "rotation_z_radians", "scale", "flip_horizontal", "flip_vertical"});
    ArchitecturalGroupTransform result;
    result.pivot = point(value.at("pivot")); result.offset = point(value.at("offset"));
    result.rotation_z_radians = number(value.at("rotation_z_radians")); result.scale = number(value.at("scale"));
    result.flip_horizontal = flag(value.at("flip_horizontal")); result.flip_vertical = flag(value.at("flip_vertical"));
    admit_transform(result); return result;
}
Json encode_transform(const ArchitecturalGroupTransform& value) {
    return {{"pivot", Json::array({value.pivot.x, value.pivot.y, value.pivot.z})},
        {"offset", Json::array({value.offset.x, value.offset.y, value.offset.z})},
        {"rotation_z_radians", value.rotation_z_radians}, {"scale", value.scale},
        {"flip_horizontal", value.flip_horizontal}, {"flip_vertical", value.flip_vertical}};
}
void admit_request(const Json& request) {
    Budget{}.read(request);
    if (!request.is_object() || !request.contains("version") || !request.at("version").is_number_integer() ||
        (request.at("version") != 1 && request.at("version") != 2 && request.at("version") != 3 &&
            request.at("version") != 4)) reject("unsupported request version");
    const bool v2 = request.at("version") != 1;
    const bool v3 = request.at("version") >= 3;
    if (v3) fields(request, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "targets", "copied_dimension_ids", "membership_entity_ids",
        "presentation_overlay_identities", "message"});
    else if (v2) fields(request, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "targets", "copied_dimension_ids", "membership_entity_ids", "message"});
    else fields(request, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "targets", "copied_dimension_ids", "message"});
    const auto expected = revision(request.at("expected_revision"));
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(request.at(name));
    if (!request.at("source_saved_revision").is_null() && revision(request.at("source_saved_revision")) > expected)
        reject("saved revision exceeds captured revision");
    action_message(text(request.at("message")));
    const auto& targets = request.at("targets");
    const auto& dimensions = request.at("copied_dimension_ids");
    if (!targets.is_array() || targets.empty() || targets.size() > target_limit) reject("copy requires 1 to 1000 roots");
    if (!dimensions.is_object() || dimensions.size() > dimension_limit) reject("copy dimension roster exceeds 1000");
    if (v2 && (!request.at("membership_entity_ids").is_object() || request.at("membership_entity_ids").size() > membership_limit))
        reject("copy companion roster exceeds 1000");
    if (v3 && (!request.at("presentation_overlay_identities").is_object() ||
        request.at("presentation_overlay_identities").size() > presentation_identity_limit))
        reject("copy overlay roster exceeds bound");
    std::set<QualifiedId> sources, destinations;
    std::set<std::string, std::less<>> raw_fresh, fresh_dimensions;
    std::optional<QualifiedId> preceding;
    for (const auto& row : targets) {
        fields(row, {"catalog_id", "instance_id", "copy_instance_id", "transform"});
        const auto& catalog = text(row.at("catalog_id")); identity(catalog);
        const auto& original = text(row.at("instance_id")); local_identity(original);
        const auto& copy = text(row.at("copy_instance_id")); identity(copy);
        QualifiedId source{catalog, original};
        if (preceding && !(*preceding < source)) reject("targets must use unique canonical catalog/local source order");
        preceding = source; sources.insert(std::move(source));
        if (!destinations.emplace(catalog, copy).second) reject("duplicate qualified destination");
        raw_fresh.insert(copy); (void)transform(row.at("transform"));
    }
    if (v3) for (const auto& row : targets) {
        const auto alias = text(row.at("catalog_id")) + ":instance:" + text(row.at("copy_instance_id"));
        if (!raw_fresh.insert(alias).second) reject("fresh root and presentation identities overlap");
    }
    for (const auto& destination : destinations)
        if (sources.contains(destination)) reject("destination overlaps a selected source");
    for (const auto& [original, copy] : dimensions.items()) {
        identity(original); const auto& destination = text(copy); identity(destination);
        if (original == destination || raw_fresh.contains(destination) || !fresh_dimensions.insert(destination).second)
            reject("copy dimension identities overlap or repeat");
    }
    raw_fresh.insert(fresh_dimensions.begin(), fresh_dimensions.end());
    if (v2) {
        const auto& memberships = request.at("membership_entity_ids");
        for (const auto& [registry, companion] : memberships.items()) {
            identity(registry); const auto& destination = text(companion); identity(destination);
            if (raw_fresh.contains(registry) || memberships.contains(destination) || !raw_fresh.insert(destination).second)
                reject("copy companion identities overlap allocations or registry identities");
        }
    }
    if (v3) for (const auto& [original, copy] : request.at("presentation_overlay_identities").items()) {
        overlay_source_identity(original); const auto& destination = text(copy); identity(destination);
        if (raw_fresh.contains(original) || request.at("presentation_overlay_identities").contains(destination) ||
            !raw_fresh.insert(destination).second) reject("copy overlay identities overlap actual allocations or source overlays");
    }
    if (raw_fresh.size() > (v3 ? presentation_identity_limit : v2 ? v2_fresh_identity_limit : fresh_identity_limit))
        reject("aggregate fresh identity roster exceeds bound");
}
void admit_typed(std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& fallback, const DimensionIds& dimensions, std::string_view message,
    const DimensionIds* memberships = nullptr, const DimensionIds* overlays = nullptr) {
    if (targets.empty() || targets.size() > target_limit || dimensions.size() > dimension_limit)
        reject("typed copy target/dimension roster exceeds bound");
    if (memberships && memberships->size() > membership_limit) reject("typed copy companion roster exceeds 1000");
    if (overlays && overlays->size() > presentation_identity_limit) reject("typed copy overlay roster exceeds bound");
    action_message(message);
    Budget budget; budget.add(2048); budget.text(message);
    // Reserve numeric slots, field names and punctuation before creating JSON.
    for (const auto& target : targets) {
        identity(target.catalog_id); local_identity(target.instance_id);
        if (!target.copy_instance_id) reject("every copy root requires a fresh identity");
        identity(*target.copy_instance_id);
        budget.add(512); budget.text(target.catalog_id); budget.text(target.instance_id); budget.text(*target.copy_instance_id);
        admit_transform(target.transform ? *target.transform : fallback);
    }
    for (const auto& [original, copy] : dimensions) {
        identity(original); identity(copy); budget.add(4); budget.text(original); budget.text(copy);
    }
    if (!memberships) return; // Preserve the historical v1 typed admission.
    // Complete v2 allocation/byte admission before JSON construction or digests.
    std::set<QualifiedId> sources, destinations;
    std::set<std::string, std::less<>> fresh;
    for (const auto& target : targets) {
        if (!sources.emplace(target.catalog_id, target.instance_id).second)
            reject("duplicate qualified copy source");
        if (!destinations.emplace(target.catalog_id, *target.copy_instance_id).second)
            reject("duplicate qualified destination");
        fresh.insert(*target.copy_instance_id);
    }
    for (const auto& destination : destinations)
        if (sources.contains(destination)) reject("destination overlaps a selected source");
    if (overlays) for (const auto& target : targets)
        if (!fresh.insert(target.catalog_id + ":instance:" + *target.copy_instance_id).second)
            reject("fresh root and presentation identities overlap");
    for (const auto& [original, copy] : dimensions)
        if (original == copy || !fresh.insert(copy).second) reject("copy dimension identities overlap or repeat");
    budget.add(32);
    for (const auto& [registry, companion] : *memberships) {
        identity(registry); identity(companion); budget.add(4); budget.text(registry); budget.text(companion);
        if (fresh.contains(registry) || memberships->contains(companion) || !fresh.insert(companion).second)
            reject("copy companion identities overlap allocations or registry identities");
    }
    if (overlays) {
        budget.add(40);
        for (const auto& [original, copy] : *overlays) {
            overlay_source_identity(original); identity(copy); budget.add(4); budget.text(original); budget.text(copy);
            if (fresh.contains(original) || overlays->contains(copy) || !fresh.insert(copy).second)
                reject("copy overlay identities overlap actual allocations or source overlays");
        }
    }
    if (fresh.size() > (overlays ? presentation_identity_limit : v2_fresh_identity_limit))
        reject("aggregate fresh identity roster exceeds bound");
}
std::vector<EmbeddedAssemblyGroupTarget> decode_targets(const Json& request) {
    std::vector<EmbeddedAssemblyGroupTarget> result; result.reserve(request.at("targets").size());
    for (const auto& row : request.at("targets")) result.push_back({text(row.at("catalog_id")),
        text(row.at("instance_id")), text(row.at("copy_instance_id")), transform(row.at("transform"))});
    return result;
}
DimensionIds decode_dimensions(const Json& request) {
    DimensionIds result;
    for (const auto& [original, copy] : request.at("copied_dimension_ids").items()) result.emplace(original, text(copy));
    return result;
}
DimensionIds decode_memberships(const Json& request) {
    DimensionIds result;
    for (const auto& [registry, companion] : request.at("membership_entity_ids").items())
        result.emplace(registry, text(companion));
    return result;
}
DimensionIds decode_overlays(const Json& request) {
    DimensionIds result;
    for (const auto& [original, copy] : request.at("presentation_overlay_identities").items())
        result.emplace(original, text(copy));
    return result;
}
using PresentationIds = std::set<std::string, std::less<>>;
// Current source admission also serves the source-only roster inspector. All
// opaque JSON is bounded before codecs, dependency scratch copies or aliases.
struct PresentationSourceBudget {
    std::size_t nodes{}, bytes{};
    void charge(std::size_t amount) {
        if (amount > 64 * 1024 * 1024 - bytes) reject("presentation source byte budget exceeded");
        bytes += amount;
    }
    void string(std::string_view value) {
        if (value.size() > (64 * 1024 * 1024 - bytes) / 6) reject("presentation source byte budget exceeded");
        charge(value.size() * 6);
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("presentation source node/nesting budget exceeded");
        charge(32);
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable presentation source JSON");
        if (value.is_string()) string(value.get_ref<const std::string&>());
        else if (value.is_object()) for (const auto& [key, child] : value.items()) { string(key); read(child, depth + 1); }
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
};
void admit_presentation_source(const AssemblyDocumentEntities& source) {
    if (source.size() > 65536) reject("presentation source entity budget exceeded");
    PresentationSourceBudget budget;
    for (const auto& [id, entity] : source) {
        if (id.empty() || id != entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("presentation source requires identified entity envelopes");
        budget.string(id); budget.string(entity.type); budget.read(entity.properties); budget.read(entity.extensions);
    }
}
bool touches(const Json& value, const PresentationIds& ids) {
    if (value.is_string()) return ids.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) if (ids.contains(key) || touches(child, ids)) return true;
    } else if (value.is_array()) for (const auto& child : value) if (touches(child, ids)) return true;
    return false;
}
bool affected_overlay(const Json& row, const PresentationIds& owners) {
    return (row.contains("object_id") && owners.contains(text(row.at("object_id")))) ||
        (row.contains("dimension_binding") && !row.at("dimension_binding").is_null() &&
            owners.contains(text(row.at("dimension_binding").at("object_id"))));
}
struct PresentationPlan {
    EmbeddedAssemblyPresentationIds source_aliases;
    std::map<std::string, AssemblyInstancePropertyEditTarget, std::less<>> dimensions;
    PresentationIds owners, overlays;
};
// Only understood references are removed from this discarded scratch envelope.
// Extension/envelope siblings remain visible to the opaque-reference fence.
Entity opaque_remainder(Entity entity, const PresentationPlan& plan) {
    auto& p = entity.properties;
    if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == "assembly_instance") {
        (void)decode_document_assembly_instance(entity); p.erase("instance"); p.erase("assembly_catalog_id");
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == assembly_phase_membership_entity_type) {
        (void)AssemblyPhaseMembership::from_json(p.at("model")); p.erase("model");
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("object_ids");
            auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                row.erase("id"); row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                    row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) {
            if (plan.owners.contains(text(row.at("target_id"))) && row.at("target_kind") != "object")
                reject("affected annotation reference is not a supported component object override");
            row.erase("target_id");
        }
    } else if (plan.dimensions.contains(entity.id)) return assembly_dimension_clone_dependency_remainder(entity);
    return entity;
}
PresentationPlan inspect_presentation(const AssemblyDocumentEntities& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets) {
    if (targets.empty() || targets.size() > target_limit) reject("presentation copy requires 1 to 1000 roots");
    // Typed source names, raw entities and catalog work precede any decode.
    std::vector<QualifiedId> roots; roots.reserve(targets.size());
    std::set<QualifiedId> unique;
    for (const auto& target : targets) {
        identity(target.catalog_id); local_identity(target.instance_id);
        if (!unique.emplace(target.catalog_id, target.instance_id).second) reject("duplicate presentation source root");
        roots.emplace_back(target.catalog_id, target.instance_id);
    }
    admit_presentation_source(source);
    AssemblyCatalogTransferBudget catalog_budget;
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalog_budget);
    }
    PresentationPlan plan;
    plan.source_aliases = embedded_assembly_presentation_ids(source);
    for (const auto& root : roots) {
        const auto found = plan.source_aliases.find(root);
        if (found == plan.source_aliases.end()) reject("presentation source root is not an actual embedded instance");
        if (!plan.owners.insert(found->second).second) reject("actual presentation source owners overlap");
    }
    plan.dimensions = inspect_embedded_assembly_dimension_clone_roster(source, roots);
    for (const auto& [id, component] : plan.dimensions) {
        (void)component;
        if (!plan.owners.insert(id).second) reject("dimension and component presentation owners overlap");
    }
    std::map<std::string, std::size_t, std::less<>> occurrences;
    for (const auto& [id, entity] : source) if (entity.type == kSheetViewEntityType) {
        (void)id; (void)decode_sheet_view_entity(entity);
        for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
            for (const auto& row : view.at("overlays")) {
                const auto& overlay = text(row.at("id")); overlay_source_identity(overlay);
                ++occurrences[overlay];
                if (affected_overlay(row, plan.owners)) {
                    plan.overlays.insert(overlay);
                    if (plan.overlays.size() > presentation_identity_limit) reject("affected overlay count exceeds bound");
                }
            }
    }
    for (const auto& id : plan.overlays) if (occurrences.at(id) != 1 || source.contains(id) || plan.owners.contains(id))
        reject("affected overlay identity aliases another retained owner");
    auto affected = plan.owners; affected.insert(plan.overlays.begin(), plan.overlays.end());
    for (const auto& [id, entity] : source) {
        (void)id;
        const auto remainder = opaque_remainder(entity, plan);
        if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
            reject("opaque source presentation reference cannot be copied safely");
    }
    return plan;
}
void reserve_presentation_allocations(const DocumentSnapshot& source, const Json& request,
    const PresentationPlan& plan) {
    const auto overlays = decode_overlays(request);
    if (overlays.size() != plan.overlays.size()) reject("overlay allocation differs from actual affected roster");
    for (const auto& id : plan.overlays) if (!overlays.contains(id)) reject("missing actual affected overlay allocation");
    const auto dimensions = decode_dimensions(request);
    if (dimensions.size() != plan.dimensions.size()) reject("dimension allocation differs from actual associated roster");
    for (const auto& [id, component] : plan.dimensions) {
        (void)component;
        if (!dimensions.contains(id)) reject("missing actual associated dimension allocation");
    }
    PresentationIds fresh;
    const auto add = [&](const std::string& id) {
        if (!fresh.insert(id).second) reject("actual fresh presentation identities overlap");
    };
    for (const auto& row : request.at("targets")) {
        add(text(row.at("copy_instance_id")));
        add(text(row.at("catalog_id")) + ":instance:" + text(row.at("copy_instance_id")));
    }
    for (const auto* name : {"copied_dimension_ids", "membership_entity_ids", "presentation_overlay_identities"})
        for (const auto& [original, copy] : request.at(name).items()) { (void)original; add(text(copy)); }
    if (fresh.size() > presentation_identity_limit) reject("complete fresh presentation inventory exceeds bound");
    const std::vector<std::string> ids(fresh.begin(), fresh.end());
    const auto reserve = [&](const AssemblyDocumentEntities& entities) {
        validate_phase_assembly_instance_property_edit_fresh_identities(entities, ids);
        for (const auto& [id, entity] : entities) if (entity.type == "assembly_model")
            for (const auto& row : entity.properties.at("model").at("instances"))
                if (fresh.contains(id + ":instance:" + text(row.at("id"))))
                    reject("fresh presentation identity reuses a retained raw component alias");
    };
    const auto reserve_assets = [&](const auto& assets) {
        for (const auto& [id, asset] : assets) {
            (void)asset;
            if (fresh.contains(id)) reject("fresh presentation identity reuses a retained asset");
        }
    };
    reserve(source.entities()); reserve_assets(source.assets());
    for (const auto& record : source.history()) { reserve(record.entities); reserve_assets(record.assets); }
    validate_retained_authoring_fresh_identities(source.history(), source.history().size(), fresh);
}
// Embedded profile/dimension expansion is already world-authored; its carrier
// or placement host supplies organization, never a second Site pose. The
// geometry operator rotates, then reflects world X/Y. Express the same
// linear map in AssemblyTransform's reflection-before-yaw convention. Work in
// pivot-relative view coordinates to avoid subtracting large affine origins.
void transform_overlay(Json& row, const CoordinatedView& view, const ArchitecturalGroupTransform& operation) {
    const double turn = 2.0 * std::numbers::pi;
    auto angle = std::remainder(std::remainder(operation.rotation_z_radians, turn) +
        (operation.flip_horizontal && operation.flip_vertical ? std::numbers::pi : 0.0), turn);
    const bool reflected = operation.flip_horizontal != operation.flip_vertical;
    if (reflected) angle = operation.flip_horizontal ? std::numbers::pi - angle : -angle;
    AssemblyTransform transform;
    transform.translation_m = {operation.offset.x, operation.offset.y, operation.offset.z};
    transform.rotation_radians = std::remainder(angle, turn);
    transform.scale = operation.scale; transform.mirrored_y = reflected;
    if (transform.translation_m == AssemblyPoint3{} && transform.rotation_radians == 0 &&
        transform.scale == 1 && !transform.mirrored_y) return;
    auto relative_view = view;
    const auto reference = coordinated_view_origins(view).reference_m;
    relative_view.origin_m = {reference[0] - operation.pivot.x, reference[1] - operation.pivot.y,
        reference[2] - operation.pivot.z};
    // The actual reference origin above already incorporates the legacy section
    // compatibility rule; this scratch frame must not apply that rule twice.
    relative_view.id = "qualified-copy-frame";
    for (const auto* field : {"start_m", "end_m"}) {
        const auto& raw = row.at(field);
        const std::array<double, 2> original{raw.at(0).get<double>(), raw.at(1).get<double>()};
        const auto moved = transform_coordinated_overlay_point(relative_view, original, transform);
        if (moved != original) row.at(field) = Json::array({moved[0], moved[1]});
    }
}
void carry_presentation(const AssemblyDocumentEntities& source, AssemblyDocumentEntities& candidate,
    std::span<const EmbeddedAssemblyGroupTarget> targets, const DimensionIds& dimensions,
    const DimensionIds& overlays, const PresentationPlan& plan) {
    admit_presentation_source(candidate);
    const auto aliases = embedded_assembly_presentation_ids(candidate);
    DimensionIds mappings;
    std::map<std::string, const ArchitecturalGroupTransform*, std::less<>> operators;
    std::map<QualifiedId, const ArchitecturalGroupTransform*> root_operators;
    for (const auto& target : targets) {
        const QualifiedId original{target.catalog_id, target.instance_id};
        const QualifiedId destination{target.catalog_id, *target.copy_instance_id};
        const auto& before = plan.source_aliases.at(original); const auto& after = aliases.at(destination);
        if (after != target.catalog_id + ":instance:" + *target.copy_instance_id || before == after || !target.transform ||
            !mappings.emplace(before, after).second) reject("missing actual fresh component presentation destination");
        operators.emplace(before, &*target.transform); root_operators.emplace(original, &*target.transform);
    }
    for (const auto& [id, component] : plan.dimensions) {
        const auto& destination = dimensions.at(id);
        if (!candidate.contains(destination) || !mappings.emplace(id, destination).second)
            reject("missing actual fresh dimension presentation destination");
        operators.emplace(id, root_operators.at({component.catalog_id, component.instance_id}));
    }
    for (const auto& [root, alias] : plan.source_aliases)
        if (aliases.at(root) != alias) reject("presentation copy changed a retained component alias");
    const auto remap = [&](Json& row, const char* key) {
        if (!row.contains(key)) return;
        const auto found = mappings.find(text(row.at(key)));
        if (found != mappings.end()) row.at(key) = found->second;
    };
    for (const auto& [id, entity] : source) {
        auto& changed = candidate.at(id);
        if (entity.type == kSheetViewEntityType) {
            const auto decoded = decode_sheet_view_entity(entity);
            const auto& views = entity.properties.at("model").at("views");
            auto& changed_views = changed.properties.at("model").at("views");
            for (std::size_t index = 0; index < views.size(); ++index) {
                const auto& original = views.at(index); auto& view = changed_views.at(index);
                const auto frame = std::find_if(decoded.views().begin(), decoded.views().end(),
                    [&](const auto& v) { return v.id == text(original.at("id")); });
                if (frame == decoded.views().end()) reject("actual saved presentation frame is absent");
                if (original.contains("object_ids")) for (const auto& object : original.at("object_ids")) {
                    const auto found = mappings.find(text(object));
                    if (found != mappings.end()) view.at("object_ids").push_back(found->second);
                }
                const auto& presentation = original.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    for (const auto& original_row : presentation.at("appearance").at("objects"))
                        if (plan.owners.contains(text(original_row.at("object_id")))) {
                            auto row = original_row; remap(row, "object_id");
                            view.at("presentation").at("appearance").at("objects").push_back(std::move(row));
                        }
                if (original.contains("overlays")) for (const auto& original_row : original.at("overlays"))
                    if (affected_overlay(original_row, plan.owners)) {
                        auto row = original_row; row.at("id") = overlays.at(text(original_row.at("id")));
                        // Bound dimensions regenerate from the actual new owner;
                        // detached callouts follow their saved world/view frame.
                        if ((!row.contains("dimension_binding") || row.at("dimension_binding").is_null()) && row.contains("object_id")) {
                            const auto operation = operators.find(text(row.at("object_id")));
                            if (operation != operators.end()) transform_overlay(row, *frame, *operation->second);
                        }
                        remap(row, "object_id");
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                            remap(row.at("dimension_binding"), "object_id");
                        view.at("overlays").push_back(std::move(row));
                    }
            }
            validate_sheet_view_entity(changed);
        } else if (entity.type == kAnnotationEntityType) {
            for (const auto& original : entity.properties.at("state").at("overrides"))
                if (plan.owners.contains(text(original.at("target_id")))) {
                    auto row = original; remap(row, "target_id");
                    changed.properties.at("state").at("overrides").push_back(std::move(row));
                }
            validate_annotation_entity(changed);
        }
    }
    if (embedded_assembly_presentation_ids(candidate) != aliases)
        reject("copied presentation changed actual component aliases");
}
void authenticate(const DocumentSnapshot& source, const Json& request) {
    const auto saved = request.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(request.at("source_saved_revision"))};
    if (source.revision() != revision(request.at("expected_revision")) || source.saved_revision_optional() != saved ||
        document_snapshot_digest(source) != text(request.at("source_snapshot_digest")) ||
        document_authoring_source_digest_v2(source) != text(request.at("source_authoring_digest")) ||
        entity_map_digest(source.entities()) != text(request.at("source_entities_digest"))) reject("complete captured source changed");
}
} // namespace

QualifiedEmbeddedAssemblyCopyPreparation::QualifiedEmbeddedAssemblyCopyPreparation(Json request, AssemblyDocumentEntities entities)
    : request_(std::move(request)), entities_(std::move(entities)),
      digest_(text(request_.at("source_snapshot_digest"))), message_(text(request_.at("message"))),
      revision_(revision(request_.at("expected_revision"))) {}

Json validate_qualified_embedded_assembly_copy_request(const Json& request) {
    admit_request(request); return request;
}
std::vector<std::string> inspect_qualified_embedded_assembly_copy_presentation_overlay_ids(
    const AssemblyDocumentEntities& source, std::span<const EmbeddedAssemblyGroupTarget> targets) {
    const auto plan = inspect_presentation(source, targets);
    return {plan.overlays.begin(), plan.overlays.end()};
}
QualifiedEmbeddedAssemblyCopyPreparation prepare_qualified_embedded_assembly_copy(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets, const ArchitecturalGroupTransform& fallback,
    const DimensionIds& dimensions, std::string message) {
    admit_typed(targets, fallback, dimensions, message);
    admit_retained_assembly_authoring_source(source);
    if (source.revision() > safe_revision_limit) reject("source revision is not a safe integer");
    std::vector<const EmbeddedAssemblyGroupTarget*> ordered; ordered.reserve(targets.size());
    for (const auto& target : targets) ordered.push_back(&target);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return std::tie(a->catalog_id, a->instance_id) < std::tie(b->catalog_id, b->instance_id);
    });
    Json rows = Json::array();
    for (const auto* target : ordered) rows.push_back({{"catalog_id", target->catalog_id},
        {"instance_id", target->instance_id}, {"copy_instance_id", *target->copy_instance_id},
        {"transform", encode_transform(target->transform ? *target->transform : fallback)}});
    Json request{{"version", 1}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"targets", std::move(rows)}, {"copied_dimension_ids", dimensions}, {"message", std::move(message)}};
    return replay_qualified_embedded_assembly_copy_preparation(source, request);
}
QualifiedEmbeddedAssemblyCopyPreparation prepare_qualified_embedded_assembly_copy(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets, const ArchitecturalGroupTransform& fallback,
    const DimensionIds& dimensions, const DimensionIds& memberships, std::string message) {
    admit_typed(targets, fallback, dimensions, message, &memberships);
    if (source.revision() > safe_revision_limit) reject("source revision is not a safe integer");
    if (source.saved_revision_optional() && *source.saved_revision_optional() > source.revision())
        reject("saved revision exceeds captured revision");
    std::vector<const EmbeddedAssemblyGroupTarget*> ordered; ordered.reserve(targets.size());
    for (const auto& target : targets) ordered.push_back(&target);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return std::tie(a->catalog_id, a->instance_id) < std::tie(b->catalog_id, b->instance_id);
    });
    Json rows = Json::array();
    for (const auto* target : ordered) rows.push_back({{"catalog_id", target->catalog_id},
        {"instance_id", target->instance_id}, {"copy_instance_id", *target->copy_instance_id},
        {"transform", encode_transform(target->transform ? *target->transform : fallback)}});
    // Fixed-size digest placeholders permit complete raw request admission
    // before hashing or deriving any source copy. Replace only those fields.
    const std::string pending_digest(64, '0');
    Json request{{"version", 2}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", pending_digest}, {"source_authoring_digest", pending_digest},
        {"source_entities_digest", pending_digest},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"targets", std::move(rows)}, {"copied_dimension_ids", dimensions},
        {"membership_entity_ids", memberships}, {"message", std::move(message)}};
    admit_request(request); admit_retained_assembly_authoring_source(source);
    request.at("source_snapshot_digest") = document_snapshot_digest(source);
    request.at("source_authoring_digest") = document_authoring_source_digest_v2(source);
    request.at("source_entities_digest") = entity_map_digest(source.entities());
    return replay_qualified_embedded_assembly_copy_preparation(source, request);
}
static QualifiedEmbeddedAssemblyCopyPreparation prepare_presented_embedded_assembly_copy(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets, const ArchitecturalGroupTransform& fallback,
    const DimensionIds& dimensions, const DimensionIds& memberships, const DimensionIds& overlays,
    int version, std::string message) {
    admit_typed(targets, fallback, dimensions, message, &memberships, &overlays);
    if (source.revision() > safe_revision_limit) reject("source revision is not a safe integer");
    if (source.saved_revision_optional() && *source.saved_revision_optional() > source.revision())
        reject("saved revision exceeds captured revision");
    std::vector<const EmbeddedAssemblyGroupTarget*> ordered; ordered.reserve(targets.size());
    for (const auto& target : targets) ordered.push_back(&target);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return std::tie(a->catalog_id, a->instance_id) < std::tie(b->catalog_id, b->instance_id);
    });
    Json rows = Json::array();
    for (const auto* target : ordered) rows.push_back({{"catalog_id", target->catalog_id},
        {"instance_id", target->instance_id}, {"copy_instance_id", *target->copy_instance_id},
        {"transform", encode_transform(target->transform ? *target->transform : fallback)}});
    const std::string pending_digest(64, '0');
    Json request{{"version", version}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", pending_digest}, {"source_authoring_digest", pending_digest},
        {"source_entities_digest", pending_digest},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"targets", std::move(rows)}, {"copied_dimension_ids", dimensions}, {"membership_entity_ids", memberships},
        {"presentation_overlay_identities", overlays}, {"message", std::move(message)}};
    admit_request(request); admit_retained_assembly_authoring_source(source); admit_presentation_source(source.entities());
    request.at("source_snapshot_digest") = document_snapshot_digest(source);
    request.at("source_authoring_digest") = document_authoring_source_digest_v2(source);
    request.at("source_entities_digest") = entity_map_digest(source.entities());
    return replay_qualified_embedded_assembly_copy_preparation(source, request);
}
QualifiedEmbeddedAssemblyCopyPreparation prepare_qualified_embedded_assembly_copy(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets, const ArchitecturalGroupTransform& fallback,
    const DimensionIds& dimensions, const DimensionIds& memberships, const DimensionIds& overlays, std::string message) {
    return prepare_presented_embedded_assembly_copy(source,targets,fallback,dimensions,memberships,overlays,3,std::move(message));
}
QualifiedEmbeddedAssemblyCopyPreparation prepare_qualified_embedded_assembly_copy(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets, const ArchitecturalGroupTransform& fallback,
    const DimensionIds& dimensions, const DimensionIds& memberships, const DimensionIds& overlays,
    EmbeddedAssemblyCopyContextPolicy context_policy, std::string message) {
    if (context_policy!=EmbeddedAssemblyCopyContextPolicy::preserve_source) reject("unsupported copied drawing context policy");
    return prepare_presented_embedded_assembly_copy(source,targets,fallback,dimensions,memberships,overlays,4,std::move(message));
}
QualifiedEmbeddedAssemblyCopyPreparation replay_qualified_embedded_assembly_copy_preparation(
    const DocumentSnapshot& source, const Json& request) {
    admit_request(request); admit_retained_assembly_authoring_source(source);
    if (request.at("version") >= 3) admit_presentation_source(source.entities());
    authenticate(source, request);
    const auto targets = decode_targets(request);
    const auto dimensions = decode_dimensions(request);
    const auto presentation = request.at("version") >= 3 ?
        std::optional<PresentationPlan>{inspect_presentation(source.entities(), targets)} : std::nullopt;
    if (presentation) reserve_presentation_allocations(source, request, *presentation);
    // Every target has its actual operator. The unused fallback supplies no
    // allocator, candidate-map, Document fork or partial publication authority.
    auto entities = request.at("version") == 4 ?
        prepare_qualified_embedded_assembly_copy_entities(source, targets,
            ArchitecturalGroupTransform{}, revision(request.at("expected_revision")), dimensions, decode_memberships(request),
            EmbeddedAssemblyCopyContextPolicy::preserve_source) : request.at("version") == 1 ?
        prepare_qualified_embedded_assembly_copy_entities(source, targets,
            ArchitecturalGroupTransform{}, revision(request.at("expected_revision")), dimensions) :
        prepare_qualified_embedded_assembly_copy_entities(source, targets,
            ArchitecturalGroupTransform{}, revision(request.at("expected_revision")), dimensions, decode_memberships(request));
    if (presentation) carry_presentation(source.entities(), entities, targets, dimensions, decode_overlays(request), *presentation);
    return {request, std::move(entities)};
}
void require_qualified_embedded_assembly_copy_source_current(const DocumentSnapshot& source,
    const QualifiedEmbeddedAssemblyCopyPreparation& preparation) {
    admit_request(preparation.request()); admit_retained_assembly_authoring_source(source);
    if (preparation.request().at("version") >= 3) admit_presentation_source(source.entities());
    authenticate(source, preparation.request());
    if (preparation.expected_revision() != revision(preparation.request().at("expected_revision")) ||
        preparation.message() != text(preparation.request().at("message")) ||
        preparation.source_snapshot_digest() != text(preparation.request().at("source_snapshot_digest")))
        reject("sealed source metadata does not match request");
}
ApplyBoundaryConstraintChanges qualified_embedded_assembly_copy_command(
    const QualifiedEmbeddedAssemblyCopyPreparation& preparation) {
    admit_request(preparation.request());
    ApplyBoundaryConstraintChanges command;
    command.expected_revision = preparation.expected_revision();
    command.message = preparation.message();
    command.qualified_embedded_assembly_copy_intent = preparation.request();
    command.qualified_embedded_assembly_copy_completion = true;
    return command;
}

} // namespace sketch

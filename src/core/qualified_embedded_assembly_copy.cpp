#include "sketch/qualified_embedded_assembly_copy.hpp"

#include "sketch/document_digest.hpp"
#include "sketch/phase_assembly_catalog_edit_preparation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
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
    fields(request, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "targets", "copied_dimension_ids", "message"});
    if (!request.at("version").is_number_integer() || request.at("version") != 1) reject("unsupported request version");
    const auto expected = revision(request.at("expected_revision"));
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(request.at(name));
    if (!request.at("source_saved_revision").is_null() && revision(request.at("source_saved_revision")) > expected)
        reject("saved revision exceeds captured revision");
    action_message(text(request.at("message")));
    const auto& targets = request.at("targets");
    const auto& dimensions = request.at("copied_dimension_ids");
    if (!targets.is_array() || targets.empty() || targets.size() > target_limit) reject("copy requires 1 to 1000 roots");
    if (!dimensions.is_object() || dimensions.size() > dimension_limit) reject("copy dimension roster exceeds 1000");
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
    for (const auto& destination : destinations)
        if (sources.contains(destination)) reject("destination overlaps a selected source");
    for (const auto& [original, copy] : dimensions.items()) {
        identity(original); const auto& destination = text(copy); identity(destination);
        if (original == destination || raw_fresh.contains(destination) || !fresh_dimensions.insert(destination).second)
            reject("copy dimension identities overlap or repeat");
    }
    raw_fresh.insert(fresh_dimensions.begin(), fresh_dimensions.end());
    if (raw_fresh.size() > fresh_identity_limit) reject("aggregate fresh identity roster exceeds bound");
}
void admit_typed(std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& fallback, const DimensionIds& dimensions, std::string_view message) {
    if (targets.empty() || targets.size() > target_limit || dimensions.size() > dimension_limit)
        reject("typed copy target/dimension roster exceeds bound");
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
QualifiedEmbeddedAssemblyCopyPreparation replay_qualified_embedded_assembly_copy_preparation(
    const DocumentSnapshot& source, const Json& request) {
    admit_request(request); admit_retained_assembly_authoring_source(source); authenticate(source, request);
    const auto targets = decode_targets(request);
    const auto dimensions = decode_dimensions(request);
    // Every target has its actual operator. The unused fallback supplies no
    // allocator, candidate-map, Document fork or partial publication authority.
    auto entities = prepare_qualified_embedded_assembly_copy_entities(source, targets,
        ArchitecturalGroupTransform{}, revision(request.at("expected_revision")), dimensions);
    return {request, std::move(entities)};
}
void require_qualified_embedded_assembly_copy_source_current(const DocumentSnapshot& source,
    const QualifiedEmbeddedAssemblyCopyPreparation& preparation) {
    admit_request(preparation.request()); admit_retained_assembly_authoring_source(source);
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

#include "sketch/ordinary_selection_edit.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/presentation_transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
using QualifiedIds = std::set<std::pair<std::string, std::string>>;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
constexpr std::size_t node_limit = 100000;
constexpr std::size_t depth_limit = 64;
constexpr std::size_t target_limit = 1000;

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Ordinary selection edit: ") + reason);
}

// Bound and validate the complete input before copies, serialization or codecs.
// Numeric slots reserve 32 bytes, a conservative compact JSON upper bound.
struct Budget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("encoded byte budget exceeded");
        bytes += count;
    }
    void quoted(std::string_view value) {
        add(2);
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c >= 0x80) {
                const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                if (!tail || tail >= value.size() - i) reject("invalid UTF-8 string");
                const auto first = static_cast<unsigned char>(value[i + 1]);
                if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
                    (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90))
                    reject("invalid UTF-8 string");
                for (std::size_t j = 1; j <= tail; ++j) {
                    const auto byte = static_cast<unsigned char>(value[i + j]);
                    if (byte < 0x80 || byte > 0xbf) reject("invalid UTF-8 string");
                }
                add(tail + 1);
                i += tail;
            } else if (c == '"' || c == '\\' || c == '\b' || c == '\f' ||
                       c == '\n' || c == '\r' || c == '\t') add(2);
            else add(c < 0x20 ? 6 : 1);
        }
    }
    void json(const Json& value, std::size_t depth) {
        if (depth > depth_limit || ++nodes > node_limit) reject("JSON complexity budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>())))
            reject("nonportable JSON value");
        if (value.is_object()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) {
                quoted(key);
                add(1);
                json(child, depth + 1);
            }
        } else if (value.is_array()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) json(child, depth + 1);
        } else if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};

void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}

const std::string& identity(const Json& value) {
    if (!value.is_string()) reject("identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 128 ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) reject("invalid lexical identity");
    return text;
}

Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid source revision");
}

// Reject a lossy integer-to-double conversion before entering typed producers.
// Re-encode in the admitted JSON numeric category so 1 and 1.0, including -0.0,
// remain distinct raw intent. Avoid boundary casts that would be undefined.
Json encoded_number(double number, const Json& admitted) {
    if (admitted.is_number_unsigned()) {
        if (number < 0.0 || number >= 18446744073709551616.0)
            reject("numeric value is not losslessly representable");
        return Json(static_cast<std::uint64_t>(number));
    }
    if (admitted.is_number_integer()) {
        if (number < -9223372036854775808.0 || number >= 9223372036854775808.0)
            reject("numeric value is not losslessly representable");
        return Json(static_cast<std::int64_t>(number));
    }
    return Json(number);
}

double number(const Json& value) {
    if (!value.is_number()) reject("transform value must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) reject("transform value must be finite");
    if (encoded_number(result, value).dump() != value.dump())
        reject("numeric value is not losslessly representable");
    return result;
}

bool flag(const Json& value) {
    if (!value.is_boolean()) reject("transform flags must be booleans");
    return value.get<bool>();
}

Vec2 point2(const Json& value) {
    if (!value.is_array() || value.size() != 2) reject("planar point requires exactly XY");
    return {number(value.at(0)), number(value.at(1))};
}

Vec3 point3(const Json& value) {
    if (!value.is_array() || value.size() != 3) reject("model point requires exactly XYZ");
    return {number(value.at(0)), number(value.at(1)), number(value.at(2))};
}

Json encoded_point(Vec2 point, const Json& admitted) {
    return Json::array({encoded_number(point.x, admitted.at(0)),
        encoded_number(point.y, admitted.at(1))});
}
Json encoded_point(Vec3 point, const Json& admitted) {
    return Json::array({encoded_number(point.x, admitted.at(0)),
        encoded_number(point.y, admitted.at(1)), encoded_number(point.z, admitted.at(2))});
}

ArchitecturalGroupTransform model_transform(const Json& value) {
    fields(value, {"pivot", "offset", "rotation_z_radians", "scale", "flip_horizontal", "flip_vertical"});
    ArchitecturalGroupTransform result;
    result.pivot = point3(value.at("pivot"));
    result.offset = point3(value.at("offset"));
    result.rotation_z_radians = number(value.at("rotation_z_radians"));
    result.scale = number(value.at("scale"));
    if (result.scale <= 0.0) reject("model scale must be positive");
    result.flip_horizontal = flag(value.at("flip_horizontal"));
    result.flip_vertical = flag(value.at("flip_vertical"));
    const Json encoded = {{"pivot", encoded_point(result.pivot, value.at("pivot"))},
        {"offset", encoded_point(result.offset, value.at("offset"))},
        {"rotation_z_radians", encoded_number(result.rotation_z_radians, value.at("rotation_z_radians"))},
        {"scale", encoded_number(result.scale, value.at("scale"))},
        {"flip_horizontal", result.flip_horizontal}, {"flip_vertical", result.flip_vertical}};
    if (encoded.dump() != value.dump()) reject("model transform is not canonical");
    return result;
}

PlanarTransform planar_transform(const Json& value) {
    fields(value, {"pivot", "offset", "rotation_radians", "flip_horizontal", "flip_vertical"});
    PlanarTransform result;
    result.pivot = point2(value.at("pivot"));
    result.offset = point2(value.at("offset"));
    result.rotation_radians = number(value.at("rotation_radians"));
    result.flip_horizontal = flag(value.at("flip_horizontal"));
    result.flip_vertical = flag(value.at("flip_vertical"));
    const Json encoded = {{"pivot", encoded_point(result.pivot, value.at("pivot"))},
        {"offset", encoded_point(result.offset, value.at("offset"))},
        {"rotation_radians", encoded_number(result.rotation_radians, value.at("rotation_radians"))},
        {"flip_horizontal", result.flip_horizontal}, {"flip_vertical", result.flip_vertical}};
    if (encoded.dump() != value.dump()) reject("planar transform is not canonical");
    return result;
}

void shape(const Json& request) {
    Budget budget;
    budget.json(request, 0);
    fields(request, {"version", "expected_revision", "transaction_id", "architectural", "embedded",
        "annotations", "references", "message"});
    if (!request.at("version").is_number_integer() || request.at("version") != 1)
        reject("unsupported request version");
    if (revision(request.at("expected_revision")) == std::numeric_limits<Revision>::max())
        reject("source revision cannot advance");
    (void)identity(request.at("transaction_id"));
    if (!request.at("message").is_string()) reject("message must be a string");
    const auto& message = request.at("message").get_ref<const std::string&>();
    if (message.empty() || message.size() > 1024 ||
        std::any_of(message.begin(), message.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }) ||
        std::all_of(message.begin(), message.end(), [](unsigned char c) { return c == ' '; }))
        reject("message must be a bounded nonempty action");
    std::size_t count = 0;
    for (const auto* lane : {"architectural", "embedded", "annotations", "references"}) {
        const auto& rows = request.at(lane);
        if (!rows.is_array() || rows.size() > target_limit - count)
            reject("selection exceeds 1000 targets or has an invalid array");
        count += rows.size();
    }
    if (count == 0) reject("selection must contain at least one target");

    Ids objects, catalogs, annotation_owners, references;
    QualifiedIds embedded, annotations;
    for (const auto& row : request.at("architectural")) {
        fields(row, {"entity_id", "transform"});
        if (!objects.insert(identity(row.at("entity_id"))).second) reject("repeated persisted object");
        (void)model_transform(row.at("transform"));
    }
    for (const auto& row : request.at("embedded")) {
        fields(row, {"catalog_id", "instance_id", "transform"});
        const auto& catalog = identity(row.at("catalog_id"));
        const auto& instance = identity(row.at("instance_id"));
        if (objects.contains(catalog)) reject("overlapping persisted object and embedded catalog");
        catalogs.insert(catalog);
        if (!embedded.emplace(catalog, instance).second) reject("repeated qualified embedded member");
        (void)model_transform(row.at("transform"));
    }
    for (const auto& row : request.at("annotations")) {
        fields(row, {"owner_id", "child_id", "transform"});
        const auto& owner = identity(row.at("owner_id"));
        const auto& child = identity(row.at("child_id"));
        if (objects.contains(owner) || catalogs.contains(owner)) reject("overlapping persisted annotation owner");
        annotation_owners.insert(owner);
        if (!annotations.emplace(owner, child).second) reject("repeated qualified annotation child");
        (void)planar_transform(row.at("transform"));
    }
    for (const auto& row : request.at("references")) {
        fields(row, {"entity_id", "transform"});
        const auto& id = identity(row.at("entity_id"));
        if (objects.contains(id) || catalogs.contains(id) || annotation_owners.contains(id) ||
            !references.insert(id).second) reject("overlapping or repeated reference owner");
        (void)planar_transform(row.at("transform"));
    }
}

const std::string& text(const Json& value, const char* key) {
    return value.at(key).get_ref<const std::string&>();
}

void sort_targets(Json& request) {
    const auto single = [](const Json& a, const Json& b) { return text(a, "entity_id") < text(b, "entity_id"); };
    std::sort(request.at("architectural").begin(), request.at("architectural").end(), single);
    std::sort(request.at("references").begin(), request.at("references").end(), single);
    std::sort(request.at("embedded").begin(), request.at("embedded").end(), [](const Json& a, const Json& b) {
        return std::tie(text(a, "catalog_id"), text(a, "instance_id")) <
            std::tie(text(b, "catalog_id"), text(b, "instance_id"));
    });
    std::sort(request.at("annotations").begin(), request.at("annotations").end(), [](const Json& a, const Json& b) {
        return std::tie(text(a, "owner_id"), text(a, "child_id")) <
            std::tie(text(b, "owner_id"), text(b, "child_id"));
    });
}

bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}

void unchanged_assets(const DocumentSnapshot& source, const DocumentSnapshot& candidate) {
    if (source.assets().size() != candidate.assets().size()) reject("producer changed actual assets");
    for (const auto& [id, asset] : source.assets()) {
        const auto found = candidate.assets().find(id);
        if (found == candidate.assets().end() || asset != found->second ||
            asset.metadata.dump() != found->second.metadata.dump()) reject("producer changed actual assets");
    }
}

void persisted_owner(const DocumentSnapshot& source, const std::string& id) {
    const auto found = source.entities().find(id);
    if (found == source.entities().end() || found->second.id != id)
        reject("target is not an actual consistent persisted owner");
}
} // namespace

Json validate_ordinary_selection_edit_request(const Json& request) {
    shape(request);
    auto canonical = request;
    sort_targets(canonical);
    return canonical;
}

Json ordinary_selection_edit_semantic_targets(const Json& request) {
    const auto canonical = validate_ordinary_selection_edit_request(request);
    auto roster = Json::array();
    // Tag order and each lane's structural ID order define the canonical roster;
    // never concatenate qualified IDs into an ambiguous render/presentation key.
    for (const auto& row : canonical.at("annotations"))
        roster.push_back({{"kind", "annotation"}, {"owner_id", row.at("owner_id")}, {"child_id", row.at("child_id")}});
    for (const auto& row : canonical.at("embedded"))
        roster.push_back({{"kind", "embedded"}, {"catalog_id", row.at("catalog_id")}, {"instance_id", row.at("instance_id")}});
    for (const auto& row : canonical.at("architectural"))
        roster.push_back({{"kind", "object"}, {"entity_id", row.at("entity_id")}});
    for (const auto& row : canonical.at("references"))
        roster.push_back({{"kind", "reference"}, {"entity_id", row.at("entity_id")}});
    return roster;
}

ApplyEntityChanges replay_ordinary_selection_edit(const DocumentSnapshot& source, const Json& request) {
    const auto canonical = validate_ordinary_selection_edit_request(request);
    const auto expected = revision(canonical.at("expected_revision"));
    if (source.revision() != expected)
        throw DocumentError(DocumentErrorCode::stale_revision, "The ordinary selection edit source changed.");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, "The ordinary selection edit source is read-only.");

    std::vector<ArchitecturalGroupTransformTarget> architectural;
    std::vector<EmbeddedAssemblyGroupTarget> embedded;
    std::vector<PresentationAnnotationTransformTarget> annotations;
    std::vector<PresentationReferenceTransformTarget> references;
    for (const auto& row : canonical.at("architectural")) {
        persisted_owner(source, text(row, "entity_id"));
        architectural.push_back({text(row, "entity_id"), model_transform(row.at("transform"))});
    }
    for (const auto& row : canonical.at("embedded")) {
        persisted_owner(source, text(row, "catalog_id"));
        embedded.push_back({text(row, "catalog_id"), text(row, "instance_id"), std::nullopt,
            model_transform(row.at("transform"))});
    }
    for (const auto& row : canonical.at("annotations")) {
        persisted_owner(source, text(row, "owner_id"));
        annotations.push_back({{text(row, "owner_id"), text(row, "child_id")}, planar_transform(row.at("transform"))});
    }
    for (const auto& row : canonical.at("references")) {
        persisted_owner(source, text(row, "entity_id"));
        references.push_back({text(row, "entity_id"), planar_transform(row.at("transform"))});
    }

    std::vector<Entities> candidates;
    const auto admit = [&](const ApplyEntityChanges& command) {
        if (command.expected_revision != expected || !command.asset_changes.empty())
            reject("producer returned a different revision or asset authority");
        const auto admitted = Document::preview_command(source, command);
        unchanged_assets(source, admitted);
        candidates.push_back(admitted.entities());
    };
    if (!architectural.empty()) admit(architectural_group_transform_command(source, architectural,
        text(canonical, "transaction_id"), expected));
    // Every embedded row has an explicit operation and no copy identity. The
    // fallback can never replace per-row request authority.
    if (!embedded.empty()) admit(embedded_assembly_group_transform_command(source, embedded,
        ArchitecturalGroupTransform{}, expected));
    if (!annotations.empty() || !references.empty())
        admit(presentation_group_transform_command(source, annotations, references, expected));

    const auto composed = compose_mixed_selection_edit_candidates(source.entities(), candidates);
    ApplyEntityChanges result{expected, {}, {}, text(canonical, "message")};
    // Stable map order, using the existing composer's exact raw equality rule.
    for (const auto& [id, before] : source.entities()) {
        (void)before;
        if (!composed.contains(id)) result.entity_changes.push_back(EntityChange::erase(id));
    }
    for (const auto& [id, after] : composed) {
        const auto before = source.entities().find(id);
        if (before == source.entities().end() || !exact(before->second, after))
            result.entity_changes.push_back(EntityChange::upsert(after));
    }
    std::sort(result.entity_changes.begin(), result.entity_changes.end(), [](const EntityChange& a, const EntityChange& b) {
        const auto& a_id = a.kind == EntityChangeKind::upsert ? a.entity.id : a.entity_id;
        const auto& b_id = b.kind == EntityChangeKind::upsert ? b.entity.id : b.entity_id;
        return a_id < b_id;
    });
    const auto final = Document::preview_command(source, result);
    unchanged_assets(source, final);
    if (final.entities().size() != composed.size()) reject("final admission changed the composed owners");
    for (const auto& [id, after] : composed) {
        const auto found = final.entities().find(id);
        if (found == final.entities().end() || !exact(found->second, after))
            reject("final admission changed the composed raw result");
    }
    return result;
}

} // namespace sketch

#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/boundary_dimension.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Root = AssemblyInstancePropertyEditTarget;
constexpr std::size_t dimension_limit = 1000;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(std::string("Assembly dimension transform: ") + message);
}
void document_id(std::string_view value) {
    require(!value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "invalid document identity");
}

// Same complete-map framing ceilings as measurement resolution. Keep one raw
// ledger across both inputs and retained output copies, before semantic codecs.
struct RawBudget {
    std::size_t bytes{}, nodes{};
    void reserve(std::size_t count) {
        require(count <= 4 * 1024 * 1024 - nodes && count <= (64 * 1024 * 1024 - bytes) / 32,
            "raw node/byte budget exceeded");
        nodes += count; bytes += 32 * count;
    }
    void text(std::string_view value) {
        require(value.size() <= (64 * 1024 * 1024 - bytes) / 6, "raw byte budget exceeded");
        bytes += 6 * value.size();
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c < 0x80) continue;
            const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
                c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            require(tail && tail < value.size() - i, "invalid raw UTF-8");
            const auto first = static_cast<unsigned char>(value[i + 1]);
            require(!((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
                (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)), "invalid raw UTF-8");
            for (std::size_t j = 1; j <= tail; ++j) {
                const auto byte = static_cast<unsigned char>(value[i + j]);
                require(byte >= 0x80 && byte <= 0xbf, "invalid raw UTF-8");
            }
            i += tail;
        }
    }
    void read(const Json& value, std::size_t depth = 0) {
        require(depth <= 64, "raw depth budget exceeded");
        reserve(1);
        require(!value.is_binary() && !value.is_discarded() &&
            (!value.is_number_float() || std::isfinite(value.get<double>())), "nonportable raw JSON");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
    void entity(const Entity& value) {
        document_id(value.id);
        require(value.properties.is_object() && value.extensions.is_object(), "invalid raw entity envelope");
        text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
    void map(const AssemblyDocumentEntities& values) {
        require(values.size() <= 100000, "actual entity count exceeds bound");
        for (const auto& [id, value] : values) {
            require(value.id == id, "actual map identity is inconsistent");
            entity(value);
        }
    }
};

bool same_raw(const Entity& a, const Entity& b) {
    return a.id == b.id && a.type == b.type && a.required == b.required &&
        a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
bool has_root(const AssemblyDimensionTarget& target, const Root& root) {
    const auto& component = target.component;
    return component.catalog_id == root.catalog_id && component.instance_id == root.instance_id &&
        component.document_entity_id == root.document_entity_id;
}

// Borrow only framed actual rows. Catalog admission has already bounded all
// rows/definitions; geometry expansion is deliberately deferred to measurement.
const Json* root_row(const AssemblyDocumentEntities& actual, const Root& root) {
    const auto catalog = actual.find(root.catalog_id);
    if (catalog == actual.end()) return nullptr;
    require(catalog->second.type == "assembly_model", "root catalog has the wrong entity type");
    if (root.document_entity_id) {
        const auto owner = actual.find(*root.document_entity_id);
        if (owner == actual.end()) return nullptr;
        const auto value = decode_document_assembly_instance(owner->second);
        require(value.assembly_catalog_id == root.catalog_id && value.instance.id == root.instance_id,
            "independent root framing differs from its qualified identity");
        return &owner->second.properties.at("instance");
    }
    const auto& rows = catalog->second.properties.at("model").at("instances");
    require(rows.is_array(), "embedded root roster is invalid");
    const Json* found = nullptr;
    for (const auto& row : rows) {
        require(row.is_object() && row.contains("id") && row.at("id").is_string(), "invalid embedded root row");
        if (row.at("id") != root.instance_id) continue;
        require(!found, "embedded qualified root is duplicated");
        found = &row;
    }
    return found;
}

double finite(double value) {
    require(std::isfinite(value), "text transform arithmetic is unrepresentable");
    return value;
}
struct TextOperator {
    double cosine{}, sine{}, scale{};
    bool reflected{}, identity_axes{};
};
TextOperator text_operator(const AssemblyTransform& before, const AssemblyTransform& after) {
    const auto reflected = before.mirrored_y != after.mirrored_y;
    // Use the exact same axes as the admitted frames. Subtracting/reducing yaw
    // values can lose their actual trigonometric direction at huge finite angles.
    const auto [old_c, old_s] = assembly_rotation_components(before.rotation_radians);
    const auto [new_c, new_s] = assembly_rotation_components(after.rotation_radians);
    const bool identity_axes = !reflected && old_c == new_c && old_s == new_s;
    const auto sign = reflected ? -1.0 : 1.0;
    auto c = identity_axes ? 1.0 : finite(std::fma(new_c, old_c, sign * new_s * old_s));
    auto s = identity_axes ? 0.0 : finite(std::fma(new_s, old_c, -sign * new_c * old_s));
    const auto length = finite(std::hypot(c, s));
    require(length > 0, "text transform axes vanished");
    c /= length; s /= length;
    const auto scale = finite(after.scale / before.scale);
    require(scale > 0, "text transform scale underflow");
    return {c, s, scale, reflected, !reflected && c == 1 && s == 0};
}
Vec2 carry_position(Vec2 point, const AssemblyTransform& before, const AssemblyTransform& after,
    const TextOperator& op) {
    if (op.identity_axes && op.scale == 1) {
        // Preserve untouched coordinates and small translation at distant frames.
        return {before.translation_m.x == after.translation_m.x ? point.x :
            finite(point.x + finite(after.translation_m.x - before.translation_m.x)),
            before.translation_m.y == after.translation_m.y ? point.y :
            finite(point.y + finite(after.translation_m.y - before.translation_m.y))};
    }
    const auto x = finite(point.x - before.translation_m.x);
    const auto y = finite(point.y - before.translation_m.y) * (op.reflected ? -1.0 : 1.0);
    const auto turned_x = finite(std::fma(op.cosine, x, -op.sine * y));
    const auto turned_y = finite(std::fma(op.sine, x, op.cosine * y));
    return {finite(std::fma(op.scale, turned_x, after.translation_m.x)),
        finite(std::fma(op.scale, turned_y, after.translation_m.y))};
}
double carry_rotation(double angle, const TextOperator& op) {
    if (op.identity_axes) return angle;
    const auto [c, s] = assembly_rotation_components(angle);
    const auto y = op.reflected ? -s : s;
    const auto x_new = finite(std::fma(op.cosine, c, -op.sine * y));
    const auto y_new = finite(std::fma(op.sine, c, op.cosine * y));
    // A reflected unchanged direction keeps its authored numeric representation.
    if (x_new == c && y_new == s) return angle;
    require(x_new != 0 || y_new != 0, "transformed text direction vanished");
    return finite(std::atan2(y_new, x_new));
}

Entity encode_carried(const Entity& original, const BoundaryDimension& before,
    const BoundaryDimension& after) {
    // The owning codec requires matching IDs. Encode changed typed values over
    // the real original first; cloning only assigns the explicitly supplied ID.
    auto same_id = after; same_id.id = before.id;
    auto result = encode_boundary_dimension_entity(same_id, &original);
    result.id = after.id;
    if (before.assembly_target != after.assembly_target) {
        // The closed target was admitted by both codecs. Retain raw version,
        // form, path and axis; only genuinely changed qualified identity slots
        // come from the owning encoder's canonical result.
        const auto& encoded = result.properties.at("target").at("assembly");
        auto retained = original.properties.at("target").at("assembly");
        const auto& old_root = before.assembly_target->component;
        const auto& new_root = after.assembly_target->component;
        if (old_root.catalog_id != new_root.catalog_id) retained["catalog_id"] = encoded.at("catalog_id");
        if (old_root.instance_id != new_root.instance_id) retained["instance_id"] = encoded.at("instance_id");
        if (old_root.document_entity_id != new_root.document_entity_id)
            retained["document_entity_id"] = encoded.at("document_entity_id");
        result.properties.at("target").at("assembly") = std::move(retained);
    }
    // Retain each unchanged numeric field even when another field in its owning
    // presentation/coordinate envelope changed and the codec rewrote that group.
    auto& point = result.properties.at("text_position");
    const auto& raw_point = original.properties.at("text_position");
    if (before.text_position.x == after.text_position.x) point[0] = raw_point.at(0);
    if (before.text_position.y == after.text_position.y) point[1] = raw_point.at(1);
    if (before.presentation && after.presentation) {
        auto& presentation = result.properties.at("presentation");
        const auto& raw = original.properties.at("presentation");
        if (before.presentation->text_height_mm == after.presentation->text_height_mm)
            presentation["text_height_mm"] = raw.at("text_height_mm");
        if (before.presentation->rotation_radians == after.presentation->rotation_radians)
            presentation["rotation_radians"] = raw.at("rotation_radians");
    }
    return result;
}
} // namespace

std::vector<Entity> carry_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    const Root& source_root, const Root& destination_root, AssemblyDimensionTransformMode mode,
    const std::map<std::string, std::string, std::less<>>& fresh_dimension_ids) {
    require(mode == AssemblyDimensionTransformMode::move || mode == AssemblyDimensionTransformMode::clone,
        "unknown carry mode");
    require(source_root.part_path.empty() && destination_root.part_path.empty(), "targets must be qualified roots");
    (void)encode_assembly_dimension_target({source_root, AssemblyDimensionAxis::width});
    (void)encode_assembly_dimension_target({destination_root, AssemblyDimensionAxis::width});
    require(source_root.document_entity_id.has_value() == destination_root.document_entity_id.has_value(),
        "carry cannot change root form");
    const bool clone = mode == AssemblyDimensionTransformMode::clone;
    require(clone ? source_root != destination_root : source_root == destination_root,
        "roots do not match the requested carry mode");
    require(fresh_dimension_ids.size() <= dimension_limit && (clone || fresh_dimension_ids.empty()),
        "unexpected or overbound dimension identities");
    std::set<std::string, std::less<>> destinations;
    for (const auto& [old_id, new_id] : fresh_dimension_ids) {
        document_id(old_id); document_id(new_id);
        require(destinations.insert(new_id).second && !source.contains(new_id) && !candidate.contains(new_id),
            "fresh dimension identity is duplicated or collides");
    }
    RawBudget raw; raw.map(source); raw.map(candidate);
    AssemblyCatalogTransferBudget catalogs;
    for (const auto* actual : {&source, &candidate}) for (const auto& [id, value] : *actual) {
        (void)id;
        if (value.type == "assembly_model") admit_existing_assembly_catalog_work(value, catalogs);
    }
    const auto* original_row = root_row(source, source_root);
    const auto* destination_row = root_row(candidate, destination_root);
    require(original_row && destination_row, "actual source or destination root is missing");
    if (clone) {
        if (destination_root.document_entity_id)
            require(!source.contains(*destination_root.document_entity_id), "cloned document root already exists in source");
        else require(!root_row(source, destination_root), "cloned embedded root already exists in source");
        const auto* retained = root_row(candidate, source_root);
        require(retained && retained->dump() == original_row->dump(), "clone changed its original component row");
        if (source_root.document_entity_id)
            require(same_raw(source.at(*source_root.document_entity_id), candidate.at(*source_root.document_entity_id)),
                "clone changed its original component envelope");
    }

    std::vector<BoundaryDimension> dimensions;
    std::vector<AssemblyDimensionTarget> old_targets, new_targets;
    for (const auto& [id, entity] : source) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto version = inspect_boundary_dimension_version(entity);
        if (version.format != BoundaryDimensionFormat::supported_v6) continue;
        auto decoded = decode_boundary_dimension_entity(entity);
        require(decoded.dimension && decoded.dimension->assembly_target, "known assembly dimension is malformed");
        auto& dimension = *decoded.dimension;
        if (!has_root(*dimension.assembly_target, source_root)) continue;
        require(dimensions.size() < dimension_limit, "associated dimension count exceeds bound");
        const auto unchanged = candidate.find(id);
        require(unchanged != candidate.end() && same_raw(entity, unchanged->second),
            "candidate must retain associated original dimensions unchanged");
        require(!clone || fresh_dimension_ids.contains(id), "clone is missing a dimension identity mapping");
        // Admit the complete raw output carrier before copying it for encoding.
        raw.entity(entity);
        // Reserve a possible new default presentation and longer destination
        // identity slots without first constructing their owning JSON values.
        raw.reserve(32);
        if (clone) {
            raw.text(fresh_dimension_ids.at(id)); raw.text(destination_root.catalog_id);
            raw.text(destination_root.instance_id);
            if (destination_root.document_entity_id) raw.text(*destination_root.document_entity_id);
        }
        old_targets.push_back(*dimension.assembly_target);
        auto target = *dimension.assembly_target;
        target.component.catalog_id = destination_root.catalog_id;
        target.component.instance_id = destination_root.instance_id;
        target.component.document_entity_id = destination_root.document_entity_id;
        new_targets.push_back(std::move(target));
        dimensions.push_back(std::move(dimension));
    }
    require(!clone || fresh_dimension_ids.size() == dimensions.size(), "clone has extraneous dimension identity mappings");
    if (dimensions.empty()) return {};

    const auto before = resolve_assembly_dimensions(source, old_targets);
    const auto after = resolve_assembly_dimensions(candidate, new_targets);
    std::vector<BoundaryDimension> carried; carried.reserve(dimensions.size());
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        require(before[i].selected_node.type_id == after[i].selected_node.type_id,
            "carry replaced the selected component type");
        auto dimension = dimensions[i];
        const auto& old_frame = before[i].selected_node.transform;
        const auto& new_frame = after[i].selected_node.transform;
        const auto op = text_operator(old_frame, new_frame);
        dimension.text_position = carry_position(dimension.text_position, old_frame, new_frame, op);
        const auto old_angle = dimension.presentation ? dimension.presentation->rotation_radians : 0.0;
        const auto angle = carry_rotation(old_angle, op);
        if (angle != old_angle) {
            if (!dimension.presentation) dimension.presentation.emplace();
            dimension.presentation->rotation_radians = angle;
        }
        if (clone) {
            dimension.id = fresh_dimension_ids.at(dimension.id);
            dimension.assembly_target = new_targets[i];
            dimension.boundary_id = destination_root.document_entity_id.value_or(destination_root.catalog_id);
        }
        carried.push_back(std::move(dimension));
    }
    // Every actual path, mapping and arithmetic result has succeeded before the
    // first owning entity copy. Publication remains the enclosing caller's job.
    std::vector<Entity> result; result.reserve(carried.size());
    for (std::size_t i = 0; i < carried.size(); ++i) {
        if (!clone && carried[i] == dimensions[i]) continue;
        result.push_back(encode_carried(source.at(dimensions[i].id), dimensions[i], carried[i]));
    }
    return result;
}

} // namespace sketch

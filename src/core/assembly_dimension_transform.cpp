#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
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
using RootKey = std::tuple<std::string_view, std::string_view, std::optional<std::string_view>>;
RootKey root_key(const Root& root) {
    return {root.catalog_id, root.instance_id, root.document_entity_id
        ? std::optional<std::string_view>(*root.document_entity_id) : std::nullopt};
}

// Borrow only framed actual rows. Catalog admission has already bounded all
// rows/definitions; geometry expansion is deliberately deferred to measurement.
class RootRows {
public:
    explicit RootRows(const AssemblyDocumentEntities& actual) : actual_(actual) {}
    const Json* find(const Root& root) {
        return find(root_key(root));
    }
    // Keys borrow stable request identities or framed raw dimension targets.
    const Json* find(const RootKey& key) {
        if (const auto cached = resolved_.find(key); cached != resolved_.end()) return cached->second;
        const auto* row = find_uncached(key);
        resolved_.emplace(key, row);
        return row;
    }
private:
    const AssemblyDocumentEntities& actual_;
    std::map<RootKey, const Json*> resolved_;
    std::map<std::string_view, std::map<std::string_view, const Json*>> embedded_;

    const Json* find_uncached(const RootKey& root) {
        const auto& [catalog_id, instance_id, document_entity_id] = root;
        const auto catalog = actual_.find(catalog_id);
        if (catalog == actual_.end()) return nullptr;
        require(catalog->second.type == "assembly_model", "root catalog has the wrong entity type");
        if (document_entity_id) {
            const auto owner = actual_.find(*document_entity_id);
            if (owner == actual_.end()) return nullptr;
            const auto value = decode_document_assembly_instance(owner->second);
            require(value.assembly_catalog_id == catalog_id && value.instance.id == instance_id,
                "independent root framing differs from its qualified identity");
            return &owner->second.properties.at("instance");
        }
        auto cached = embedded_.find(catalog_id);
        if (cached == embedded_.end()) {
            const auto& rows = catalog->second.properties.at("model").at("instances");
            require(rows.is_array(), "embedded root roster is invalid");
            std::map<std::string_view, const Json*> indexed;
            for (const auto& row : rows) {
                require(row.is_object() && row.contains("id") && row.at("id").is_string(), "invalid embedded root row");
                const auto& id = row.at("id").get_ref<const std::string&>();
                const auto [found, inserted] = indexed.emplace(id, &row);
                // Preserve the single-root rule: only a requested duplicate is
                // refused here; unrelated rows remain the catalog codec's job.
                if (!inserted) found->second = nullptr;
            }
            cached = embedded_.emplace(catalog->first, std::move(indexed)).first;
        }
        const auto found = cached->second.find(instance_id);
        if (found == cached->second.end()) return nullptr;
        require(found->second, "embedded qualified root is duplicated");
        return found->second;
    }
};

// Both public APIs borrow their mapping. In particular, the existing single-
// root API never copies a caller's unchecked mapping to construct a request.
struct RequestView {
    const Root& source_root;
    const Root& destination_root;
    AssemblyDimensionTransformMode mode;
    const std::map<std::string, std::string, std::less<>>& fresh_dimension_ids;
};

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
std::vector<Entity> carry_requests(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    std::span<const RequestView> requests) {
    require(!requests.empty() && requests.size() <= dimension_limit, "request count exceeds bound or is empty");
    std::map<RootKey, std::size_t> source_requests, destination_requests;
    std::set<std::string, std::less<>> destinations;
    std::size_t mapping_count = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        const auto& source_root = request.source_root;
        const auto& destination_root = request.destination_root;
        require(request.mode == AssemblyDimensionTransformMode::move || request.mode == AssemblyDimensionTransformMode::clone ||
            request.mode == AssemblyDimensionTransformMode::detached_clone,
            "unknown carry mode");
        require(source_root.part_path.empty() && destination_root.part_path.empty(), "targets must be qualified roots");
        (void)encode_assembly_dimension_target({source_root, AssemblyDimensionAxis::width});
        (void)encode_assembly_dimension_target({destination_root, AssemblyDimensionAxis::width});
        if (request.mode == AssemblyDimensionTransformMode::detached_clone)
            require(!source_root.document_entity_id && destination_root.document_entity_id &&
                source_root.catalog_id == destination_root.catalog_id,
                "detached clone requires an embedded source and independent destination in the same catalog");
        else require(source_root.document_entity_id.has_value() == destination_root.document_entity_id.has_value(),
            "carry cannot change root form");
        const bool clone = request.mode != AssemblyDimensionTransformMode::move;
        require(clone ? source_root != destination_root : source_root == destination_root,
            "roots do not match the requested carry mode");
        require(request.fresh_dimension_ids.size() <= dimension_limit - mapping_count &&
            (clone || request.fresh_dimension_ids.empty()), "unexpected or overbound dimension identities");
        mapping_count += request.fresh_dimension_ids.size();
        require(source_requests.emplace(root_key(source_root), i).second, "source root is duplicated");
        require(destination_requests.emplace(root_key(destination_root), i).second, "destination root is duplicated");
        for (const auto& [old_id, new_id] : request.fresh_dimension_ids) {
            document_id(old_id); document_id(new_id);
            require(destinations.insert(new_id).second && !source.contains(new_id) && !candidate.contains(new_id),
                "fresh dimension identity is duplicated or collides");
        }
    }
    for (const auto& [key, index] : destination_requests) {
        const auto found = source_requests.find(key);
        require(found == source_requests.end() || (found->second == index &&
            requests[index].mode == AssemblyDimensionTransformMode::move), "source and destination rosters overlap");
    }

    RawBudget raw; raw.map(source); raw.map(candidate);
    AssemblyCatalogTransferBudget catalogs;
    for (const auto* actual : {&source, &candidate}) for (const auto& [id, value] : *actual) {
        (void)id;
        if (value.type == "assembly_model") admit_existing_assembly_catalog_work(value, catalogs);
    }
    RootRows source_rows(source), candidate_rows(candidate);
    for (const auto& request : requests) {
        const auto& source_root = request.source_root;
        const auto& destination_root = request.destination_root;
        const auto* original_row = source_rows.find(source_root);
        const auto* destination_row = candidate_rows.find(destination_root);
        require(original_row && destination_row, "actual source or destination root is missing");
        if (request.mode != AssemblyDimensionTransformMode::move) {
            if (destination_root.document_entity_id)
                require(!source.contains(*destination_root.document_entity_id), "cloned document root already exists in source");
            else require(!source_rows.find(destination_root), "cloned embedded root already exists in source");
            const auto* retained = candidate_rows.find(source_root);
            require(retained && retained->dump() == original_row->dump(), "clone changed its original component row");
            if (source_root.document_entity_id)
                require(same_raw(source.at(*source_root.document_entity_id), candidate.at(*source_root.document_entity_id)),
                    "clone changed its original component envelope");
            if (request.mode == AssemblyDimensionTransformMode::detached_clone)
                require(same_raw(source.at(source_root.catalog_id), candidate.at(source_root.catalog_id)),
                    "detached clone changed its original catalog envelope");
        }
    }

    std::vector<BoundaryDimension> dimensions;
    std::vector<AssemblyDimensionTarget> old_targets, new_targets;
    std::vector<std::size_t> dimension_requests;
    std::vector<std::size_t> dimension_counts(requests.size());
    for (const auto& [id, entity] : source) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto version = inspect_boundary_dimension_version(entity);
        if (version.format != BoundaryDimensionFormat::supported_v6) continue;
        auto decoded = decode_boundary_dimension_entity(entity);
        require(decoded.dimension && decoded.dimension->assembly_target, "known assembly dimension is malformed");
        auto& dimension = *decoded.dimension;
        const auto owner = source_requests.find(root_key(dimension.assembly_target->component));
        if (owner == source_requests.end()) continue;
        const auto& request = requests[owner->second];
        const auto& destination_root = request.destination_root;
        const bool clone = request.mode != AssemblyDimensionTransformMode::move;
        require(dimensions.size() < dimension_limit, "associated dimension count exceeds bound");
        const auto unchanged = candidate.find(id);
        require(unchanged != candidate.end() && same_raw(entity, unchanged->second),
            "candidate must retain associated original dimensions unchanged");
        require(!clone || request.fresh_dimension_ids.contains(id), "clone is missing a dimension identity mapping");
        // Admit the complete raw output carrier before copying it for encoding.
        raw.entity(entity);
        // Reserve a possible new default presentation and longer destination
        // identity slots without first constructing their owning JSON values.
        raw.reserve(32);
        if (clone) {
            raw.text(request.fresh_dimension_ids.at(id)); raw.text(destination_root.catalog_id);
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
        dimension_requests.push_back(owner->second);
        ++dimension_counts[owner->second];
    }
    for (std::size_t i = 0; i < requests.size(); ++i)
        require(requests[i].mode == AssemblyDimensionTransformMode::move ||
            requests[i].fresh_dimension_ids.size() == dimension_counts[i], "clone has extraneous dimension identity mappings");
    if (dimensions.empty()) return {};

    const auto before = resolve_assembly_dimensions(source, old_targets);
    const auto after = resolve_assembly_dimensions(candidate, new_targets);
    std::vector<BoundaryDimension> carried; carried.reserve(dimensions.size());
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        const auto& request = requests[dimension_requests[i]];
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
        if (request.mode != AssemblyDimensionTransformMode::move) {
            dimension.id = request.fresh_dimension_ids.at(dimension.id);
            dimension.assembly_target = new_targets[i];
            dimension.boundary_id = request.destination_root.document_entity_id.value_or(request.destination_root.catalog_id);
        }
        carried.push_back(std::move(dimension));
    }
    // Every actual path, mapping and arithmetic result has succeeded before the
    // first owning entity copy. Publication remains the enclosing caller's job.
    std::vector<Entity> result; result.reserve(carried.size());
    for (std::size_t i = 0; i < carried.size(); ++i) {
        if (requests[dimension_requests[i]].mode == AssemblyDimensionTransformMode::move && carried[i] == dimensions[i]) continue;
        result.push_back(encode_carried(source.at(dimensions[i].id), dimensions[i], carried[i]));
    }
    return result;
}
} // namespace

std::map<std::string, Root, std::less<>> inspect_embedded_assembly_dimension_clone_roster(
    const AssemblyDocumentEntities& source,
    std::span<const std::pair<std::string, std::string>> roots) {
    require(roots.size() <= 4096, "embedded clone root count exceeds bound");
    RawBudget raw; raw.map(source);
    AssemblyCatalogTransferBudget catalogs;
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalogs);
    }
    std::set<std::pair<std::string_view, std::string_view>> requested;
    std::set<std::string_view> requested_catalogs;
    RootRows rows(source);
    for (const auto& root : roots) {
        raw.text(root.first); raw.text(root.second);
        require(requested.emplace(root.first, root.second).second, "embedded clone root is duplicated");
        requested_catalogs.insert(root.first);
        const Root target{root.first, root.second, std::nullopt, {}};
        (void)encode_assembly_dimension_target({target, AssemblyDimensionAxis::width});
        require(rows.find(RootKey{root.first, root.second, std::nullopt}), "embedded clone root is missing");
    }
    const auto field = [](const Json& object, const char* name) -> const Json* {
        if (!object.is_object()) return nullptr;
        const auto found = object.find(name);
        return found == object.end() ? nullptr : &*found;
    };
    std::map<std::string, Root, std::less<>> result;
    std::vector<AssemblyDimensionTarget> targets;
    for (const auto& [id, entity] : source) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto* target = field(entity.properties, "target");
        const auto* assembly = target ? field(*target, "assembly") : nullptr;
        const auto* catalog = assembly ? field(*assembly, "catalog_id") : nullptr;
        const auto* instance = assembly ? field(*assembly, "instance_id") : nullptr;
        const auto* carrier = target ? field(*target, "entity_id") : nullptr;
        const bool selected_carrier = carrier && carrier->is_string() &&
            requested_catalogs.contains(carrier->get_ref<const std::string&>());
        const bool selected_catalog = catalog && catalog->is_string() &&
            requested_catalogs.contains(catalog->get_ref<const std::string&>());
        if (!selected_catalog) {
            require(!selected_carrier, "affected clone dimension has no qualified catalog target");
            continue;
        }
        require(instance && instance->is_string(), "affected clone dimension has no qualified instance target");
        if (!requested.contains({catalog->get_ref<const std::string&>(), instance->get_ref<const std::string&>()})) continue;
        const auto* owner = field(*assembly, "document_entity_id");
        if (owner && !owner->is_null()) {
            require(owner->is_string(), "affected clone dimension has an invalid independent owner");
            const auto& owner_id = owner->get_ref<const std::string&>();
            document_id(owner_id);
            require(rows.find(RootKey{catalog->get_ref<const std::string&>(),
                instance->get_ref<const std::string&>(), std::string_view(owner_id)}),
                "affected clone dimension has no actual independent root");
            continue;
        }
        const auto decoded = decode_boundary_dimension_entity(entity);
        require(owner && decoded.supported() && decoded.version == 6 && decoded.dimension &&
            decoded.dimension->kind == BoundaryDimensionKind::assembly_extent && decoded.dimension->assembly_target &&
            !decoded.dimension->assembly_target->component.document_entity_id,
            "affected embedded clone dimension requires a supported v6 extent");
        const auto& component = decoded.dimension->assembly_target->component;
        require(component.catalog_id == catalog->get_ref<const std::string&>() &&
            component.instance_id == instance->get_ref<const std::string&>() &&
            decoded.dimension->boundary_id == component.catalog_id, "embedded clone dimension target framing differs");
        require(result.size() < dimension_limit, "embedded clone dimension count exceeds bound");
        raw.entity(entity);
        targets.push_back(*decoded.dimension->assembly_target);
        result.emplace(id, component);
    }
    if (!targets.empty()) (void)resolve_assembly_dimensions(source, targets);
    return result;
}

Entity assembly_dimension_clone_dependency_remainder(const Entity& entity) {
    RawBudget raw; raw.entity(entity);
    const auto decoded = decode_boundary_dimension_entity(entity);
    require(decoded.supported() && decoded.version == 6 && decoded.dimension &&
        decoded.dimension->kind == BoundaryDimensionKind::assembly_extent && decoded.dimension->assembly_target &&
        !decoded.dimension->assembly_target->component.document_entity_id,
        "dependency remainder requires a supported embedded v6 extent");
    auto result = entity;
    auto& target = result.properties.at("target");
    target.erase("entity_id");
    auto& assembly = target.at("assembly");
    for (const auto* name : {"version", "form", "catalog_id", "instance_id", "document_entity_id", "part_path", "axis"})
        assembly.erase(name);
    return result;
}

std::vector<Entity> carry_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    const Root& source_root, const Root& destination_root, AssemblyDimensionTransformMode mode,
    const std::map<std::string, std::string, std::less<>>& fresh_dimension_ids) {
    const RequestView request{source_root, destination_root, mode, fresh_dimension_ids};
    return carry_requests(source, candidate, std::span<const RequestView>(&request, 1));
}

std::vector<Entity> carry_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    std::span<const AssemblyDimensionTransformRequest> requests) {
    require(!requests.empty() && requests.size() <= dimension_limit, "request count exceeds bound or is empty");
    std::vector<RequestView> views;
    views.reserve(requests.size());
    for (const auto& request : requests)
        views.push_back({request.source_root, request.destination_root, request.mode, request.fresh_dimension_ids});
    return carry_requests(source, candidate, views);
}

std::vector<Entity> carry_hosted_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    std::span<const std::string> actual_host_ids) {
    return carry_hosted_assembly_dimension_transform_entities(source,candidate,actual_host_ids,dimension_limit);
}

static std::vector<Entity> carry_hosted_dimension_contribution(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    std::span<const std::string> actual_host_ids, std::size_t maximum_associated_dimensions,
    bool wall_opening_hosts) {
    require(maximum_associated_dimensions<=dimension_limit, "hosted dimension association budget exceeds bound");
    require(actual_host_ids.size() <= 100000, "actual host roster exceeds entity bound");
    // Admit both complete raw inputs and all catalog work before indexing rows
    // or decoding targets. Discovery borrows these already bounded carriers.
    RawBudget raw; raw.map(source); raw.map(candidate);
    AssemblyCatalogTransferBudget catalogs;
    for (const auto* actual : {&source, &candidate}) for (const auto& [id, entity] : *actual) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalogs);
    }
    std::set<std::string_view> hosts;
    for (const auto& id : actual_host_ids) {
        document_id(id);
        const auto original = source.find(id), current = candidate.find(id);
        require(hosts.insert(id).second && original != source.end() && current != candidate.end(),
            "actual host roster must name unique persisted source and candidate hosts");
        const auto& type = original->second.type;
        const bool supported=wall_opening_hosts ? (type=="wall" || type=="opening") :
            (type == "wall" || type == "column" || type == "beam" || type == "slab" ||
                type == "roof" || type == "stair" || type == "railing");
        require(supported && current->second.type == type,
            "actual host roster must retain supported physical host types");
    }

    if (hosts.empty()) return {};
    RootRows source_rows(source), candidate_rows(candidate);
    std::map<RootKey, bool> hosted_roots;
    std::vector<AssemblyDimensionTransformRequest> requests;
    std::size_t associated_count{};
    for (const auto& [id, entity] : source) {
        (void)id;
        if (!can_recognize_boundary_dimension_entity_type(entity.type) ||
            inspect_boundary_dimension_version(entity).format != BoundaryDimensionFormat::supported_v6) continue;
        const auto& target = entity.properties.at("target");
        require(target.is_object() && target.size() == 2 && target.contains("entity_id") &&
            target.contains("assembly"), "known assembly dimension target is not closed");
        const auto& assembly = target.at("assembly");
        auto qualified = decode_assembly_dimension_target(assembly).component;
        require(target.at("entity_id").is_string() && target.at("entity_id") ==
            qualified.document_entity_id.value_or(qualified.catalog_id),
            "known assembly dimension carrier differs from its qualified target");
        if (qualified.document_entity_id) continue;
        // Borrow keys from the original raw target, never the temporary decoded
        // root. Both row-index and admission caches outlive each loop iteration.
        const RootKey key{assembly.at("catalog_id").get_ref<const std::string&>(),
            assembly.at("instance_id").get_ref<const std::string&>(), std::nullopt};
        const auto admit = [&]() {
            const auto* original = source_rows.find(key);
            if (!original) return false;
            const auto placement = original->find("placement");
            if (placement == original->end()) return false;
            require(placement->is_object() && placement->contains("host_entity_id") &&
                placement->at("host_entity_id").is_string(), "embedded source placement is malformed");
            const auto& host = placement->at("host_entity_id").get_ref<const std::string&>();
            if (!hosts.contains(host)) return false;
            const auto* current = candidate_rows.find(key);
            require(current && original->size() == current->size() && current->contains("placement"),
                "hosted carry changed its qualified root framing");
            const auto& proposed_placement = current->at("placement");
            require(proposed_placement.is_object() && proposed_placement.contains("host_entity_id") &&
                proposed_placement.at("host_entity_id").dump() == placement->at("host_entity_id").dump(),
                "hosted carry changed its original host binding");
            for (const auto& [field, value] : original->items()) if (field != "placement")
                require(current->contains(field) && current->at(field).dump() == value.dump(),
                    "hosted carry changed fields outside placement");
            return true;
        };
        const auto known = hosted_roots.find(key);
        const bool first = known == hosted_roots.end();
        const bool admitted = first ? hosted_roots.emplace(key, admit()).first->second : known->second;
        if (!admitted) continue;
        require(associated_count < maximum_associated_dimensions, "associated hosted dimension count exceeds bound");
        ++associated_count;
        if (!first) continue;
        qualified.part_path.clear();
        requests.push_back({qualified, qualified, AssemblyDimensionTransformMode::move, {}});
    }
    if (requests.empty()) return {};
    // One shared batch preserves closed paths/axes and original dimension
    // envelopes, then resolves actual selected-node frames on both complete maps.
    // Raw placement equality cannot establish equality of host-derived frames.
    return carry_assembly_dimension_transform_entities(source, candidate, requests);
}

std::vector<Entity> carry_hosted_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    std::span<const std::string> actual_host_ids, std::size_t maximum_associated_dimensions) {
    return carry_hosted_dimension_contribution(source,candidate,actual_host_ids,maximum_associated_dimensions,false);
}

std::vector<Entity> carry_wall_hosted_assembly_dimension_transform_entities(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate,
    std::span<const std::string> actual_host_ids) {
    return carry_hosted_dimension_contribution(source,candidate,actual_host_ids,dimension_limit,true);
}

void validate_ordinary_assembly_dimension_changes(
    const AssemblyDocumentEntities& source, std::span<const std::string> dimension_ids) {
    if (dimension_ids.empty()) return;
    require(dimension_ids.size() <= dimension_limit, "ordinary dimension change roster exceeds bound");
    std::set<std::string, std::less<>> unique;
    for (const auto& id : dimension_ids) {
        const auto found = source.find(id);
        require(unique.insert(id).second && found != source.end() && found->second.id == id &&
            can_recognize_boundary_dimension_entity_type(found->second.type) &&
            inspect_boundary_dimension_version(found->second).format == BoundaryDimensionFormat::supported_v6,
            "ordinary dimension change must name a unique supported source dimension");
    }
    const auto scope = constraint_phase_scope(source);
    for (const auto& id : dimension_ids)
        require(!scope.inactive_owner_ids.contains(id), "ordinary editing cannot change an inactive dimension");
    for (const auto& [registry_id, entity] : source) {
        (void)registry_id;
        if (entity.type != "model_phases") continue;
        const auto phases = ModelPhases::from_json(entity.properties.at("model"));
        if (!phases.active_alternative()) continue;
        for (const auto& id : dimension_ids)
            require(std::find(phases.baseline_ids().begin(), phases.baseline_ids().end(), id) ==
                phases.baseline_ids().end(), "ordinary editing requires replacement of a protected baseline dimension");
    }
}

} // namespace sketch

#include "sketch/dxf_annotation_source.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/dxf_phase_source.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/svg_admission.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Owners = std::map<std::string, Entity, std::less<>>;
using References = std::map<std::string, std::string, std::less<>>;
References output_view_sources(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget);

[[noreturn]] void refuse(const std::string& reason) {
    throw std::invalid_argument("Native DXF annotation source: " + reason);
}
void require(bool condition, const std::string& reason) { if (!condition) refuse(reason); }
std::string reference(const Json& value) {
    require(value.is_string(), "typed owner reference must be a string");
    const auto& text = value.get_ref<const std::string&>();
    require(!text.empty() && text.size() <= 255 &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }) &&
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
        "invalid document owner reference");
    return text;
}
const std::string& local_view_id(const Json& value) {
    require(value.is_string(), "local view identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    require(!text.empty() && !std::all_of(text.begin(), text.end(),
        [](unsigned char c) { return std::isspace(c); }), "blank local view identity");
    return text;
}
void supported_source(const Entity& source) {
    require(native_dxf_annotation_source_type(source.type), "unsupported support-owner type");
    require(source.properties.is_object() && source.extensions.is_object(), "invalid Entity envelope");
    const auto& p = source.properties;
    if (source.type == kAnnotationEntityType) {
        require(p.contains("schema") && p.at("schema") == "sketch.annotation_entity" &&
            p.contains("version") && p.at("version").is_number_integer() &&
            (p.at("version") == 1 || p.at("version") == 2 || p.at("version") == 3),
            "unsupported annotation entity schema/version");
        require(p.contains("state") && p.at("state").is_object(), "missing annotation state");
        const auto& state = p.at("state");
        require(state.contains("version") && state.at("version").is_number_integer(), "missing annotation state version");
        const auto& version = state.at("version");
        require(version >= 1 && version <= 11, "unsupported annotation state version");
        for (const auto* slot : {"labels", "symbols", "overrides"})
            require(state.contains(slot) && state.at(slot).is_array() && state.at(slot).size() <= 100'000,
                "invalid annotation collection");
    } else {
        require(p.contains("dimension_version") && p.at("dimension_version").is_number_integer() &&
            p.contains("dimension_kind") && p.at("dimension_kind").is_string(), "missing dimension version/kind");
        const auto& version = p.at("dimension_version");
        const auto& kind = p.at("dimension_kind");
        require(((version == 1 || version == 2) &&
                (kind == "segment_length" || kind == "angle" || kind == "area")) ||
            (version == 3 && kind == "segment_length") || (version == 4 && kind == "wall_axis_length"),
            "unsupported dimension schema/version/kind");
        require(p.contains("target") && p.at("target").is_object() && p.at("target").contains("entity_id"),
            "missing dimension target owner");
    }
}
bool owner_override(std::string_view kind) {
    return kind == "area" || kind == "area_name" || kind == "area_calculation" ||
        kind == "wall_dimension" || kind == "object";
}
bool local_child_override(const Json& row, const Owners& authored, const std::set<std::string, std::less<>>& children) {
    if (row.at("target_kind") != "object") return false;
    const auto& id = row.at("target_id").get_ref<const std::string&>();
    const auto owner = authored.find(id);
    const auto appearance_owner = [](std::string_view type) {
        return type == "wall" || type == "opening" || type == "slab" || type == "room" ||
            type == "assembly_instance" || type == "roof_join" || type == "column" || type == "beam" ||
            type == "stair" || type == "railing" || type == "roof";
    };
    // Match supportsObjectAppearance and building_entity's canonical vocabulary
    // without importing its native solid-builder dependency into this raw pass.
    // Other same-spelled document records do not shadow local artwork.
    return (owner == authored.end() || !appearance_owner(owner->second.type)) && children.contains(id);
}
template<class Callback> void references(const Entity& source, const Owners& authored, Callback visit,
    NativeDxfWallSourceWorkBudget* work_budget) {
    supported_source(source);
    if (source.type != kAnnotationEntityType) {
        visit(source.properties.at("target").at("entity_id"), std::string_view{});
        return;
    }
    const auto& state = source.properties.at("state");
    const auto child_ids = native_dxf_annotation_child_identity_ids(source);
    const std::set<std::string, std::less<>> children(child_ids.begin(), child_ids.end());
    for (const auto* slot : {"labels", "symbols"}) for (const auto& child : state.at(slot)) {
        require(child.is_object() && child.contains("placement") && child.at("placement").is_object(),
            "missing annotation child placement");
        const auto& placement = child.at("placement");
        if (placement.contains("layer_id")) {
            require(placement.at("layer_id").is_string(), "invalid annotation child layer");
            // Explicit empty IDs retain legacy unscoped placement semantics.
            if (!placement.at("layer_id").get_ref<const std::string&>().empty())
                visit(placement.at("layer_id"), std::string_view{"layer"});
        }
    }
    const auto view_sources = output_view_sources(source, authored, work_budget);
    for (const auto& row : state.at("overrides")) {
        require(row.is_object() && row.contains("target_kind") && row.at("target_kind").is_string() &&
            row.contains("target_id") && row.at("target_id").is_string(), "invalid presentation target");
        const auto& kind = row.at("target_kind").get_ref<const std::string&>();
        require(owner_override(kind) || kind == "output_view", "unsupported presentation target kind");
        // Saved output-view IDs belong to the sheet/view model's own namespace.
        if (owner_override(kind) && !local_child_override(row, authored, children))
            visit(row.at("target_id"), std::string_view{});
        if (kind == "output_view")
            visit(Json(view_sources.at(row.at("target_id").get_ref<const std::string&>())), std::string_view{"sheet_view_model"});
    }
}
void charge(std::size_t& consumed, std::size_t amount, std::size_t maximum, const char* reason) {
    require(consumed <= maximum && amount <= maximum - consumed, reason);
    consumed += amount;
}
void work(NativeDxfWallSourceWorkBudget& budget, std::size_t amount) {
    auto& b = budget.catalog_transfer;
    charge(b.consumed_validation_work, amount, b.max_validation_work, "cumulative support validation work limit");
}
void product_work(NativeDxfWallSourceWorkBudget& budget, std::size_t a, std::size_t b) {
    require(!b || a <= native_dxf_phase_source_work_limit / b, "support work product limit");
    work(budget, a * b);
}
References output_view_sources(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget) {
    if (source.type != kAnnotationEntityType) return {};
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    const auto& rows = source.properties.at("state").at("overrides");
    require(rows.is_array() && rows.size() <= native_dxf_phase_source_node_limit, "view override inventory limit");
    product_work(budget, rows.size(), 512);
    References requested;
    for (const auto& row : rows)
        if (row.at("target_kind") == "output_view") {
            const auto& id = local_view_id(row.at("target_id"));
            require(id.size() <= 256, "output view target identity limit");
            work(budget, id.size() + 1);
            requested.emplace(id, std::string{});
        }
    if (requested.empty()) return {};
    require(authored.size() <= native_dxf_phase_source_owner_limit, "view companion owner inventory limit");
    product_work(budget, authored.size(), 256);
    std::set<std::string, std::less<>> ambiguous;
    std::size_t view_count = 0;
    std::size_t lookup_steps = 1;
    for (auto count = requested.size(); count > 1; count = (count + 1) / 2) ++lookup_steps;
    for (const auto& [owner_id, owner] : authored) {
        if (owner.type != "sheet_view_model") continue;
        require(owner.id == owner_id && owner.properties.is_object(), "view companion identity mismatch");
        const auto& model = owner.properties.at("model");
        const auto& views = model.at("views");
        require(views.is_array(), "view companion inventory shape");
        charge(view_count, views.size(), native_dxf_phase_source_node_limit, "view companion inventory limit");
        product_work(budget, views.size(), 512);
        for (const auto& view : views) {
            const auto& id = local_view_id(view.at("id"));
            product_work(budget, id.size() + 1, lookup_steps);
            const auto found = requested.find(id);
            if (found == requested.end()) continue;
            if (!found->second.empty()) ambiguous.insert(id);
            else found->second = reference(Json(owner_id));
        }
    }
    for (const auto& [id, owner] : requested)
        require(!owner.empty() && !ambiguous.contains(id), "output view requires one unambiguous actual companion: " + id);
    // This reads only typed identities. The graph admits and validates each
    // selected companion with its strict codec before granting publication.
    return requested;
}
std::size_t raw(const Json& value, NativeDxfWallSourceWorkBudget& budget, std::size_t depth = 0) {
    require(depth <= native_dxf_phase_source_depth_limit, "raw JSON depth limit");
    auto& b = budget.catalog_transfer;
    charge(b.consumed_json_nodes, 1, b.max_json_nodes, "cumulative raw support node limit");
    work(budget, 1);
    const auto bytes = [&](std::size_t amount) {
        charge(b.consumed_json_bytes, amount, b.max_json_bytes, "cumulative raw support byte limit");
    };
    const auto text = [&](const std::string& text) {
        require(text.size() <= native_dxf_phase_source_byte_limit / 6, "raw support string limit");
        bytes(text.size() * 6 + 3);
    };
    std::size_t nodes = 1;
    if (value.is_string()) text(value.get_ref<const std::string&>());
    else if (value.is_object()) {
        bytes(2);
        for (const auto& [key, child] : value.items()) { text(key); bytes(1); nodes += raw(child, budget, depth + 1); }
    } else if (value.is_array()) {
        bytes(2);
        for (const auto& child : value) { bytes(1); nodes += raw(child, budget, depth + 1); }
    } else if (value.is_number()) {
        require(!value.is_number_float() || std::isfinite(value.get<double>()), "nonfinite support number");
        bytes(32);
    } else { require(value.is_null() || value.is_boolean(), "unsupported JSON value"); bytes(5); }
    return nodes;
}
std::size_t raw_owner(const Entity& source, NativeDxfWallSourceWorkBudget& budget) {
    require(source.properties.is_object() && source.extensions.is_object(), "invalid raw owner envelope");
    std::size_t nodes = raw(Json(source.id), budget);
    nodes += raw(Json(source.type), budget);
    nodes += raw(source.properties, budget);
    nodes += raw(source.extensions, budget);
    nodes += raw(Json(source.required), budget);
    return nodes;
}
const Entity& actual(const Owners& authored, const std::string& id, std::string_view role = {}) {
    const auto found = authored.find(id);
    require(found != authored.end() && found->second.id == id, "missing or mismatched actual target owner " + id);
    require(role.empty() || found->second.type == role, "typed target owner role differs " + id);
    return found->second;
}
// Reserve nonlinear retained-target work using only actual schema-owned slots.
// Opaque metadata pays raw admission, never speculative topology expansion.
void reserve_target(const Entity& target, NativeDxfWallSourceWorkBudget& budget) {
    std::size_t edges = 0, passes = 1, growth = 0;
    const auto count_edges = [&](const Json& rows) {
        require(rows.is_array() && rows.size() <= 16'384, "retained target topology limit");
        edges = std::max(edges, rows.size());
    };
    const auto visit = [&](const auto& self, const Json& item, std::size_t depth) -> void {
        require(depth <= native_dxf_phase_source_depth_limit, "retained replay depth limit");
        work(budget, 1);
        if (!item.is_object()) return;
        if (item.contains("kind") && item.at("kind") == "insert_vertex")
            charge(growth, 1, 16'384, "retained replay growth limit");
        for (const auto* slot : {"segments", "boundary", "edges", "replacement_segments"})
            if (item.contains(slot) && item.at(slot).is_array()) count_edges(item.at(slot));
        for (const auto* slot : {"operations", "transforms", "edits"}) if (item.contains(slot)) {
            const auto& rows = item.at(slot);
            require(rows.is_array(), "retained replay operation shape");
            charge(passes, rows.size(), 16'384, "retained replay operation limit");
            for (const auto& row : rows) self(self, row, depth + 1);
        }
        for (const auto* slot : {"value", "edit", "replacement_authoring", "source_boundary_authoring", "source_boundary", "outer"})
            if (item.contains(slot)) {
                const auto& nested = item.at(slot);
                if (nested.is_array()) for (const auto& row : nested) self(self, row, depth + 1);
                else self(self, nested, depth + 1);
            }
    };
    if (target.type == "measurement_linework") {
        require(target.properties.contains("model"), "missing measured target model");
        visit(visit, target.properties.at("model"), 0);
    } else if (can_recognize_boundary_entity_type(target.type)) {
        require(target.properties.contains("segments"), "missing retained target segments");
        count_edges(target.properties.at("segments"));
    } else require(target.type == "wall", "unsupported dimension target type");
    require(edges <= 16'384 - growth, "retained target final topology limit");
    edges += growth;
    // Decode/replay and stable-edge/chain resolution may each repeat topology.
    product_work(budget, edges * edges, passes * 16);
}
std::string mapped(const Json& value, const References& mapping) {
    const auto id = reference(value);
    const auto found = mapping.find(id);
    require(found != mapping.end(), "typed reference missing from destination map " + id);
    (void)reference(Json(found->second));
    return found->second;
}
} // namespace

bool native_dxf_annotation_source_type(std::string_view type) noexcept {
    return type == kAnnotationEntityType || can_recognize_boundary_dimension_entity_type(type);
}
std::vector<std::string> native_dxf_annotation_child_identity_ids(const Entity& source) {
    supported_source(source);
    if (source.type != kAnnotationEntityType) return {};
    std::set<std::string, std::less<>> result;
    const auto& state = source.properties.at("state");
    for (const auto* slot : {"labels", "symbols"}) for (const auto& child : state.at(slot)) {
        require(child.is_object() && child.contains("id") && child.at("id").is_string(), "invalid annotation child identity");
        const auto& id = child.at("id").get_ref<const std::string&>();
        require(!id.empty() && id.size() <= 256 && result.insert(id).second, "duplicate or oversized annotation child identity");
    }
    return {result.begin(), result.end()};
}
References native_dxf_annotation_source_dependencies(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget) {
    References result;
    references(source, authored, [&](const Json& value, std::string_view role) {
        const auto id = reference(value);
        const auto [found, inserted] = result.emplace(id, role);
        require(inserted || role.empty() || found->second.empty() || found->second == role,
            "conflicting support reference roles " + id);
        if (!role.empty()) found->second = role;
    }, work_budget);
    return result;
}
void remap_native_dxf_annotation_source_dependencies(Entity& source,
    const Owners& authored, const References& owner_mapping, const References& context_mapping,
    const NativeDxfAnnotationChildMaps& child_mapping, const NativeDxfAnnotationChildMaps& sheet_view_mapping,
    NativeDxfWallSourceWorkBudget* work_budget) {
    (void)native_dxf_annotation_source_dependencies(source, authored, work_budget);
    const auto view_sources = output_view_sources(source, authored, work_budget);
    const auto child_ids = native_dxf_annotation_child_identity_ids(source);
    const std::set<std::string, std::less<>> children(child_ids.begin(), child_ids.end());
    const auto child_scope = child_mapping.find(source.id);
    if (child_scope != child_mapping.end()) {
        require(child_scope->second.size() == children.size(), "annotation child mapping is incomplete");
        std::set<std::string, std::less<>> destinations;
        for (const auto& id : children) {
            const auto found = child_scope->second.find(id);
            require(found != child_scope->second.end() && !found->second.empty() && found->second.size() <= 256 &&
                destinations.insert(found->second).second, "invalid or noninjective annotation child mapping");
        }
    }
    auto result = source;
    if (result.type != kAnnotationEntityType) {
        auto& id = result.properties.at("target").at("entity_id");
        id = mapped(id, owner_mapping);
    } else {
        auto& state = result.properties.at("state");
        for (const auto* slot : {"labels", "symbols"}) for (auto& child : state.at(slot)) {
            if (child_scope != child_mapping.end())
                child.at("id") = child_scope->second.at(child.at("id").get_ref<const std::string&>());
            auto& placement = child.at("placement");
            if (placement.contains("layer_id") && !placement.at("layer_id").get_ref<const std::string&>().empty()) {
                auto& id = placement.at("layer_id");
                id = mapped(id, context_mapping);
            }
        }
        for (auto& row : state.at("overrides")) {
            if (owner_override(row.at("target_kind").get_ref<const std::string&>())) {
                auto& id = row.at("target_id");
                if (local_child_override(row, authored, children)) {
                    if (child_scope != child_mapping.end())
                        id = child_scope->second.at(id.get_ref<const std::string&>());
                } else id = mapped(id, owner_mapping);
            }
            if (row.at("target_kind") == "output_view") {
                auto& id = row.at("target_id");
                const auto companion = view_sources.at(id.get_ref<const std::string&>());
                const auto scope = sheet_view_mapping.find(companion);
                if (scope != sheet_view_mapping.end()) {
                    const auto found = scope->second.find(local_view_id(id));
                    require(found != scope->second.end(), "local view missing from destination map");
                    const auto& destination = found->second;
                    require(destination.size() <= 256, "mapped output view target identity limit");
                    if (work_budget) work(*work_budget, destination.size() + 1);
                    (void)local_view_id(Json(destination));
                    id = destination;
                }
            }
        }
    }
    // Mapped owners are not present in the source evidence map. Recheck only
    // intrinsic shape/local identities; destination closure is the graph's job.
    (void)native_dxf_annotation_child_identity_ids(result);
    source = std::move(result);
}
void admit_native_dxf_annotation_source_work(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget& budget, std::size_t codec_replays) {
    if (!native_dxf_annotation_source_type(source.type)) return;
    require(codec_replays > 0 && codec_replays <= 8, "invalid support consumer replay reservation");
    const auto& b = budget.catalog_transfer;
    require(b.max_json_bytes <= native_dxf_phase_source_byte_limit && b.max_json_nodes <= native_dxf_phase_source_node_limit &&
        b.max_validation_work <= native_dxf_phase_source_work_limit && b.consumed_json_bytes <= b.max_json_bytes &&
        b.consumed_json_nodes <= b.max_json_nodes && b.consumed_validation_work <= b.max_validation_work,
        "invalid shared support ledger");
    const auto nodes = raw_owner(source, budget);
    product_work(budget, nodes, 32 * codec_replays);
    supported_source(source);
    (void)native_dxf_annotation_source_dependencies(source, authored, &budget);
    if (source.type == kAnnotationEntityType) {
        const auto& state = source.properties.at("state");
        // Reserve the built-in catalog's construction/validation and searches
        // before its codec-owned static initialization. This deliberately caps
        // admitted work without loading a catalog or artwork in preflight.
        constexpr std::size_t catalog_work = 65'536;
        work(budget, catalog_work);
        product_work(budget, state.at("symbols").size(), 4096 * codec_replays);
        for (const auto& symbol : state.at("symbols")) {
            if (symbol.contains("definition")) {
                const auto& definition = symbol.at("definition");
                require(definition.is_object() && definition.contains("preview") && definition.at("preview").is_array() &&
                    definition.at("preview").size() <= 10'000, "invalid pinned symbol preview");
                product_work(budget, definition.at("preview").size(), 32 * codec_replays);
            }
            if (symbol.contains("pinned_svg")) {
                require(symbol.at("pinned_svg").is_string(), "invalid pinned SVG text");
                const auto& svg = symbol.at("pinned_svg").get_ref<const std::string&>();
                if (!svg.empty()) {
                    // Use the actual SVG admission consumer, including decoded
                    // CSS selectors and inherited resources. Charge its work
                    // before execution and reserve every actual later codec
                    // replay. Failed attempts remain billed.
                    validate_svg_structure(svg, [&](std::size_t amount) {
                        product_work(budget, amount, 1 + codec_replays);
                    });
                }
            }
        }
    } else {
        const auto& target = actual(authored, reference(source.properties.at("target").at("entity_id")));
        product_work(budget, raw_owner(target, budget), 32);
        reserve_target(target, budget);
    }
}
void validate_native_dxf_annotation_source(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget) {
    supported_source(source);
    const auto& owner = actual(authored, source.id, source.type);
    require(owner == source, "support owner differs from actual source inventory");
    for (const auto& [id, role] : native_dxf_annotation_source_dependencies(source, authored, work_budget)) (void)actual(authored, id, role);
    if (source.type == kAnnotationEntityType) {
        validate_annotation_entity(source);
    }
    else {
        const auto decoded = decode_boundary_dimension_entity(source);
        require(decoded.supported(), "unsupported retained dimension: " + decoded.unsupported_reason);
        validate_boundary_dimension_target(*decoded.dimension, actual(authored, decoded.dimension->boundary_id));
    }
}
} // namespace sketch

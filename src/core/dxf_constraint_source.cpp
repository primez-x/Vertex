#include "sketch/dxf_constraint_source.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/dxf_phase_source.hpp"
#include "sketch/dxf_project_exchange.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Owners = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;

[[noreturn]] void refuse(const std::string& reason) {
    throw std::invalid_argument("Native DXF constraint source: " + reason);
}
void require(bool condition, const std::string& reason) { if (!condition) refuse(reason); }
void identity(const std::string& id) {
    require(!id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "invalid typed identity");
}
const std::string& reference(const Json& value) {
    require(value.is_string(), "typed reference must be a string");
    const auto& id = value.get_ref<const std::string&>(); identity(id); return id;
}
void limits(const NativeDxfWallSourceWorkBudget& budget) {
    const auto& b = budget.catalog_transfer;
    require(b.max_json_bytes <= native_dxf_phase_source_byte_limit &&
        b.max_json_nodes <= native_dxf_phase_source_node_limit &&
        b.max_validation_work <= native_dxf_phase_source_work_limit &&
        b.consumed_json_bytes <= b.max_json_bytes && b.consumed_json_nodes <= b.max_json_nodes &&
        b.consumed_validation_work <= b.max_validation_work, "invalid shared work ledger");
}
void charge(std::size_t& used, std::size_t amount, std::size_t maximum, const char* reason) {
    require(used <= maximum, reason);
    if (amount > maximum - used) { used = maximum; refuse(reason); }
    used += amount;
}
void work(NativeDxfWallSourceWorkBudget& budget, std::size_t amount) {
    auto& b = budget.catalog_transfer;
    charge(b.consumed_validation_work, amount, b.max_validation_work, "cumulative typed work limit");
}
void product(NativeDxfWallSourceWorkBudget& budget, std::size_t a, std::size_t b) {
    auto& ledger = budget.catalog_transfer;
    require(ledger.consumed_validation_work <= ledger.max_validation_work, "invalid typed work ledger");
    if (b && a > (ledger.max_validation_work - ledger.consumed_validation_work) / b) {
        ledger.consumed_validation_work = ledger.max_validation_work;
        refuse("cumulative model replay work limit");
    }
    work(budget, a * b);
}
// Traverse before codecs/copies; escaped string bytes are conservatively charged.
void raw(const Json& value, NativeDxfWallSourceWorkBudget& budget, std::size_t depth = 0) {
    work(budget, 32);
    require(depth <= 64, "raw JSON depth limit");
    auto& b = budget.catalog_transfer;
    charge(b.consumed_json_nodes, 1, b.max_json_nodes, "cumulative raw JSON node limit");
    const auto bytes = [&](std::size_t n) {
        charge(b.consumed_json_bytes, n, b.max_json_bytes, "cumulative raw JSON byte limit");
    };
    const auto text = [&](const std::string& s) {
        require(s.size() <= native_dxf_phase_source_byte_limit / 6, "raw JSON string limit");
        bytes(s.size() * 6 + 3);
    };
    if (value.is_string()) text(value.get_ref<const std::string&>());
    else if (value.is_object()) {
        bytes(2);
        for (const auto& [key, child] : value.items()) { text(key); bytes(1); raw(child, budget, depth + 1); }
    } else if (value.is_array()) {
        bytes(2);
        for (const auto& child : value) { bytes(1); raw(child, budget, depth + 1); }
    } else if (value.is_number()) {
        require(!value.is_number_float() || std::isfinite(value.get<double>()), "nonfinite raw number");
        bytes(32);
    } else { require(value.is_boolean() || value.is_null(), "nonportable raw value"); bytes(5); }
}
void raw_entity(const Entity& owner, NativeDxfWallSourceWorkBudget& budget) {
    require(owner.properties.is_object() && owner.extensions.is_object(), "invalid Entity envelope");
    require(!owner.type.empty() && owner.type.size() <= 255, "invalid Entity type");
    // Ambient document owners can have the wider Document identity alphabet;
    // the constraint codec separately checks every binding/local identity.
    require(!owner.id.empty() && owner.id.size() <= 255, "invalid Entity identity");
    raw(Json(owner.id), budget); raw(Json(owner.type), budget);
    raw(owner.properties, budget); raw(owner.extensions, budget); raw(Json(owner.required), budget);
}
void codec_tree_work(const Json& value, NativeDxfWallSourceWorkBudget& budget, std::size_t depth = 0) {
    work(budget, 32);
    require(depth <= 64, "codec JSON depth limit");
    if (value.is_structured()) for (const auto& child : value) codec_tree_work(child, budget, depth + 1);
}
bool same_raw(const Json& a, const Json& b) {
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_array()) {
        for (std::size_t i = 0; i < a.size(); ++i) if (!same_raw(a.at(i), b.at(i))) return false;
        return true;
    }
    if (a.is_object()) {
        for (const auto& [key, value] : a.items()) {
            const auto found = b.find(key);
            if (found == b.end() || !same_raw(value, *found)) return false;
        }
        return true;
    }
    if (a.is_number_float() && a.get<double>() == 0 && b.get<double>() == 0)
        return std::signbit(a.get<double>()) == std::signbit(b.get<double>());
    return a == b;
}
const Json& bindings(const Entity& source) {
    require(source.type == "constraint", "constraint entity required");
    const auto& rows = source.properties.at("bindings");
    require(rows.is_array() && !rows.empty() && rows.size() <= 256, "binding inventory limit");
    return rows;
}
const char* inventory(const Entity& source) {
    const auto& version = source.properties.at("version");
    return version == 1 ? "wall_ids" : version == 2 || source.properties.contains("entity_ids")
        ? "entity_ids" : "wall_ids";
}
PersistentConstraint supported(const Entity& source) {
    const auto decoded = decode_constraint_entity(source);
    require(decoded.supported(), "unsupported relation/version: " + decoded.unsupported_reason);
    return *decoded.constraint;
}
NativeDxfConstraintOwnerMap references(const Entity& source) {
    NativeDxfConstraintOwnerMap result;
    const auto add = [&](const std::string& id, const char* type) {
        const auto [found, inserted] = result.emplace(id, type);
        require(inserted || !*type || found->second.empty() || found->second == type, "owner role conflict");
        if (*type) found->second = type;
    };
    for (const auto& row : bindings(source)) {
        require(row.is_object(), "binding must be an object");
        const auto& feature = reference(row.at("feature"));
        require(feature == "baseline" || feature == "boundary_segment", "unsupported endpoint feature");
        add(reference(row.at("owner_id")), feature == "baseline" ? "wall" : "");
    }
    // Both typed slots are Document references when present, even if only one
    // is the relation codec's canonical inventory. Keep the other's raw order.
    for (const auto* slot : {"wall_ids", "entity_ids"}) if (source.properties.contains(slot)) {
        const auto& rows = source.properties.at(slot);
        require(rows.is_array() && rows.size() <= native_dxf_phase_source_owner_limit, "owner inventory limit");
        for (const auto& row : rows) add(reference(row), std::string_view(slot) == "wall_ids" ? "wall" : "");
    }
    return result;
}
void reserve_constraint(const Entity& source, NativeDxfWallSourceWorkBudget& budget) {
    codec_tree_work(source.properties, budget); codec_tree_work(source.extensions, budget);
    const auto count = bindings(source).size();
    // Envelope/quantity codecs, ordered endpoint checks, map/string comparisons
    // and source+mapped codec passes; supported chains contain at most 128 arcs.
    product(budget, count + 1, (count + 1) * 256);
    product(budget, count + 1, 8192);
    for (const auto* slot : {"wall_ids", "entity_ids"}) if (source.properties.contains(slot)) {
        const auto& rows = source.properties.at(slot);
        require(rows.is_array() && rows.size() <= native_dxf_phase_source_owner_limit, "owner inventory limit");
        product(budget, rows.size(), 2048);
    }
    (void)references(source);
}
const Entity& actual(const Owners& owners, const std::string& id) {
    const auto found = owners.find(id);
    require(found != owners.end() && found->second.id == id, "missing/inconsistent actual owner " + id);
    return found->second;
}
void reserve_endpoint_owner(const Entity& owner, NativeDxfWallSourceWorkBudget& budget) {
    codec_tree_work(owner.properties, budget); codec_tree_work(owner.extensions, budget);
    if (owner.type == "wall") {
        work(budget, 4096); // Physical endpoint/curve/quantity/layer validation.
        return;
    }
    require(can_recognize_boundary_entity_type(owner.type) || owner.type == "measurement_linework",
        "unsupported endpoint owner type " + owner.type);
    const auto& rows = owner.type == "measurement_linework"
        ? owner.properties.at("model").at("segments") : owner.properties.at("segments");
    require(rows.is_array() && !rows.empty() && rows.size() <= 16'384, "endpoint topology limit");
    const auto edges = rows.size();
    std::size_t passes = 1;
    if (owner.type == "measurement_linework") {
        require(edges <= 512, "measured endpoint topology limit");
        const auto& model = owner.properties.at("model");
        for (const auto* key : {"transforms", "operations"}) if (model.contains(key)) {
            const auto& operations = model.at(key);
            require(operations.is_array(), "measured history must be an array");
            for (const auto& operation : operations) {
                work(budget, 32);
                std::size_t count = 1;
                if (operation.is_object() && operation.value("type", Json()) == "vertex_batch") {
                    const auto& edits = operation.at("edits");
                    require(edits.is_array() && edits.size() <= native_dxf_phase_source_node_limit, "vertex batch limit");
                    count += edits.size();
                }
                charge(passes, count, native_dxf_phase_source_work_limit, "measured replay pass limit");
            }
        }
    }
    // Boundary intersection admission and receipt/edit replay can be nonlinear;
    // reserve each actual resolution, including tangent/arc additional passes.
    product(budget, edges + 1, (edges + 1) * 32);
    product(budget, (edges + 1) * (edges + 1), passes * 32);
}
void reserve_source_bindings(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget& budget, bool include_raw) {
    reserve_constraint(source, budget);
    const auto refs = references(source);
    for (const auto& [id, type] : refs) {
        const auto& owner = actual(authored, id);
        require(type.empty() || owner.type == type, "typed owner role differs " + id);
        if (include_raw) raw_entity(owner, budget);
    }
    for (const auto& row : bindings(source)) {
        const auto& owner = actual(authored, reference(row.at("owner_id")));
        // Two integrity/structure passes and tangent/arc pair resolution can
        // decode the same owner repeatedly. Reserve all before the first codec.
        for (unsigned pass = 0; pass < 4; ++pass) reserve_endpoint_owner(owner, budget);
    }
}
void validate_structure(const Entity& source, const Owners& authored) {
    const auto& actual_source = actual(authored, source.id);
    require(&actual_source == &source || (actual_source.type == source.type && actual_source.required == source.required &&
        same_raw(actual_source.properties, source.properties) && same_raw(actual_source.extensions, source.extensions)),
        "source differs from actual authored entity");
    const auto constraint = supported(source);
    for (const auto& [id, type] : references(source)) {
        const auto& owner = actual(authored, id);
        require(type.empty() || owner.type == type, "typed owner role differs " + id);
    }
    for (const auto& binding : constraint.bindings) {
        const auto& owner = actual(authored, binding.owner_id);
        if (binding.segment_id.empty()) {
            require(owner.type == "wall", "baseline owner is not a physical wall");
        } else {
            const auto boundary = resolve_constraint_segment_owner(owner);
            const auto found = std::find_if(boundary.segments.begin(), boundary.segments.end(),
                [&](const auto& segment) { return segment.segment_id == binding.segment_id; });
            require(found != boundary.segments.end(), "actual endpoint segment missing");
            require((binding.role == WallEndpointRole::start ? found->start_vertex_id : found->end_vertex_id)
                == binding.vertex_id, "actual endpoint vertex differs");
        }
    }
    if (constraint.relation == ConstraintRelationKind::tangent)
        (void)resolve_constraint_tangent_segments(constraint, authored);
    if (constraint.relation == ConstraintRelationKind::fixed_arc_length)
        for (std::size_t i = 0; i < constraint.bindings.size(); i += 2) {
            auto pair = constraint;
            pair.bindings = {constraint.bindings.at(i), constraint.bindings.at(i + 1)};
            // Retained inactive chains still prove each real curved segment.
            // Cross-pair continuity/satisfaction is an active residual.
            (void)resolve_constraint_arc_segment(pair, authored);
        }
}
} // namespace

bool native_dxf_constraint_source_type(std::string_view type) noexcept { return type == "constraint"; }

NativeDxfConstraintOwnerMap native_dxf_constraint_source_dependencies(const Entity& source,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        limits(budget); raw_entity(source, budget); reserve_constraint(source, budget);
        (void)supported(source); return references(source);
    } catch (const Json::exception& error) { refuse(std::string("malformed dependencies: ") + error.what()); }
}

Entity remap_native_dxf_constraint_source_dependencies(const Entity& source,
    const NativeDxfConstraintOwnerMap& owner_ids, const NativeDxfConstraintElementMap& segment_ids,
    const NativeDxfConstraintElementMap& vertex_ids, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        limits(budget); raw_entity(source, budget); reserve_constraint(source, budget);
        require(owner_ids.size() <= native_dxf_phase_source_owner_limit &&
            segment_ids.size() <= native_dxf_phase_source_node_limit &&
            vertex_ids.size() <= native_dxf_phase_source_node_limit, "mapping inventory limit");
        const auto constraint = supported(source); const auto refs = references(source);
        Ids targets;
        for (const auto& [id, type] : refs) {
            (void)type; const auto mapped = owner_ids.find(id);
            require(mapped != owner_ids.end(), "missing owner mapping " + id); identity(mapped->second);
            require(targets.insert(mapped->second).second, "owner mapping is not injective");
        }
        const auto elements = [&](const NativeDxfConstraintElementMap& mapping, bool segment) {
            std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> mapped;
            if (mapping.empty()) return;
            for (const auto& binding : constraint.bindings) {
                const auto& id = segment ? binding.segment_id : binding.vertex_id;
                if (id.empty()) continue;
                const auto found = mapping.find({binding.owner_id, id});
                require(found != mapping.end(), "missing owner-local element mapping"); identity(found->second);
                const auto [previous, inserted] = mapped[binding.owner_id].emplace(found->second, id);
                require(inserted || previous->second == id, "owner-local element mapping is not injective");
            }
        };
        elements(segment_ids, true); elements(vertex_ids, false);
        // Admission precedes the raw copy; no canonical encoder participates.
        Entity result = source;
        for (auto& row : result.properties.at("bindings")) {
            const auto original_owner = reference(row.at("owner_id"));
            for (const auto& [slot, mapping] : {std::pair{"segment_id", &segment_ids}, std::pair{"vertex_id", &vertex_ids}})
                if (!mapping->empty() && row.contains(slot) && row.at(slot) != "")
                    row.at(slot) = mapping->at({original_owner, reference(row.at(slot))});
            row.at("owner_id") = owner_ids.at(original_owner);
        }
        for (const auto* slot : {"wall_ids", "entity_ids"}) if (result.properties.contains(slot)) {
            auto& rows = result.properties.at(slot);
            for (auto& row : rows) row = owner_ids.at(reference(row));
            if (std::string_view(slot) == inventory(source))
                std::sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) { return reference(a) < reference(b); });
        }
        (void)supported(result);
        return result;
    } catch (const Json::exception& error) { refuse(std::string("malformed remapping: ") + error.what()); }
}

void admit_native_dxf_constraint_source_work(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget& budget) {
    try {
        limits(budget); raw_entity(source, budget);
        require(authored.size() <= native_dxf_phase_source_owner_limit, "actual graph owner limit");
        const auto& actual_source = actual(authored, source.id);
        if (&actual_source != &source) raw_entity(actual_source, budget);
        reserve_source_bindings(source, authored, budget, true);
    } catch (const Json::exception& error) { refuse(std::string("malformed work admission: ") + error.what()); }
}

void validate_native_dxf_constraint_source(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    admit_native_dxf_constraint_source_work(source, authored, budget);
    try { validate_structure(source, authored); }
    catch (const Json::exception& error) { refuse(std::string("malformed actual bindings: ") + error.what()); }
}

void validate_native_dxf_constraint_source_graph(const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        limits(budget);
        admit_native_dxf_phase_document_entities(authored, &budget);
        product(budget, authored.size() + 1, 128);
        std::size_t bindings_count = 0;
        for (const auto& [id, source] : authored) if (source.type == "constraint") {
            require(id == source.id, "constraint actual map identity differs");
            reserve_source_bindings(source, authored, budget, false);
            charge(bindings_count, bindings(source).size(), native_dxf_phase_source_node_limit, "graph binding limit");
        }
        // Existing integrity indexes all openings and may validate a wall's
        // hosted cuts for each relation. Reserve full actual-map/overlap work.
        std::size_t openings_count = 0;
        for (const auto& [id, owner] : authored) { (void)id; if (owner.type == "opening") ++openings_count; }
        product(budget, bindings_count + 1, (openings_count + 1) * (openings_count + 1) * 128);
        for (const auto& [id, source] : authored) {
            (void)id; if (source.type == "constraint") validate_structure(source, authored);
        }
        if (const auto unsupported = validate_active_phase_constraint_integrity(authored)) refuse(*unsupported);
    } catch (const Json::exception& error) { refuse(std::string("malformed actual graph: ") + error.what()); }
}
} // namespace sketch

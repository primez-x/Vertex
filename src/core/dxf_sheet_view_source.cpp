#include "sketch/dxf_sheet_view_source.hpp"

#include "sketch/boundary_dimension.hpp"
#include "sketch/dxf_architectural_source.hpp"
#include "sketch/dxf_phase_source.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <array>
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

[[noreturn]] void refuse(const std::string& reason) {
    throw std::invalid_argument("Native DXF sheet/view source: " + reason);
}
void require(bool condition, const std::string& reason) { if (!condition) refuse(reason); }
void charge(std::size_t& consumed, std::size_t amount, std::size_t maximum, const char* reason) {
    require(consumed <= maximum && amount <= maximum - consumed, reason);
    consumed += amount;
}
void work(NativeDxfWallSourceWorkBudget& budget, std::size_t amount) {
    auto& b = budget.catalog_transfer;
    charge(b.consumed_validation_work, amount, b.max_validation_work, "cumulative validation work limit");
}
void product_work(NativeDxfWallSourceWorkBudget& budget, std::size_t a, std::size_t b) {
    require(!b || a <= native_dxf_phase_source_work_limit / b, "codec work product limit");
    work(budget, a * b);
}
void raw_text(const std::string& value, NativeDxfWallSourceWorkBudget& budget) {
    require(value.size() <= native_dxf_phase_source_byte_limit / 6, "raw string limit");
    auto& b = budget.catalog_transfer;
    charge(b.consumed_json_bytes, value.size() * 6 + 3, b.max_json_bytes, "cumulative raw byte limit");
}
std::size_t raw(const Json& value, NativeDxfWallSourceWorkBudget& budget, std::size_t depth = 0) {
    require(depth <= native_dxf_phase_source_depth_limit, "raw JSON depth limit");
    auto& b = budget.catalog_transfer;
    charge(b.consumed_json_nodes, 1, b.max_json_nodes, "cumulative raw node limit");
    work(budget, 1);
    const auto bytes = [&](std::size_t amount) {
        charge(b.consumed_json_bytes, amount, b.max_json_bytes, "cumulative raw byte limit");
    };
    std::size_t nodes = 1;
    if (value.is_string()) raw_text(value.get_ref<const std::string&>(), budget);
    else if (value.is_object()) {
        bytes(2);
        for (const auto& [key, child] : value.items()) {
            raw_text(key, budget); bytes(1); nodes += raw(child, budget, depth + 1);
        }
    } else if (value.is_array()) {
        bytes(2);
        for (const auto& child : value) { bytes(1); nodes += raw(child, budget, depth + 1); }
    } else if (value.is_number()) {
        require(!value.is_number_float() || std::isfinite(value.get<double>()), "nonfinite raw number");
        bytes(32);
    } else { require(value.is_null() || value.is_boolean(), "unsupported JSON value"); bytes(5); }
    return nodes;
}
std::size_t raw_owner(const Entity& source, NativeDxfWallSourceWorkBudget& budget) {
    require(source.properties.is_object() && source.extensions.is_object(), "invalid Entity envelope");
    auto& b = budget.catalog_transfer;
    charge(b.consumed_json_nodes, 2, b.max_json_nodes, "cumulative raw node limit");
    work(budget, 2);
    raw_text(source.id, budget); raw_text(source.type, budget);
    auto nodes = 2 + raw(source.properties, budget);
    nodes += raw(source.extensions, budget);
    nodes += raw(Json(source.required), budget);
    return nodes;
}
const Json& supported_model(const Entity& source) {
    require(native_dxf_sheet_view_source_type(source.type), "unsupported companion type");
    const auto& p = source.properties;
    require(p.is_object() && source.extensions.is_object() && p.size() == 3 &&
        p.contains("schema") && p.at("schema") == "sketch.sheet_view_entity" &&
        p.contains("version") && p.at("version").is_number_integer() && p.at("version") == 1 &&
        p.contains("model") && p.at("model").is_object(), "unsupported entity schema/version/fields");
    const auto& model = p.at("model");
    require(model.contains("schema") && model.at("schema") == "sketch.sheet_view_model" &&
        model.contains("version") && model.at("version").is_number_integer() &&
        model.at("version") >= 1 && model.at("version") <= 9, "unsupported model schema/version");
    return model;
}
const Json& rows(const Json& parent, const char* slot) {
    require(parent.is_object() && parent.contains(slot) && parent.at(slot).is_array(),
        std::string("invalid model collection ") + slot);
    return parent.at(slot);
}
std::size_t text_size(const Json& value) {
    require(value.is_string(), "invalid model identity type");
    return value.get_ref<const std::string&>().size();
}
// Shape validation searches canonical keyed arrays by identity. Reserve that
// actual quadratic work, including comparison lengths, rather than squaring
// the entire raw payload (which also contains opaque extensions and prose).
void keyed_work(const Json& values, const char* key, NativeDxfWallSourceWorkBudget& budget,
    std::size_t replays) {
    std::size_t text = 0;
    for (const auto& row : values) {
        require(row.is_object() && row.contains(key), "missing model row identity");
        charge(text, text_size(row.at(key)) + 1, native_dxf_phase_source_work_limit, "identity comparison work limit");
    }
    product_work(budget, values.size(), text * replays);
}
std::size_t search_steps(std::size_t count) {
    std::size_t steps = 1;
    while (count > 1) { count = (count + 1) / 2; ++steps; }
    return steps;
}
void scalar_id_work(const Json& values, NativeDxfWallSourceWorkBudget& budget, std::size_t replays) {
    std::size_t longest = 0;
    for (const auto& value : values) longest = std::max(longest, text_size(value) + 1);
    // Plain object/schedule ID arrays use ordered sets and sorting, rather
    // than shape's keyed-row linear searches. Preserve that logarithmic bound.
    product_work(budget, values.size(), longest * search_steps(values.size()) * 4 * replays);
}
void reserve_codec(const Json& model, NativeDxfWallSourceWorkBudget& budget, std::size_t replays) {
    const auto& views = rows(model, "views");
    const auto& sheets = rows(model, "sheets");
    keyed_work(views, "id", budget, replays);
    keyed_work(sheets, "id", budget, replays);
    const auto& schedules = rows(model, "schedule_ids");
    scalar_id_work(schedules, budget, replays);
    std::size_t schedule_text = 0;
    for (const auto& id : schedules)
        schedule_text = std::max(schedule_text, text_size(id) + 1);
    std::size_t longest_view_text = 0, sheet_text = 0, viewport_text = 0, callout_count = 0;
    for (const auto& view : views) {
        longest_view_text = std::max(longest_view_text, text_size(view.at("id")) + 1);
        std::size_t objects = 0, object_text = 0;
        if (view.contains("object_ids")) {
            const auto& ids = rows(view, "object_ids");
            scalar_id_work(ids, budget, replays); objects = ids.size();
            for (const auto& id : ids)
                charge(object_text, text_size(id) + 1, native_dxf_phase_source_work_limit, "object identity work limit");
        }
        require(view.contains("presentation") && view.at("presentation").is_object(), "missing view presentation");
        const auto& presentation = view.at("presentation");
        if (presentation.contains("appearance")) {
            const auto& appearance = presentation.at("appearance");
            keyed_work(rows(appearance, "objects"), "object_id", budget, replays);
        }
        if (view.contains("overlays")) {
            const auto& overlays = rows(view, "overlays");
            keyed_work(overlays, "id", budget, replays);
            // Each overlay may check both its explicit object and its bound
            // dimension against the owning view's complete restricted list.
            product_work(budget, overlays.size(), (objects + object_text) * 2 * replays);
        }
    }
    for (const auto& sheet : sheets) {
        charge(sheet_text, text_size(sheet.at("id")) + 1, native_dxf_phase_source_work_limit, "sheet identity work limit");
        for (const auto* slot : {"revisions", "viewports", "callouts", "schedules"})
            keyed_work(rows(sheet, slot), "id", budget, replays);
        std::size_t local_viewport_text = 0;
        for (const auto& viewport : sheet.at("viewports"))
            charge(local_viewport_text, text_size(viewport.at("id")) + 1,
                native_dxf_phase_source_work_limit, "viewport identity work limit");
        viewport_text = std::max(viewport_text, local_viewport_text);
        charge(callout_count, sheet.at("callouts").size(), native_dxf_phase_source_node_limit, "callout count limit");
        product_work(budget, sheet.at("viewports").size(), longest_view_text * search_steps(views.size()) * replays);
        product_work(budget, sheet.at("schedules").size(), schedule_text * search_steps(schedules.size()) * replays);
    }
    // Cross-sheet callouts linearly locate a sheet and its target viewport.
    product_work(budget, callout_count, (sheet_text + viewport_text) * replays);
}
std::string global_id(const Json& value) {
    require(value.is_string(), "typed owner reference must be a string");
    const auto& id = value.get_ref<const std::string&>();
    require(!id.empty() && id.size() <= 255 &&
        std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; }) &&
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }),
        "invalid document owner reference");
    return id;
}
const Entity& actual(const Owners& authored, const std::string& id) {
    const auto found = authored.find(id);
    require(found != authored.end() && found->second.id == id, "missing or mismatched actual owner " + id);
    return found->second;
}
enum class ReferenceKind { owner, dimension_host, optional_witness };
template<class Model, class Visit> void global_slots(Model& model, Visit visit) {
    for (auto& view : model.at("views")) {
        if (view.contains("object_ids")) for (auto& id : view.at("object_ids")) visit(id, ReferenceKind::owner);
        auto& presentation = view.at("presentation");
        if (presentation.contains("appearance"))
            for (auto& object : presentation.at("appearance").at("objects")) visit(object.at("object_id"), ReferenceKind::owner);
        if (view.contains("overlays")) for (auto& overlay : view.at("overlays")) {
            if (!overlay.at("object_id").template get_ref<const std::string&>().empty())
                visit(overlay.at("object_id"), ReferenceKind::optional_witness);
            if (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null())
                visit(overlay.at("dimension_binding").at("object_id"), ReferenceKind::dimension_host);
        }
    }
}
void validate_globals(const Entity& source, const Owners& authored, NativeDxfWallSourceWorkBudget& budget) {
    require(authored.size() <= native_dxf_phase_source_owner_limit, "actual owner inventory limit");
    require(actual(authored, global_id(Json(source.id))) == source, "companion differs from actual source inventory");
    global_slots(source.properties.at("model"), [&](const Json& value, ReferenceKind kind) {
        // Detached overlays retain coordinates and may keep an unresolved
        // source witness. It is not an associative dimension's required host.
        if (kind == ReferenceKind::optional_witness &&
            !authored.contains(value.get_ref<const std::string&>())) return;
        const auto& target = actual(authored, global_id(value));
        if (kind == ReferenceKind::dimension_host) {
            // Keep in step with Document's sheet/view object-reference contract.
            static constexpr std::array<std::string_view, 12> types{
                "wall", "opening", "room", "slab", "roof", "stair", "railing",
                "column", "beam", "wall_join", "roof_join", "assembly_instance"};
            require(std::find(types.begin(), types.end(), target.type) != types.end() || target.type == "corner_window",
                "associative dimension references unsupported actual object " + target.id);
        }
    });
    // The codec checks the aligned-axis/leg pairing. Complete actual-map
    // resolution proves that the saved leg names real hosts and owned cuts;
    // retained admission does not demand a currently active phase cohort.
    for (const auto& view : source.properties.at("model").at("views")) {
        if (!view.contains("overlays")) continue;
        for (const auto& overlay : view.at("overlays")) {
            if (!overlay.contains("dimension_binding") || overlay.at("dimension_binding").is_null()) continue;
            const auto& binding = overlay.at("dimension_binding");
            const auto& target = actual(authored, global_id(binding.at("object_id")));
            if (!binding.contains("corner_leg")) {
                // Original three-field horizontal/vertical bindings measure
                // the complete corner silhouette, without selecting a leg.
                if (target.type == "corner_window") {
                    admit_native_dxf_corner_window_source_work(target, authored, budget);
                    validate_native_dxf_corner_window_source(target, authored);
                }
                continue;
            }
            require(target.type == "corner_window", "corner leg binding requires an actual corner-window owner");
            BoundaryDimension dimension;
            dimension.id = target.id;
            dimension.boundary_id = target.id;
            dimension.kind = BoundaryDimensionKind::corner_window_leg_length;
            dimension.corner_leg = binding.at("corner_leg").get<std::uint32_t>();
            admit_native_dxf_corner_window_source_work(target, authored, budget);
            (void)resolve_dimension_corner_window_leg_owner(dimension, authored);
        }
    }
}
std::vector<std::string> view_ids(const SheetViewModel& model) {
    std::vector<std::string> result;
    result.reserve(model.views().size());
    for (const auto& view : model.views()) result.push_back(view.id);
    return result; // The authoritative model already validated and sorted IDs.
}
void lookup_work(const std::string& id, std::size_t count, NativeDxfWallSourceWorkBudget& budget) {
    product_work(budget, id.size() + 1, search_steps(count));
}
std::vector<std::string> unresolved_witness_ids(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget& budget) {
    std::set<std::string, std::less<>> identities;
    global_slots(source.properties.at("model"), [&](const Json& value, ReferenceKind kind) {
        if (kind != ReferenceKind::optional_witness) return;
        const auto& id = value.get_ref<const std::string&>();
        lookup_work(id, authored.size(), budget);
        if (authored.contains(id)) return;
        // Original missing names are raw witnesses, not document owner IDs.
        // Charge comparison and potential string/node allocation before insert.
        raw_text(id, budget);
        product_work(budget, id.size() + 1, search_steps(identities.size()) * 2);
        identities.insert(id);
    });
    work(budget, identities.size());
    std::vector<std::string> result;
    result.reserve(identities.size());
    for (const auto& id : identities) {
        raw_text(id, budget);
        work(budget, id.size() + 1);
        result.push_back(id);
    }
    return result;
}
const References* unresolved_witness_scope(const Entity& source,
    const std::vector<std::string>& identities, const NativeDxfSheetViewSourceMaps& mapping,
    const Owners& authored, const References* owner_mapping, NativeDxfWallSourceWorkBudget& budget) {
    lookup_work(source.id, mapping.size(), budget);
    const auto scope = mapping.find(source.id);
    if (scope == mapping.end()) return nullptr;
    require(scope->second.size() == identities.size(), "unresolved witness mapping is incomplete");
    // Reference existing owner-map strings; never invent owners or copy the
    // complete owner inventory just to reserve their names.
    std::set<std::string_view, std::less<>> mapped_owners;
    if (owner_mapping) {
        require(owner_mapping->size() <= native_dxf_phase_source_owner_limit, "mapped owner inventory limit");
        work(budget, owner_mapping->size());
        for (const auto& [original, destination] : *owner_mapping) {
            raw_text(original, budget);
            raw_text(destination, budget);
            product_work(budget, destination.size() + 1, search_steps(mapped_owners.size()) * 2);
            mapped_owners.insert(destination);
        }
    }
    std::set<std::string_view, std::less<>> destinations;
    work(budget, scope->second.size());
    for (const auto& [original, destination] : scope->second) {
        // Bill every supplied scoped string before scans or comparison work,
        // including a malformed extra key in an otherwise equal-sized scope.
        raw_text(original, budget);
        raw_text(destination, budget);
        work(budget, destination.size() + 1);
        require(!destination.empty() && !std::all_of(destination.begin(), destination.end(),
            [](unsigned char c) { return std::isspace(c); }), "blank mapped unresolved witness identity");
        lookup_work(original, identities.size(), budget);
        require(std::binary_search(identities.begin(), identities.end(), original),
            "unresolved witness mapping has an unknown source identity");
        lookup_work(destination, authored.size(), budget);
        require(!authored.contains(destination), "mapped unresolved witness names an actual source owner");
        lookup_work(destination, mapped_owners.size(), budget);
        require(!mapped_owners.contains(std::string_view(destination)),
            "mapped unresolved witness names a mapped owner");
        product_work(budget, destination.size() + 1, search_steps(destinations.size()) * 2);
        require(destinations.insert(destination).second, "noninjective unresolved witness mapping");
    }
    return &scope->second;
}
} // namespace

bool native_dxf_sheet_view_source_type(std::string_view type) noexcept { return type == kSheetViewEntityType; }

void admit_native_dxf_sheet_view_source_work(const Entity& source,
    NativeDxfWallSourceWorkBudget& budget, std::size_t codec_replays) {
    require(codec_replays > 0 && codec_replays <= 8, "invalid codec replay reservation");
    const auto& b = budget.catalog_transfer;
    require(b.max_json_bytes <= native_dxf_phase_source_byte_limit && b.max_json_nodes <= native_dxf_phase_source_node_limit &&
        b.max_validation_work <= native_dxf_phase_source_work_limit && b.consumed_json_bytes <= b.max_json_bytes &&
        b.consumed_json_nodes <= b.max_json_nodes && b.consumed_validation_work <= b.max_validation_work,
        "invalid shared work ledger");
    const auto nodes = raw_owner(source, budget);
    product_work(budget, nodes, 32 * codec_replays);
    reserve_codec(supported_model(source), budget, codec_replays);
}

std::vector<std::string> native_dxf_sheet_view_source_view_identity_ids(const Entity& source,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local;
    auto& budget = work_budget ? *work_budget : local;
    admit_native_dxf_sheet_view_source_work(source, budget);
    return view_ids(decode_sheet_view_entity(source));
}

References native_dxf_sheet_view_source_dependencies(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local;
    auto& budget = work_budget ? *work_budget : local;
    admit_native_dxf_sheet_view_source_work(source, budget);
    (void)decode_sheet_view_entity(source);
    validate_globals(source, authored, budget);
    References result;
    global_slots(source.properties.at("model"), [&](const Json& value, ReferenceKind kind) {
        if (kind == ReferenceKind::optional_witness &&
            !authored.contains(value.get_ref<const std::string&>())) return;
        result.emplace(global_id(value), "");
    });
    return result;
}

std::vector<std::string> native_dxf_sheet_view_source_unresolved_witness_ids(const Entity& source,
    const Owners& authored, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local;
    auto& budget = work_budget ? *work_budget : local;
    admit_native_dxf_sheet_view_source_work(source, budget);
    (void)decode_sheet_view_entity(source);
    validate_globals(source, authored, budget);
    return unresolved_witness_ids(source, authored, budget);
}

void validate_native_dxf_sheet_view_source(const Entity& source, const Owners& authored,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local;
    auto& budget = work_budget ? *work_budget : local;
    admit_native_dxf_sheet_view_source_work(source, budget);
    (void)decode_sheet_view_entity(source);
    validate_globals(source, authored, budget);
}

void validate_native_dxf_sheet_view_witness_binding(const Entity& source,
    const Owners& original_owners, const Owners& candidate_owners,
    NativeDxfWallSourceWorkBudget* work_budget,
    const NativeDxfSheetViewSourceMaps& unresolved_witness_mapping) {
    NativeDxfWallSourceWorkBudget local;
    auto& budget = work_budget ? *work_budget : local;
    admit_native_dxf_sheet_view_source_work(source, budget);
    (void)decode_sheet_view_entity(source);
    validate_globals(source, original_owners, budget);
    require(candidate_owners.size() <= native_dxf_phase_source_owner_limit, "candidate owner inventory limit");
    const auto* scope = unresolved_witness_mapping.empty() ? nullptr : unresolved_witness_scope(source,
        unresolved_witness_ids(source, original_owners, budget), unresolved_witness_mapping,
        original_owners, nullptr, budget);
    global_slots(source.properties.at("model"), [&](const Json& value, ReferenceKind kind) {
        if (kind != ReferenceKind::optional_witness) return;
        const auto& id = value.get_ref<const std::string&>();
        lookup_work(id, original_owners.size(), budget);
        if (original_owners.contains(id)) return;
        if (scope) lookup_work(id, scope->size(), budget);
        const auto& destination = scope ? scope->at(id) : id;
        lookup_work(destination, candidate_owners.size(), budget);
        require(!candidate_owners.contains(destination),
            "unresolved overlay witness would attach to an unrelated owner " + destination);
    });
}

void remap_native_dxf_sheet_view_source_dependencies(Entity& source, const Owners& authored,
    const References& owner_mapping, const References& context_mapping,
    const NativeDxfSheetViewSourceMaps& view_mapping, NativeDxfWallSourceWorkBudget* work_budget,
    const NativeDxfSheetViewSourceMaps& unresolved_witness_mapping) {
    (void)context_mapping; // The strict three-field sheet/view envelope owns none.
    NativeDxfWallSourceWorkBudget local;
    auto& budget = work_budget ? *work_budget : local;
    admit_native_dxf_sheet_view_source_work(source, budget);
    const auto identities = view_ids(decode_sheet_view_entity(source));
    validate_globals(source, authored, budget);
    const auto* witness_scope = unresolved_witness_mapping.empty() ? nullptr : unresolved_witness_scope(source,
        unresolved_witness_ids(source, authored, budget), unresolved_witness_mapping,
        authored, &owner_mapping, budget);
    const auto scope = view_mapping.find(source.id);
    if (scope != view_mapping.end()) {
        require(scope->second.size() == identities.size(), "view mapping is incomplete");
        std::size_t destination_text = 0;
        for (const auto& id : identities) {
            const auto found = scope->second.find(id);
            require(found != scope->second.end(), "view mapping is incomplete");
            const auto& destination = found->second;
            raw_text(destination, budget);
            work(budget, destination.size() + 1);
            charge(destination_text, destination.size() + 1, native_dxf_phase_source_work_limit,
                "mapped identity comparison work limit");
            require(!destination.empty() && !std::all_of(destination.begin(), destination.end(),
                [](unsigned char c) { return std::isspace(c); }), "blank mapped view identity");
        }
        product_work(budget, identities.size(), destination_text);
        std::set<std::string, std::less<>> destinations;
        for (const auto& id : identities)
            require(destinations.insert(scope->second.at(id)).second, "noninjective view mapping");
    }
    // Charge the detached raw copy before allocating it. The final codec pass
    // gets its own reservation against the actual remapped identity lengths.
    product_work(budget, raw_owner(source, budget), 8);
    auto result = source;
    auto& model = result.properties.at("model");
    global_slots(model, [&](Json& value, ReferenceKind kind) {
        if (kind == ReferenceKind::optional_witness) {
            const auto& id = value.get_ref<const std::string&>();
            lookup_work(id, authored.size(), budget);
            if (!authored.contains(id)) {
                if (witness_scope) {
                    lookup_work(id, witness_scope->size(), budget);
                    const auto& destination = witness_scope->at(id);
                    raw_text(destination, budget);
                    work(budget, destination.size() + 1);
                    value = destination;
                }
                return;
            }
        }
        const auto id = global_id(value);
        const auto found = owner_mapping.find(id);
        require(found != owner_mapping.end(), "typed reference missing from destination map " + id);
        require(found->second.size() <= 255, "mapped document owner reference limit");
        raw_text(found->second, budget);
        value = global_id(Json(found->second));
    });
    if (scope != view_mapping.end()) {
        const auto patch_view = [&](Json& value) {
            const auto& destination = scope->second.at(value.get_ref<const std::string&>());
            // A single local view may be repeated by many viewports. Bill
            // every replacement before allocating its potentially long text.
            raw_text(destination, budget);
            work(budget, destination.size() + 1);
            value = destination;
        };
        for (auto& view : model.at("views")) patch_view(view.at("id"));
        for (auto& sheet : model.at("sheets")) for (auto& viewport : sheet.at("viewports"))
            patch_view(viewport.at("view_id"));
    }
    admit_native_dxf_sheet_view_source_work(result, budget);
    (void)decode_sheet_view_entity(result);
    source = std::move(result);
}
} // namespace sketch

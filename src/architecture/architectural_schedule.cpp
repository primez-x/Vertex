#include "sketch/architectural_schedule.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"

#include <Standard_Failure.hxx>
#include <TopoDS_Iterator.hxx>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <map>
#include <limits>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {
std::string roof_join_material_schedule_row_id(std::string_view join_id,
                                              std::string_view source_roof_id) {
    return "roof-join:" + std::to_string(join_id.size()) + ":" + std::string(join_id) +
        ":member:" + std::to_string(source_roof_id.size()) + ":" +
        std::string(source_roof_id) + ":material";
}

namespace {

constexpr std::string_view material_suffix = ":material";
constexpr std::string_view layer_marker = ":layer:";

struct MaterialGroup {
    std::string key;
    std::string display_name;
    std::int64_t count{};
    double volume{};
    bool complete_volume{true};
    std::vector<ScheduleSourceRef> sources;
};

std::string normalized_material_name(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    bool pending_space = false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            if (!result.empty()) pending_space = true;
            continue;
        }
        if (pending_space) result.push_back(' ');
        result.push_back(static_cast<char>(std::tolower(byte)));
        pending_space = false;
    }
    return result;
}

std::string material_group_digest(std::string_view key) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(key.data()), key.size());
    return sha256_hex(bytes);
}

void append_sources(std::vector<ScheduleSourceRef>& destination,
                    const std::vector<ScheduleSourceRef>& sources) {
    destination.insert(destination.end(), sources.begin(), sources.end());
}

void normalize_sources(std::vector<ScheduleSourceRef>& sources) {
    std::sort(sources.begin(), sources.end(), [](const auto& left, const auto& right) {
        return std::pair{left.object_id, left.property} <
               std::pair{right.object_id, right.property};
    });
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
}

std::string material_source_id(std::string_view object_id,
                               const DocumentSnapshot* document = nullptr) {
    if (object_id.size() <= material_suffix.size() ||
        !object_id.ends_with(material_suffix)) {
        return {};
    }
    const auto stem = object_id.substr(0, object_id.size() - material_suffix.size());
    // An authored ID may itself contain a synthetic-looking layer marker.
    // Resolve the complete entity identity before interpreting derived paths.
    if (document && document->entities().contains(std::string(stem))) return std::string(stem);
    const auto marker = stem.find(layer_marker);
    return marker == std::string_view::npos ? std::string(stem) :
                                                std::string(stem.substr(0, marker));
}

bool is_layer_material_row(const ScheduleRow& row) {
    return row.cells.contains("layer_id") && row.kind == ScheduleRowKind::material;
}

bool is_assembly_material_row(const ScheduleRow& row) {
    // Assembly material rows are derived from an immutable catalog instance;
    // their synthetic IDs therefore do not resolve to a Document entity.
    // Keep the explicit cells as the discriminator rather than parsing IDs.
    return row.kind == ScheduleRowKind::material &&
           row.cells.contains("instance_id") &&
           row.cells.contains("catalog_id") &&
           row.cells.contains("material_id");
}

void append_material_summaries(const DocumentSnapshot& document,
                               DocumentScheduleProjection& projection) {
    std::map<std::string, MaterialGroup, std::less<>> groups;
    for (const auto& row : projection.snapshot.rows) {
        if (row.kind != ScheduleRowKind::material ||
            row.object_id.size() <= material_suffix.size() ||
            !row.object_id.ends_with(material_suffix)) {
            continue;
        }
        const auto basis = row.cells.find("takeoff_basis");
        if (basis != row.cells.end() &&
            std::holds_alternative<std::string>(basis->second.value) &&
            std::get<std::string>(basis->second.value) == "source_gross") continue;
        const auto assembly_material = is_assembly_material_row(row) ||
                                       row.cells.contains("joined_roof_id");
        const auto source_id = assembly_material ? std::string{} :
            material_source_id(row.object_id, &document);
        const auto source = document.entities().find(source_id);
        const auto name_cell = row.cells.find("name");
        if ((source == document.entities().end() && !assembly_material) ||
            name_cell == row.cells.end() ||
            !std::holds_alternative<std::string>(name_cell->second.value)) {
            projection.diagnostics.push_back(
                source_id + ": material summary source is incomplete");
            continue;
        }
        const auto display_name = std::get<std::string>(name_cell->second.value);
        std::string group_key;
        const nlohmann::json* assignment = nullptr;
        if (!is_layer_material_row(row) && !assembly_material &&
            source != document.entities().end()) {
            const auto found = source->second.properties.find("material_assignment");
            if (found != source->second.properties.end() && found->is_object()) {
                assignment = &*found;
            }
        }
        if (assignment != nullptr) {
            const auto catalog = assignment->find("catalog_id");
            const auto material = assignment->find("material_id");
            if (catalog != assignment->end() && material != assignment->end() &&
                catalog->is_string() && material->is_string() &&
                !catalog->get<std::string>().empty() && !material->get<std::string>().empty()) {
                group_key = "assigned\n" + catalog->get<std::string>() + "\n" +
                            material->get<std::string>();
            }
        }
        if (group_key.empty()) {
            const auto catalog = row.cells.find("catalog_id");
            const auto material = row.cells.find("material_id");
            if (catalog != row.cells.end() && material != row.cells.end() &&
                std::holds_alternative<std::string>(catalog->second.value) &&
                std::holds_alternative<std::string>(material->second.value)) {
                const auto& catalog_id = std::get<std::string>(catalog->second.value);
                const auto& material_id = std::get<std::string>(material->second.value);
                if (!catalog_id.empty() && !material_id.empty()) {
                    group_key = "assigned\n" + catalog_id + "\n" + material_id;
                } else if (!catalog_id.empty() && is_assembly_material_row(row)) {
                    group_key = "unassigned-assembly\n" + catalog_id;
                }
            }
        }
        if (group_key.empty()) {
            const auto normalized = normalized_material_name(display_name);
            if (normalized.empty()) continue;
            group_key = "explicit\n" + normalized;
        }
        auto [group, inserted] = groups.try_emplace(
            group_key, MaterialGroup{group_key, display_name});
        if (!inserted && group->second.display_name.empty() && !display_name.empty())
            group->second.display_name = display_name;
        auto& aggregate = group->second;
        const auto count = row.cells.find("count");
        if (count != row.cells.end() && std::holds_alternative<std::int64_t>(count->second.value))
            aggregate.count += std::get<std::int64_t>(count->second.value);
        else
            ++aggregate.count;
        append_sources(aggregate.sources, name_cell->second.sources);
        if (count != row.cells.end()) append_sources(aggregate.sources, count->second.sources);

        const auto volume = row.cells.find("volume");
        if (volume == row.cells.end()) {
            aggregate.complete_volume = false;
            continue;
        }
        if (!std::holds_alternative<ScheduleQuantity>(volume->second.value) ||
            std::get<ScheduleQuantity>(volume->second.value).unit != ScheduleUnit::cubic_metre ||
            !std::isfinite(std::get<ScheduleQuantity>(volume->second.value).value) ||
            std::get<ScheduleQuantity>(volume->second.value).value < 0.0) {
            aggregate.complete_volume = false;
            continue;
        }
        aggregate.volume += std::get<ScheduleQuantity>(volume->second.value).value;
        append_sources(aggregate.sources, volume->second.sources);
    }

    for (auto& [key, aggregate] : groups) {
        normalize_sources(aggregate.sources);
        const auto digest = material_group_digest(key);
        ScheduleRow summary;
        summary.object_id = "material-summary:" + digest;
        summary.mark = "MS-" + digest;
        summary.kind = ScheduleRowKind::material_summary;
        const auto source_count = aggregate.count;
        const auto source_label = source_count == 1 ? "source contribution" : "source contributions";
        summary.cells.emplace("mark", ScheduleCell{
            summary.mark, false, aggregate.sources,
            "Stable mark for this grouped material summary"});
        summary.cells.emplace("name", ScheduleCell{
            aggregate.display_name, false, aggregate.sources,
            "Grouped material identity/name across " + std::to_string(source_count) +
                " " + source_label});
        summary.cells.emplace("count", ScheduleCell{
            source_count, false, aggregate.sources,
            "Count of source material contributions (objects, layers or assembly profiles)"});
        if (aggregate.complete_volume) {
            summary.cells.emplace("volume", ScheduleCell{
                ScheduleQuantity{aggregate.volume, ScheduleUnit::cubic_metre}, false,
                aggregate.sources,
                "Net quantity sums contributing solids and disjoint joined regions; gross joined sources are excluded; no waste allowance"});
        } else {
            projection.diagnostics.push_back(
                summary.object_id + ": grouped material volume is incomplete; "
                "every source must have a valid net volume");
        }
        projection.snapshot.rows.push_back(std::move(summary));
    }
}

void append_layer_material_rows(
    const DocumentSnapshot& document,
    const std::map<std::string, std::vector<const Entity*>, std::less<>>& openings,
    DocumentScheduleProjection& projection,
    const std::set<std::string, std::less<>>* visible_entity_ids) {
    static const std::vector<const Entity*> no_openings;
    std::map<std::string, AssemblyModel> catalogs;
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "wall" ||
            (visible_entity_ids && !visible_entity_ids->contains(id)) ||
            !entity.properties.contains("layers")) {
            continue;
        }
        try {
            Wall wall;
            std::string error;
            const auto& wall_openings = openings.contains(id) ? openings.at(id) : no_openings;
            if (!read_document_wall(entity, wall_openings, wall, error)) {
                throw std::invalid_argument(error);
            }
            if (wall.layers.empty()) continue;
            // The wall factory emits one direct child per authored layer,
            // retaining its true baseline offset, sloped top and hosted cuts.
            // Re-centering a thinner wall changes curved-layer quantities and
            // discards the slope, so never reconstruct a material surrogate.
            const auto composite_shape = make_wall(wall);
            TopoDS_Iterator layer_shape(composite_shape);
            std::vector<double> layer_volumes;
            layer_volumes.reserve(wall.layers.size());
            for (std::size_t index = 0; index < wall.layers.size(); ++index) {
                if (!layer_shape.More())
                    throw std::invalid_argument("wall layer solid identities are incomplete");
                const auto volume = solid_volume(layer_shape.Value());
                if (!std::isfinite(volume) || volume <= 0.0)
                    throw std::invalid_argument("layer solid volume must be positive and finite");
                layer_volumes.push_back(volume);
                layer_shape.Next();
            }
            if (layer_shape.More())
                throw std::invalid_argument("wall layer solid identities do not match the authored stack");
            std::vector<ScheduleRow> layer_rows;
            layer_rows.reserve(wall.layers.size());
            std::size_t layer_index = 0;
            for (const auto& layer : wall.layers) {
                const auto volume = layer_volumes[layer_index++];
                if (!layer.material.has_value()) continue;
                const auto& assignment = *layer.material;
                if (!catalogs.contains(assignment.catalog_id)) {
                    const auto catalog = document.entities().find(assignment.catalog_id);
                    if (catalog == document.entities().end() || catalog->second.type != "assembly_model") {
                        throw std::invalid_argument("layer material catalog is missing");
                    }
                    const auto model = catalog->second.properties.find("model");
                    if (model == catalog->second.properties.end()) {
                        throw std::invalid_argument("layer material catalog has no model");
                    }
                    catalogs.emplace(assignment.catalog_id, AssemblyModel::from_json(model.value()));
                }
                const auto& materials = catalogs.at(assignment.catalog_id).materials();
                const auto material = std::find_if(materials.begin(), materials.end(),
                    [&](const auto& candidate) { return candidate.id == assignment.material_id; });
                if (material == materials.end()) {
                    throw std::invalid_argument("layer material is missing from its catalog");
                }

                ScheduleRow row;
                row.object_id = id + std::string(layer_marker) + layer.id +
                                std::string(material_suffix);
                row.mark = "M-" + id + "-" + layer.id;
                row.kind = ScheduleRowKind::material;
                const auto source_prefix = id + ":layers." + layer.id;
                row.cells.emplace("name", ScheduleCell{
                    material->name, false,
                    {{id, source_prefix + ".material_assignment"},
                     {assignment.catalog_id, "model"}},
                    "Assigned material name for this wall layer"});
                row.cells.emplace("count", ScheduleCell{
                    std::int64_t{1}, false, {{id, source_prefix}},
                    "One material layer instance"});
                row.cells.emplace("layer_id", ScheduleCell{
                    layer.id, false, {{id, "layers"}}, "Stable wall layer identity"});
                row.cells.emplace("thickness", ScheduleCell{
                    ScheduleQuantity{layer.thickness, ScheduleUnit::metre}, false,
                    {{id, source_prefix + ".thickness_m"}}, "Authored layer thickness"});
                row.cells.emplace("catalog_id", ScheduleCell{
                    assignment.catalog_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material catalog identity"});
                row.cells.emplace("material_id", ScheduleCell{
                    assignment.material_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material identity"});

                std::vector<ScheduleSourceRef> volume_sources{{id, "geometry"}, {id, "layers"}};
                for (const auto* opening : wall_openings) {
                    volume_sources.push_back({opening->id, "geometry"});
                }
                row.cells.emplace("volume", ScheduleCell{
                    ScheduleQuantity{volume, ScheduleUnit::cubic_metre}, false,
                    std::move(volume_sources),
                    "Actual offset layer solid volume, including sloped top and hosted openings"});
                layer_rows.push_back(std::move(row));
            }
            // A malformed later layer must not leave a partial wall takeoff.
            projection.snapshot.rows.insert(projection.snapshot.rows.end(),
                std::make_move_iterator(layer_rows.begin()), std::make_move_iterator(layer_rows.end()));
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": layer material volume unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": layer material unavailable: " + error.what());
        }
    }
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "slab" ||
            (visible_entity_ids && !visible_entity_ids->contains(id)) ||
            !entity.properties.contains("layers")) {
            continue;
        }
        try {
            Slab slab;
            std::string error;
            if (!read_document_slab(entity, slab, error)) {
                throw std::invalid_argument(error);
            }
            if (slab.layers.empty()) continue;
            double layer_elevation = slab.elevation;
            for (const auto& layer : slab.layers) {
                if (!layer.material.has_value()) {
                    layer_elevation += layer.thickness;
                    continue;
                }
                const auto& assignment = *layer.material;
                if (!catalogs.contains(assignment.catalog_id)) {
                    const auto catalog = document.entities().find(assignment.catalog_id);
                    if (catalog == document.entities().end() || catalog->second.type != "assembly_model") {
                        throw std::invalid_argument("layer material catalog is missing");
                    }
                    const auto model = catalog->second.properties.find("model");
                    if (model == catalog->second.properties.end()) {
                        throw std::invalid_argument("layer material catalog has no model");
                    }
                    catalogs.emplace(assignment.catalog_id, AssemblyModel::from_json(model.value()));
                }
                const auto& materials = catalogs.at(assignment.catalog_id).materials();
                const auto material = std::find_if(materials.begin(), materials.end(),
                    [&](const auto& candidate) { return candidate.id == assignment.material_id; });
                if (material == materials.end()) {
                    throw std::invalid_argument("layer material is missing from its catalog");
                }

                ScheduleRow row;
                row.object_id = id + std::string(layer_marker) + layer.id +
                                std::string(material_suffix);
                row.mark = "M-" + id + "-" + layer.id;
                row.kind = ScheduleRowKind::material;
                const auto source_prefix = id + ":layers." + layer.id;
                row.cells.emplace("name", ScheduleCell{
                    material->name, false,
                    {{id, source_prefix + ".material_assignment"},
                     {assignment.catalog_id, "model"}},
                    "Assigned material name for this slab layer"});
                row.cells.emplace("count", ScheduleCell{
                    std::int64_t{1}, false, {{id, source_prefix}},
                    "One material layer instance"});
                row.cells.emplace("layer_id", ScheduleCell{
                    layer.id, false, {{id, "layers"}}, "Stable slab layer identity"});
                row.cells.emplace("thickness", ScheduleCell{
                    ScheduleQuantity{layer.thickness, ScheduleUnit::metre}, false,
                    {{id, source_prefix + ".thickness_m"}}, "Authored layer thickness"});
                row.cells.emplace("catalog_id", ScheduleCell{
                    assignment.catalog_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material catalog identity"});
                row.cells.emplace("material_id", ScheduleCell{
                    assignment.material_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material identity"});

                Slab homogeneous{id, slab.boundary, slab.holes, layer.thickness,
                                layer_elevation, slab.element_kind, {}};
                const auto shape = make_slab(homogeneous);
                const auto volume = solid_volume(shape);
                if (!std::isfinite(volume) || volume <= 0.0) {
                    throw std::invalid_argument("layer solid volume must be positive and finite");
                }
                row.cells.emplace("volume", ScheduleCell{
                    ScheduleQuantity{volume, ScheduleUnit::cubic_metre}, false,
                    {{id, "geometry"}},
                    "Net slab layer solid volume after openings"});
                projection.snapshot.rows.push_back(std::move(row));
                layer_elevation += layer.thickness;
            }
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": layer material volume unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": layer material unavailable: " + error.what());
        }
    }
}

ScheduleValue assembly_quantity_value(const AssemblyQuantityProperty& quantity) {
    switch (quantity.unit) {
    case AssemblyQuantityUnit::count:
        if (!std::isfinite(quantity.value) || quantity.value < 0 ||
            std::floor(quantity.value) != quantity.value ||
            quantity.value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("assembly count exceeds schedule integer range");
        }
        return ScheduleValue{static_cast<std::int64_t>(quantity.value)};
    case AssemblyQuantityUnit::metre:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::metre}};
    case AssemblyQuantityUnit::square_metre:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::square_metre}};
    case AssemblyQuantityUnit::cubic_metre:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::cubic_metre}};
    case AssemblyQuantityUnit::kilogram:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::kilogram}};
    }
    throw std::invalid_argument("unknown assembly quantity unit");
}

std::string assembly_row_component(std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value);
}

std::string assembly_quantity_column(const AssemblyQuantityKey& key,
    const std::map<AssemblyQuantityKey, double>& quantities) {
    const auto collisions = std::count_if(quantities.begin(), quantities.end(),
        [&](const auto& item) { return item.first.name == key.name; });
    auto result = "quantity:" + key.name;
    if (collisions > 1) {
        switch (key.unit) {
        case AssemblyQuantityUnit::count: result += ":count"; break;
        case AssemblyQuantityUnit::metre: result += ":m"; break;
        case AssemblyQuantityUnit::square_metre: result += ":m2"; break;
        case AssemblyQuantityUnit::cubic_metre: result += ":m3"; break;
        case AssemblyQuantityUnit::kilogram: result += ":kg"; break;
        }
    }
    return result;
}

void append_assembly_rows(const DocumentSnapshot& document,
                          DocumentScheduleProjection& projection,
                          const std::set<std::string, std::less<>>* visible_entity_ids) {
    const auto visible = [&](const std::string& id) {
        return !visible_entity_ids || visible_entity_ids->contains(id);
    };
    std::map<std::string, AssemblyModel, std::less<>> models;
    AssemblyDocumentEntities scoped;
    std::set<std::string, std::less<>> required_catalogs;
    std::vector<ScheduleRow> rows;
    std::string context = "assembly scope";
    try {
        // Resolve aliases against the complete captured source so visibility
        // filtering cannot change an embedded root's presentation identity.
        const auto embedded_ids = embedded_assembly_presentation_ids(document.entities());
        // Supporting catalogs may be hidden. Hidden independent owners neither
        // expand nor contribute diagnostics to a visibility-scoped schedule.
        for (const auto& [id, entity] : document.entities()) {
            if (entity.type != "assembly_instance" || !visible(id)) continue;
            context = id;
            const auto value = decode_document_assembly_instance(entity);
            required_catalogs.insert(value.assembly_catalog_id);
            scoped.emplace(id, entity);
        }
        for (const auto& [catalog_id, entity] : document.entities()) {
            if (entity.type != "assembly_model") continue;
            // A catalog can contain a legacy host-visible instance even when
            // the catalog itself is hidden. Read placement identity only to
            // select relevant catalogs; strict decoding follows for any used one.
            bool host_visible = false;
            if (visible_entity_ids && entity.properties.contains("model")) {
                const auto& json = entity.properties.at("model");
                if (json.is_object() && json.contains("instances") && json.at("instances").is_array()) {
                    for (const auto& item : json.at("instances")) {
                        if (!item.is_object() || !item.contains("placement") ||
                            !item.at("placement").is_object()) continue;
                        const auto& placement = item.at("placement");
                        if (placement.contains("host_entity_id") && placement.at("host_entity_id").is_string() &&
                            visible(placement.at("host_entity_id").get<std::string>())) host_visible = true;
                    }
                }
            }
            if (!visible(catalog_id) && !host_visible && !required_catalogs.contains(catalog_id)) continue;
            context = catalog_id;
            if (!entity.properties.contains("model"))
                throw std::invalid_argument("assembly catalog has no model: " + catalog_id);
            const auto original = AssemblyModel::from_json(entity.properties.at("model"));
            auto instances = original.instances();
            std::erase_if(instances, [&](const auto& instance) {
                return instance.placement ? !visible(instance.placement->host_entity_id) : !visible(catalog_id);
            });
            const auto model = AssemblyModel::create(original.materials(), original.types(), std::move(instances));
            auto source = entity;
            source.properties["model"] = model.to_json();
            scoped.emplace(catalog_id, std::move(source));
            models.emplace(catalog_id, model);
        }
        AssemblyExpansionBudget budget;
        context = "visible assembly scope";
        const auto independent = expand_document_assembly_instances(scoped, budget);
        const auto append = [&](const std::string& catalog_id, const AssemblyExpansion& expansion,
                                const std::string& owner_id, bool external) {
            context = owner_id;
            const auto& model = models.at(catalog_id);
            const auto& instance = expansion.source_instance;
            const auto type = std::find_if(model.types().begin(), model.types().end(),
                [&](const auto& value) { return value.id == instance.type_id; });
            if (type == model.types().end()) throw std::invalid_argument("assembly root type is missing");
            const auto geometry = make_assembly_geometry(expansion);
            const std::optional<std::string> document_root = external
                ? std::optional<std::string>{owner_id} : std::nullopt;
            const auto root_key = nlohmann::json{{"version", 1},
                {"origin", external ? "document_instance" : "embedded_catalog"},
                {"catalog_id", catalog_id}, {"instance_id", instance.id},
                {"document_entity_id", document_root
                    ? nlohmann::json(*document_root) : nlohmann::json(nullptr)}};
            std::vector<ScheduleSourceRef> sources{{catalog_id, "model"}};
            if (external) sources.push_back({owner_id, "instance"});
            if (instance.placement) sources.push_back({instance.placement->host_entity_id, "geometry"});
            normalize_sources(sources);
            const auto cell = [&](ScheduleValue value, std::string explanation) {
                return ScheduleCell{std::move(value), false, sources, std::move(explanation)};
            };
            ScheduleRow row;
            // Persisted roots retain their authored entity ID. Embedded roots
            // share the collision-safe render alias used by native/sidebar rows.
            row.object_id = external ? owner_id : embedded_ids.at({catalog_id, instance.id});
            row.mark = "A-" + catalog_id + "-" + instance.id;
            row.kind = ScheduleRowKind::assembly;
            const auto data = [&](const std::string& name, ScheduleValue value) {
                row.cells.emplace(name, cell(std::move(value), "Resolved reusable assembly instance; edit its authoritative source"));
            };
            data("mark",row.mark);
            data("name",type->name);
            data("count",std::int64_t{1});
            data("catalog_id",catalog_id);
            data("instance_id",instance.id);
            data("type_id",instance.type_id);
            data("profile_count",static_cast<std::int64_t>(geometry.solids.size()));
            data("declared_node_count",static_cast<std::int64_t>(expansion.nodes.size()));
            if (!expansion.nodes.empty())
                for (const auto& [slot,material_id] : expansion.nodes.front().materials)
                    data("material:" + slot,material_id);
            if (instance.placement) {
                const auto& placement = *instance.placement;
                data("host_entity_id",placement.host_entity_id);
                data("rotation_radians",placement.rotation_radians);
                data("scale",placement.scale);
                data("translation_x_m",ScheduleQuantity{placement.translation_m.x,ScheduleUnit::metre});
                data("translation_y_m",ScheduleQuantity{placement.translation_m.y,ScheduleUnit::metre});
                data("translation_z_m",ScheduleQuantity{placement.translation_z_m,ScheduleUnit::metre});
            } else if (instance.root_transform) {
                const auto& transform = *instance.root_transform;
                data("rotation_radians",transform.rotation_radians);
                data("scale",transform.scale);
                data("translation_x_m",ScheduleQuantity{transform.translation_m.x,ScheduleUnit::metre});
                data("translation_y_m",ScheduleQuantity{transform.translation_m.y,ScheduleUnit::metre});
                data("translation_z_m",ScheduleQuantity{transform.translation_m.z,ScheduleUnit::metre});
            }
            for (const auto& [key,value] : expansion.declared_quantities) {
                auto quantity_sources = sources;
                for (const auto& node : expansion.nodes) {
                    const auto declared = node.quantities.find(key.name);
                    if (declared != node.quantities.end() && declared->second.unit == key.unit)
                        quantity_sources.push_back({catalog_id,"assembly_node:" +
                            nlohmann::json(node.part_path).dump() + ":quantity:" + assembly_row_component(key.name)});
                }
                normalize_sources(quantity_sources);
                if (!row.cells.emplace(assembly_quantity_column(key, expansion.declared_quantities),
                    ScheduleCell{assembly_quantity_value({value,key.unit}),false,std::move(quantity_sources),
                        "Authored quantity summed once per resolved node; never scaled or inferred from profile geometry"}).second)
                    throw std::invalid_argument("assembly quantity columns are ambiguous");
            }
            if (!geometry.solids.empty())
                row.cells.emplace("volume",cell(ScheduleQuantity{geometry.volume_m3,ScheduleUnit::cubic_metre},
                    "Sum of actual transformed profile solids after their holes; each profile contributes once"));
            rows.push_back(std::move(row));
            for (const auto& solid : geometry.solids) {
                const auto& profile = solid.source;
                const auto path = nlohmann::json(profile.part_path).dump();
                const auto key = "assembly-profile:" +
                    assembly_profile_presentation_key(catalog_id, instance, profile,
                        document_root).dump() + ":material";
                ScheduleRow material_row;
                material_row.object_id = key;
                material_row.mark = "AM-" + instance.id + "-" + profile.profile.id;
                material_row.kind = ScheduleRowKind::material;
                auto profile_sources = sources;
                // Structured JSON path framing preserves colon-bearing local IDs.
                profile_sources.push_back({catalog_id,"assembly_profile:" + path + ":" +
                    assembly_row_component(profile.profile.id)});
                normalize_sources(profile_sources);
                const auto profile_cell = [&](ScheduleValue value) {
                    return ScheduleCell{std::move(value),false,profile_sources,
                        "Actual transformed profile solid after its own holes; stable local part path; no authored quantity substitution"};
                };
                std::string material_name = "Unassigned assembly material";
                if (profile.material_id) {
                    const auto material = std::find_if(model.materials().begin(),model.materials().end(),
                        [&](const auto& value) { return value.id == *profile.material_id; });
                    if (material == model.materials().end()) throw std::invalid_argument("assembly profile material is missing");
                    material_name = material->name;
                    material_row.cells.emplace("material_id",profile_cell(*profile.material_id));
                } else {
                    // Summaries distinguish this absence from assigned identity.
                    material_row.cells.emplace("material_id",profile_cell(std::string{}));
                }
                material_row.cells.emplace("name",profile_cell(material_name));
                material_row.cells.emplace("count",profile_cell(std::int64_t{1}));
                material_row.cells.emplace("catalog_id",profile_cell(catalog_id));
                material_row.cells.emplace("instance_id",profile_cell(instance.id));
                material_row.cells.emplace("source_entity_id",profile_cell(owner_id));
                material_row.cells.emplace("type_id",profile_cell(profile.type_id));
                material_row.cells.emplace("part_path",profile_cell(path));
                material_row.cells.emplace("profile_id",profile_cell(profile.profile.id));
                material_row.cells.emplace("takeoff_basis",profile_cell(std::string("assembly_profile_net")));
                if (profile.profile.material_slot)
                    material_row.cells.emplace("slot",profile_cell(*profile.profile.material_slot));
                material_row.cells.emplace("volume",profile_cell(ScheduleQuantity{solid.volume_m3,ScheduleUnit::cubic_metre}));
                rows.push_back(std::move(material_row));
            }
            // Preserve historical declaration-only material slots as readable
            // rows without certifying declarations as measured material volume.
            if (expansion.profiles.empty()) {
                for (const auto& node : expansion.nodes) for (const auto& [slot,material_id] : node.materials) {
                    const auto material = std::find_if(model.materials().begin(),model.materials().end(),
                        [&](const auto& value) { return value.id == material_id; });
                    if (material == model.materials().end()) throw std::invalid_argument("assembly slot material is missing");
                    const auto path = nlohmann::json(node.part_path).dump();
                    ScheduleRow declared;
                    auto slot_key = root_key;
                    slot_key["part_path"] = node.part_path;
                    slot_key["type_id"] = node.type_id;
                    slot_key["slot"] = slot;
                    declared.object_id = "assembly-slot:" + slot_key.dump() + ":material";
                    declared.mark = "AM-" + instance.id + "-" + slot;
                    declared.kind = ScheduleRowKind::material;
                    for (auto [key,value] : std::map<std::string,ScheduleValue>{
                        {"name",material->name},{"count",std::int64_t{1}},{"catalog_id",catalog_id},
                        {"material_id",material_id},{"instance_id",instance.id},{"part_path",path},{"slot",slot}})
                        declared.cells.emplace(key,cell(std::move(value),"Authored material slot without a measurable profile"));
                    const auto volume = std::find_if(node.quantities.begin(),node.quantities.end(),[](const auto& item) {
                        return item.second.unit == AssemblyQuantityUnit::cubic_metre &&
                            (item.first == "volume" || item.first == "net_volume");
                    });
                    if (volume != node.quantities.end()) declared.cells.emplace("declared_volume",
                        cell(assembly_quantity_value(volume->second),"Authored declaration; excluded from measured material takeoff"));
                    rows.push_back(std::move(declared));
                }
            }
        };
        for (const auto& [catalog_id,model] : models) {
            for (const auto& instance : model.instances()) {
                // Whole-scope limits were consumed exactly once above. This
                // isolated replay obtains the retained legacy expansion only.
                AssemblyExpansionBudget replay;
                append(catalog_id,model.expand(instance,replay),catalog_id,false);
            }
        }
        for (const auto& [id,expansion] : independent) {
            const auto source = decode_document_assembly_instance(scoped.at(id));
            append(source.assembly_catalog_id,expansion,id,true);
        }
        std::set<std::string,std::less<>> identities;
        for (const auto& existing : projection.snapshot.rows) identities.insert(existing.object_id);
        for (const auto& row : rows)
            if (!identities.insert(row.object_id).second)
                throw std::invalid_argument("assembly schedule row identity collides with another source");
        projection.snapshot.rows.insert(projection.snapshot.rows.end(),
            std::make_move_iterator(rows.begin()),std::make_move_iterator(rows.end()));
    } catch (const Standard_Failure& error) {
        projection.diagnostics.push_back(context + ": assembly schedule unavailable: " +
            (error.what() ? error.what() : "solid construction failed"));
    } catch (const std::exception& error) {
        // No partial assembly output escapes malformed catalogs, cycles,
        // numeric overflow, document-wide budgets or failed profile solids.
        projection.diagnostics.push_back(context + ": assembly schedule unavailable: " + error.what());
    }
}


void add_building_quantity(ScheduleRecord& record, const Entity& entity,
                           std::string_view source_name, std::string_view cell_name,
                           ScheduleUnit unit) {
    const auto found = entity.properties.find(std::string(source_name));
    if (found == entity.properties.end() || !found->is_number()) return;
    const auto value = found->get<double>();
    if (!std::isfinite(value)) return;
    record.properties.emplace(std::string(cell_name), ScheduleQuantity{value, unit});
}

std::optional<std::string> building_text_field(const Entity& entity, std::string_view name) {
    if (!entity.properties.is_object()) return std::nullopt;
    const auto found = entity.properties.find(std::string(name));
    if (found == entity.properties.end() || !found->is_string()) return std::nullopt;
    const auto value = found->get<std::string>();
    return value.empty() ? std::nullopt : std::optional<std::string>(value);
}

std::string building_mark_for(const Entity& entity, std::string_view prefix,
                              std::vector<std::string>& diagnostics) {
    if (const auto mark = building_text_field(entity, "mark")) return *mark;
    diagnostics.push_back(entity.type + " " + entity.id +
                          ": mark is absent; the stable entity ID is used as the deterministic mark");
    return std::string(prefix) + entity.id;
}

void add_building_scalar(ScheduleRecord& record, const Entity& entity,
                         std::string_view source_name, std::string_view cell_name) {
    const auto found = entity.properties.find(std::string(source_name));
    if (found == entity.properties.end() || !found->is_number()) return;
    const auto value = found->get<double>();
    if (std::isfinite(value)) record.properties.emplace(std::string(cell_name), value);
}

BuildingObject effective_building_object(const DocumentSnapshot& document, const Entity& entity) {
    auto object = decode_building_entity(entity);
    if (const auto* rail = std::get_if<Railing>(&object); rail && (rail->host || rail->landing_host)) return object;
    return decode_building_entity(resolve_vertical_placement(document, entity));
}

std::vector<ScheduleSourceRef> building_geometry_sources(const DocumentSnapshot& document,
    const Entity& entity, const BuildingObject& object) {
    std::vector<ScheduleSourceRef> sources{{entity.id, "geometry"}};
    const Entity* placed = &entity;
    if (const auto* rail = std::get_if<Railing>(&object); rail && (rail->host || rail->landing_host)) {
        const auto host = document.entities().find(rail->host?rail->host->stair_id:rail->landing_host->stair_id);
        if (host == document.entities().end() || host->second.type != "stair")
            throw std::invalid_argument("hosted railing stair is missing");
        placed = &host->second;
        sources.push_back({placed->id, "geometry"});
    }
    std::set<std::string, std::less<>> visited;
    for (;;) {
        if (!visited.insert(placed->id).second) break;
        if (placed->properties.contains("vertical_placement"))
            sources.push_back({placed->id, "vertical_placement"});
        for (const auto* key : {"vertical_level_binding", "level_connection"}) {
            const auto binding = placed->properties.find(key);
            if (binding == placed->properties.end() || !binding->is_object()) continue;
            sources.push_back({placed->id, key});
            const auto graph_id = binding->find("graph_id");
            if (graph_id != binding->end() && graph_id->is_string() &&
                document.entities().contains(graph_id->get<std::string>()))
                sources.push_back({graph_id->get<std::string>(), "model"});
        }
        const Entity* parent = nullptr;
        for (const auto* key : {"layer_id", "floor_id", "building_id", "property_id"}) {
            const auto reference = placed->properties.find(key);
            if (reference == placed->properties.end() || !reference->is_string()) continue;
            const auto found = document.entities().find(reference->get<std::string>());
            if (found != document.entities().end()) {
                sources.push_back({placed->id, key});
                parent = &found->second;
                break;
            }
        }
        if (!parent) break;
        placed = parent;
    }
    normalize_sources(sources);
    return sources;
}

void add_layout_quantity(ScheduleRecord& record, std::string name, ScheduleValue value) {
    if (record.properties.contains(name)) return;
    record.calculated.emplace(std::move(name), ScheduleCalculation{std::move(value),
        {{record.object_id, "geometry"}}, "Derived from the current authoritative stair layout"});
}

void append_building_rows(const DocumentSnapshot& document,
                          DocumentScheduleProjection& projection,
                          const std::set<std::string, std::less<>>* visible_entity_ids) {
    std::vector<ScheduleRecord> records;
    std::map<std::string, std::vector<ScheduleSourceRef>, std::less<>> geometry_sources;
    for (const auto& [id, entity] : document.entities()) {
        if (!can_recognize_building_entity_type(entity.type) ||
            (visible_entity_ids && !visible_entity_ids->contains(id))) {
            continue;
        }
        try {
            const auto object = effective_building_object(document, entity);
            ScheduleRecord record;
            record.object_id = id;
            record.kind = ScheduleRowKind::building;
            record.mark = building_mark_for(entity, "B-", projection.diagnostics);
            record.properties.emplace("type", entity.type);
            if (const auto form = building_text_field(entity, "form"))
                record.properties.emplace("form", *form);
            if (!std::holds_alternative<StairFlight>(object))
                add_building_quantity(record, entity, "width_m", "width", ScheduleUnit::metre);
            add_building_quantity(record, entity, "depth_m", "depth", ScheduleUnit::metre);
            add_building_quantity(record, entity, "height_m", "height", ScheduleUnit::metre);
            add_building_quantity(record, entity, "radius_m", "radius", ScheduleUnit::metre);
            add_building_quantity(record, entity, "length_m", "length", ScheduleUnit::metre);
            // Stair run and rise are derived from its canonical topology.
            // Unknown preserved properties with these names are not authoring
            // authority for a stair and must not shadow the layout quantities.
            if (!std::holds_alternative<StairFlight>(object))
                add_building_quantity(record, entity, "run_m", "run", ScheduleUnit::metre);
            add_building_quantity(record, entity, "span_m", "span", ScheduleUnit::metre);
            if (!std::holds_alternative<StairFlight>(object))
                add_building_quantity(record, entity, "rise_m", "rise", ScheduleUnit::metre);
            add_building_quantity(record, entity, "overhang_m", "overhang", ScheduleUnit::metre);
            add_building_quantity(record, entity, "thickness_m", "thickness", ScheduleUnit::metre);
            if (!std::holds_alternative<StairFlight>(object))
                add_building_quantity(record, entity, "going_m", "going", ScheduleUnit::metre);
            add_building_quantity(record, entity, "post_spacing_m", "post_spacing", ScheduleUnit::metre);
            add_building_scalar(record, entity, "pitch_rad", "pitch_radians");
            add_building_scalar(record, entity, "rotation_rad", "rotation_radians");
            add_building_scalar(record, entity, "orientation_rad", "orientation_radians");
            if (const auto found = entity.properties.find("riser_count");
                found != entity.properties.end() && found->is_number_integer()) {
                record.properties.emplace("riser_count", found->get<std::int64_t>());
            }
            if (const auto found = entity.properties.find("roof_openings");
                found != entity.properties.end() && found->is_array()) {
                record.properties.emplace("opening_count",
                                          static_cast<std::int64_t>(found->size()));
            }
            if (const auto found = entity.properties.find("level_connection");
                found != entity.properties.end() && found->is_object()) {
                static constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
                    connection_fields{{
                        {"graph_id", "level_graph_id"},
                        {"link_id", "level_link_id"},
                        {"lower_level_id", "lower_level_id"},
                        {"upper_level_id", "upper_level_id"},
                    }};
                for (const auto [source_name, cell_name] : connection_fields) {
                    const auto value = found->find(std::string(source_name));
                    if (value != found->end() && value->is_string() &&
                        !value->get<std::string>().empty()) {
                        record.properties.emplace(std::string(cell_name),
                                                  value->get<std::string>());
                    }
                }
            }
            if (const auto* beam = std::get_if<Beam>(&object)) {
                const auto dx = beam->end.x - beam->start.x;
                const auto dy = beam->end.y - beam->start.y;
                const auto dz = beam->end.z - beam->start.z;
                const auto length = std::hypot(std::hypot(dx, dy), dz);
                if (std::isfinite(length))
                    record.properties.emplace("length", ScheduleQuantity{length, ScheduleUnit::metre});
            }
            if (const auto* stair = std::get_if<StairFlight>(&object)) {
                const auto layout = derive_stair_layout(*stair);
                const auto add_flight_dimension = [&](const char* name, double StairFlightLayout::*dimension) {
                    const auto first = layout.flights.front().*dimension;
                    const auto shared = std::all_of(layout.flights.begin(), layout.flights.end(),
                        [&](const auto& flight) { return flight.*dimension == first; });
                    if (shared) {
                        add_layout_quantity(record, name, ScheduleQuantity{first, ScheduleUnit::metre});
                        return;
                    }
                    for (std::size_t index = 0; index < layout.flights.size(); ++index) {
                        const auto& flight = layout.flights[index];
                        const auto label = "Flight " + std::to_string(index + 1) + ' ' + name;
                        record.calculated.emplace(label, ScheduleCalculation{
                            ScheduleQuantity{flight.*dimension, ScheduleUnit::metre}, {{id, "geometry"}},
                            "Resolved " + std::string(name) + " of flight " + std::to_string(index + 1) +
                                " (" + flight.id + ") from the current authoritative stair layout"});
                    }
                };
                add_flight_dimension("width", &StairFlightLayout::width);
                add_flight_dimension("going", &StairFlightLayout::going);
                double run = 0.0;
                for (const auto& flight : layout.flights) run += flight.run;
                add_layout_quantity(record, "rise", ScheduleQuantity{stair->total_rise, ScheduleUnit::metre});
                add_layout_quantity(record, "riser_height", ScheduleQuantity{
                    stair->total_rise / static_cast<double>(stair->riser_count), ScheduleUnit::metre});
                add_layout_quantity(record, "run", ScheduleQuantity{run, ScheduleUnit::metre});
                add_layout_quantity(record, "flight_count", static_cast<std::int64_t>(layout.flights.size()));
                add_layout_quantity(record, "landing_count", static_cast<std::int64_t>(layout.landings.size()));
            }
            if (const auto* rail = std::get_if<Railing>(&object); rail && (rail->host || rail->landing_host)) {
                const auto host = document.entities().find(rail->host?rail->host->stair_id:rail->landing_host->stair_id);
                if (host == document.entities().end() || host->second.type != "stair")
                    throw std::invalid_argument("hosted railing stair is missing");
                const auto current_host = decode_building_entity(resolve_vertical_placement(document, host->second));
                const auto* stair = std::get_if<StairFlight>(&current_host);
                if (!stair) throw std::invalid_argument("hosted railing source is not a supported stair");
                const auto layout = derive_hosted_railing_layout(*rail, *stair);
                const auto& a = layout.rail_start;
                const auto& b = layout.rail_end;
                add_layout_quantity(record, "length", ScheduleQuantity{
                    std::hypot(std::hypot(b.x-a.x, b.y-a.y), b.z-a.z), ScheduleUnit::metre});
                add_layout_quantity(record, "post_count", static_cast<std::int64_t>(layout.posts.size()));
                if(rail->landing_host) {
                    const auto& h=*rail->landing_host;
                    record.properties.erase("side"); record.properties.erase("host_flight_id");
                    add_layout_quantity(record,"host_stair_id",h.stair_id);
                    add_layout_quantity(record,"host_role",std::string(h.role==StairLandingRole::top?"top":"connecting"));
                    if(h.role==StairLandingRole::connecting) {
                        add_layout_quantity(record,"host_landing_id",h.landing_id);
                        add_layout_quantity(record,"host_outgoing_flight_id",h.outgoing_flight_id);
                    }
                    add_layout_quantity(record,"host_incoming_flight_id",h.incoming_flight_id);
                    add_layout_quantity(record,"edge_index",static_cast<std::int64_t>(h.edge_index));
                } else {
                    add_layout_quantity(record, "host_stair_id", rail->host->stair_id);
                    add_layout_quantity(record, "host_flight_id", rail->host->flight_id);
                    add_layout_quantity(record, "side", std::string(rail->host->side == StairRailingSide::left ? "left" : "right"));
                }
            }
            const auto shape = make_building_shape(object, document.entities());
            const auto volume = solid_volume(shape);
            if (!std::isfinite(volume) || volume <= 0.0)
                throw std::invalid_argument("building solid volume must be positive and finite");
            record.calculated.emplace("volume", ScheduleCalculation{
                ScheduleQuantity{volume, ScheduleUnit::cubic_metre},
                {{id, "geometry"}},
                "Net volume calculated from the canonical building solid"});
            geometry_sources[id] = building_geometry_sources(document, entity, object);
            records.push_back(std::move(record));
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": building schedule unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": building schedule unavailable: " + error.what());
        }
    }
    if (records.empty()) return;
    try {
        auto rows = build_schedule(records, document.revision()).rows;
        for (auto& row : rows) {
            for (auto& [name, cell] : row.cells) {
                cell.editable = false;
                if (cell.explanation.empty())
                    cell.explanation = "Authored building-object property; edit the object in the architectural workspace";
                else {
                    append_sources(cell.sources, geometry_sources.at(row.object_id));
                    normalize_sources(cell.sources);
                }
            }
            projection.snapshot.rows.push_back(std::move(row));
        }
    } catch (const std::exception& error) {
        projection.diagnostics.push_back(std::string("building schedule rejected: ") + error.what());
    }
}

void append_roof_join_rows(const DocumentSnapshot& document,
                          DocumentScheduleProjection& projection,
                          const std::set<std::string, std::less<>>* visible_entity_ids) {
    constexpr auto explanation = "Net joined material after source openings; authored roof_ids order is overlap priority (earlier members own shared volume); no waste allowance";
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "roof_join" ||
            (visible_entity_ids && !visible_entity_ids->contains(id))) continue;
        try {
            const auto join = parse_roof_join(entity.properties, id);
            if (visible_entity_ids && std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
                [&](const auto& source) { return !visible_entity_ids->contains(source); })) continue;
            // Gross source quantities remain reviewable but never contribute
            // alongside this join to net material totals, including on failure.
            for (auto& row : projection.snapshot.rows) {
                if (is_assembly_material_row(row)) continue;
                if (std::find(join.roof_ids.begin(), join.roof_ids.end(),
                    row.kind == ScheduleRowKind::material ? material_source_id(row.object_id, &document) : row.object_id)
                    == join.roof_ids.end()) continue;
                row.cells["takeoff_basis"] = ScheduleCell{std::string("source_gross"), false,
                    {{id, "roof_ids"}}, "Gross source quantity before joined overlap deductions; excluded from joined net material totals"};
                row.cells["joined_roof_id"] = ScheduleCell{id, false, {{id, "roof_ids"}},
                    "Authoritative roof join membership"};
                const auto volume = row.cells.find("volume");
                if (volume != row.cells.end()) {
                    auto gross = volume->second;
                    gross.explanation = "Gross source solid volume after its own openings, before joined overlap deductions";
                    row.cells["gross_volume"] = gross;
                    volume->second.explanation = gross.explanation;
                }
            }
            std::vector<TopoDS_Shape> shapes;
            std::vector<const Entity*> sources;
            std::vector<ScheduleSourceRef> provenance{{id, "roof_ids"}};
            for (const auto& source_id : join.roof_ids) {
                const auto source = document.entities().find(source_id);
                if (source == document.entities().end() || source->second.type != "roof")
                    throw std::invalid_argument("roof join source roof is missing: " + source_id);
                sources.push_back(&source->second);
                shapes.push_back(make_building_shape(effective_building_object(document, source->second)));
                provenance.push_back({source_id, "geometry"});
                provenance.push_back({source_id, "material_assignment"});
                append_sources(provenance, building_geometry_sources(document, source->second,
                    effective_building_object(document, source->second)));
            }
            const auto partition = make_roof_join_partition(join, shapes);
            std::vector<ScheduleRow> rows;
            double gross_total = 0.0;
            for (std::size_t index = 0; index < partition.regions.size(); ++index) {
                const auto& region = partition.regions[index];
                gross_total += region.gross_volume;
                std::optional<RoofJoinMaterialAssignment> binding = join.material_assignment;
                if (!binding && sources[index]->properties.contains("material_assignment")) {
                    const auto& assigned = sources[index]->properties.at("material_assignment");
                    binding = RoofJoinMaterialAssignment{assigned.at("catalog_id").get<std::string>(),
                        assigned.at("material_id").get<std::string>()};
                }
                auto region_sources = provenance;
                std::string name = "Unassigned roof material";
                if (binding) {
                    const auto catalog = document.entities().find(binding->catalog_id);
                    if (catalog == document.entities().end() || catalog->second.type != "assembly_model")
                        throw std::invalid_argument("roof material catalog is missing");
                    const auto model = AssemblyModel::from_json(catalog->second.properties.at("model"));
                    const auto material = std::find_if(model.materials().begin(), model.materials().end(),
                        [&](const auto& candidate) { return candidate.id == binding->material_id; });
                    if (material == model.materials().end())
                        throw std::invalid_argument("roof material assignment references a missing catalog material");
                    name = material->name;
                    region_sources.push_back({binding->catalog_id, "model"});
                    region_sources.push_back({join.material_assignment ? id : region.source_roof_id, "material_assignment"});
                }
                normalize_sources(region_sources);
                ScheduleRow row;
                row.object_id = roof_join_material_schedule_row_id(id, region.source_roof_id);
                row.mark = "RM-" + id + "-" + std::to_string(index + 1);
                row.kind = ScheduleRowKind::material;
                const auto cell = [&](ScheduleValue value, std::string text = {}) {
                    if (text.empty()) text = explanation;
                    return ScheduleCell{std::move(value), false, region_sources, std::move(text)};
                };
                row.cells.emplace("name", cell(name));
                row.cells.emplace("count", cell(std::int64_t{1}));
                row.cells.emplace("joined_roof_id", cell(id));
                row.cells.emplace("source_roof_id", cell(region.source_roof_id));
                row.cells.emplace("overlap_priority", cell(static_cast<std::int64_t>(index + 1)));
                row.cells.emplace("takeoff_basis", cell(std::string("joined_net")));
                row.cells.emplace("material_binding", cell(std::string(join.material_assignment ? "join_override" : "source_member")));
                row.cells.emplace("gross_volume", cell(ScheduleQuantity{region.gross_volume, ScheduleUnit::cubic_metre},
                    "Gross member solid volume after its own openings, before overlap deductions"));
                row.cells.emplace("volume", cell(ScheduleQuantity{region.net_volume, ScheduleUnit::cubic_metre}));
                if (binding) {
                    row.cells.emplace("catalog_id", cell(binding->catalog_id));
                    row.cells.emplace("material_id", cell(binding->material_id));
                }
                rows.push_back(std::move(row));
            }
            normalize_sources(provenance);
            ScheduleRow fused;
            fused.object_id = id;
            fused.mark = "RJ-" + id;
            fused.kind = ScheduleRowKind::building;
            fused.cells.emplace("type", ScheduleCell{std::string("roof_join"), false, provenance, explanation});
            fused.cells.emplace("volume", ScheduleCell{ScheduleQuantity{partition.fused_volume, ScheduleUnit::cubic_metre},
                false, provenance, "Fused union volume; sum of ordered disjoint net material regions"});
            fused.cells.emplace("gross_volume", ScheduleCell{ScheduleQuantity{gross_total, ScheduleUnit::cubic_metre},
                false, provenance, "Sum of gross member volumes before overlap deductions"});
            fused.cells.emplace("member_count", ScheduleCell{static_cast<std::int64_t>(join.roof_ids.size()),
                false, provenance, "Authored roof join member count"});
            rows.push_back(std::move(fused));
            projection.snapshot.rows.insert(projection.snapshot.rows.end(),
                std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": joined roof takeoff unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": joined roof takeoff unavailable: " + error.what());
        }
    }
}

DocumentScheduleProjection augment(const DocumentSnapshot& document, DocumentScheduleProjection projection,
                                   const std::set<std::string, std::less<>>* visible_entity_ids) {
    // Physical cuts follow the complete document's saved phase choices;
    // presentation visibility only controls which schedule rows are emitted.
    const auto phase_scope = constraint_phase_scope(document.entities());
    std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "opening" || phase_scope.inactive_owner_ids.contains(id)) continue;
        std::string host, error;
        if (read_document_wall_id(entity, host, error) &&
            !phase_scope.inactive_owner_ids.contains(host)) openings[host].push_back(&entity);
    }
    for (auto& row : projection.snapshot.rows) {
        if (row.kind != ScheduleRowKind::material) continue;
        if (row.object_id.size() <= material_suffix.size() ||
            !row.object_id.ends_with(material_suffix)) continue;
        if (is_layer_material_row(row)) continue;
        const auto id = material_source_id(row.object_id, &document);
        const auto entity_it = document.entities().find(id);
        if (entity_it == document.entities().end()) {
            projection.diagnostics.push_back(id + ": material source entity was not found");
            continue;
        }
        const auto& entity = entity_it->second;
        if (entity.type == "roof_join") continue;
        if (!entity.properties.contains("material_assignment")) continue;
        try {
            TopoDS_Shape shape;
            std::string error;
            std::vector<ScheduleSourceRef> sources{{id, "material_assignment"}, {id, "geometry"}};
            if (entity.type == "wall") {
                Wall wall;
                if (!read_document_wall(entity, openings[id], wall, error)) throw std::invalid_argument(error);
                shape = make_wall(wall);
                for (const auto* opening : openings[id]) sources.push_back({opening->id, "geometry"});
            } else if (entity.type == "slab") {
                Slab slab;
                if (!read_document_slab(entity, slab, error)) throw std::invalid_argument(error);
                shape = make_slab(slab);
            } else if (can_recognize_building_entity_type(entity.type)) {
                const auto object = effective_building_object(document, entity);
                shape = make_building_shape(object, document.entities());
                append_sources(sources, building_geometry_sources(document, entity, object));
                normalize_sources(sources);
            } else {
                throw std::invalid_argument("object has no material solid representation");
            }
            const auto volume = solid_volume(shape);
            if (!std::isfinite(volume) || volume <= 0) throw std::invalid_argument("solid volume must be positive and finite");
            row.cells.emplace("volume", ScheduleCell{ScheduleQuantity{volume, ScheduleUnit::cubic_metre},
                false, std::move(sources), "Net solid volume after openings; one homogeneous assigned material"});
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": material volume unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": material volume unavailable: " + error.what());
        }
    }
    append_layer_material_rows(document, openings, projection, visible_entity_ids);
    append_assembly_rows(document, projection, visible_entity_ids);
    append_building_rows(document, projection, visible_entity_ids);
    // The core adapter's generic homogeneous assignment row is replaced by
    // the complete architecture-specific joined partition, never added twice.
    std::erase_if(projection.snapshot.rows, [&](const auto& row) {
        if (is_assembly_material_row(row)) return false;
        const auto source = document.entities().find(material_source_id(row.object_id, &document));
        return row.kind == ScheduleRowKind::material && source != document.entities().end() &&
               source->second.type == "roof_join";
    });
    append_roof_join_rows(document, projection, visible_entity_ids);
    append_material_summaries(document, projection);
    std::sort(projection.diagnostics.begin(), projection.diagnostics.end());
    projection.diagnostics.erase(std::unique(projection.diagnostics.begin(), projection.diagnostics.end()),
        projection.diagnostics.end());
    return projection;
}
} // namespace

DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document) {
    return augment(document, build_document_schedules(document), nullptr);
}
DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>& visible_entity_ids) {
    return augment(document, build_document_schedules(document, visible_entity_ids),
                   &visible_entity_ids);
}
} // namespace sketch

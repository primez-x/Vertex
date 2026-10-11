#include "sketch/retained_geometry_work.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/document.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t topology_limit = 16'384;
constexpr std::size_t topology_work_limit = 67'108'864;

void require(bool condition, const char* message) {
    if (!condition) throw RetainedGeometryWorkError(message);
}
void charge(std::size_t& consumed, std::size_t amount, std::size_t maximum, const char* message) {
    require(consumed <= maximum && amount <= maximum - consumed, message);
    consumed += amount;
}
std::size_t checked_product(std::size_t a, std::size_t b) {
    require(!b || a <= std::numeric_limits<std::size_t>::max() / b, "typed replay copy work limit");
    return a * b;
}
} // namespace

void reserve_retained_geometry_replay_work(const Json& value,
    const RetainedGeometryReserveProduct& reserve_product,
    bool root_geometry_array, std::size_t copies, const RetainedGeometryWorkLimits& limits) {
    std::size_t edges = 0, passes = 1, growth = 0;
    const auto count_edges = [&](const Json& rows) {
        require(rows.is_array() && rows.size() <= topology_limit, "typed topology row limit");
        edges = std::max(edges, rows.size()); reserve_product(1, rows.size());
    };
    const auto visit = [&](const auto& self, const Json& item, std::size_t depth) -> void {
        require(depth <= limits.maximum_depth, "typed replay depth limit");
        reserve_product(1, 1);
        if (!item.is_object()) return;
        const auto kind = item.find("kind");
        if (kind != item.end() && *kind == "insert_vertex")
            charge(growth, 1, topology_limit, "typed replay topology growth limit");
        for (const auto* slot : {"segments", "boundary", "edges", "replacement_segments"}) {
            const auto rows = item.find(slot);
            if (rows != item.end() && rows->is_array()) count_edges(*rows);
        }
        for (const auto* slot : {"operations", "transforms", "edits"}) {
            const auto rows = item.find(slot); if (rows == item.end()) continue;
            require(rows->is_array(), "typed replay operation inventory shape");
            charge(passes, rows->size(), limits.maximum_nodes, "typed replay operation limit");
            for (const auto& row : *rows) self(self, row, depth + 1);
        }
        for (const auto* slot : {"value", "edit", "replacement_authoring", "source_boundary_authoring",
            "source_boundary", "source_lineage", "outer"}) {
            const auto nested = item.find(slot);
            if (nested == item.end()) continue;
            if (nested->is_array()) for (const auto& row : *nested) self(self, row, depth + 1);
            else self(self, *nested, depth + 1);
        }
        const auto holes = item.find("holes");
        if (holes != item.end() && holes->is_array()) for (const auto& hole : *holes) {
            if (hole.is_array()) count_edges(hole); else self(self, hole, depth + 1);
        }
    };
    if (root_geometry_array && value.is_array()) count_edges(value); else visit(visit, value, 0);
    // Insert/edit operations can grow the topology beyond its captured origin.
    // Each typed operation adds at most one edge unless a retained replacement
    // array above already declares a larger shape.
    require(passes <= topology_limit && edges <= topology_limit - growth, "typed replay topology growth limit");
    const auto maximum_edges = edges + growth;
    require(!maximum_edges || maximum_edges <= topology_work_limit / maximum_edges, "typed replay topology work limit");
    reserve_product(maximum_edges * maximum_edges, checked_product(passes, copies));
}

void reserve_retained_geometry_owner_work(const Entity& owner,
    const RetainedGeometryReserveProduct& reserve_product,
    std::size_t copies, const RetainedGeometryWorkLimits& limits) {
    const auto replay = [&](const Json& value, bool root_geometry_array = false) {
        reserve_retained_geometry_replay_work(value, reserve_product, root_geometry_array, copies, limits);
    };
    if (can_recognize_boundary_entity_type(owner.type)) {
        std::size_t total_edges = 0;
        for (const auto* slot : {"segments", "boundary"}) if (owner.properties.contains(slot))
            if (owner.properties.at(slot).is_array()) {
                total_edges = std::max(total_edges, owner.properties.at(slot).size());
                replay(owner.properties.at(slot), true);
            }
        if (owner.properties.contains("holes")) {
            require(owner.properties.at("holes").is_array(), "intrinsic boundary hole inventory shape");
            for (const auto& hole : owner.properties.at("holes")) {
                require(hole.is_array(), "intrinsic boundary hole shape");
                charge(total_edges, hole.size(), topology_limit, "intrinsic boundary aggregate edge limit");
                replay(hole, true);
            }
            reserve_product(total_edges, checked_product(total_edges, checked_product(2, copies)));
        }
        for (const auto* slot : {"boundary_geometry_derivation", "boundary_identity_history"})
            if (owner.extensions.contains(slot)) replay(owner.extensions.at(slot));
        if (owner.properties.contains("boundary_authoring")) replay(owner.properties.at("boundary_authoring"));
    }
    if (owner.type == "measurement_linework") replay(owner.properties.at("model"));
    if (owner.extensions.contains("physical_wall_room")) {
        const auto& descriptor = owner.extensions.at("physical_wall_room");
        replay(descriptor);
        const auto& holes = descriptor.at("holes");
        require(holes.is_array(), "intrinsic physical room hole inventory shape");
        std::size_t edges = owner.properties.contains("segments") ? owner.properties.at("segments").size() : 0;
        for (const auto& hole : holes) {
            require(hole.is_array(), "intrinsic physical room hole shape");
            charge(edges, hole.size(), topology_limit, "intrinsic physical room aggregate edge limit");
        }
        reserve_product(edges, checked_product(edges, checked_product(2, copies)));
    }
    if (owner.type == "measurement_boundary" && owner.properties.contains("wall_measurement_source")) {
        const auto& source = owner.properties.at("wall_measurement_source");
        const auto& walls = source.at("walls");
        require(walls.is_array() && walls.size() <= 2048, "intrinsic wall measurement source inventory limit");
        const auto translations = source.find("translations");
        const auto passes = translations == source.end() ? 1 : translations->size() + 2;
        require(translations == source.end() || (translations->is_array() && translations->size() <= 4096), "intrinsic wall translation replay limit");
        reserve_product(walls.size() * walls.size(), checked_product(passes, checked_product(2, copies)));
    }
    if (owner.type == "wall") for (const auto* slot : {"wall_split_archive", "wall_merge_archive"})
        if (owner.extensions.contains(slot)) replay(owner.extensions.at(slot));
}
} // namespace sketch

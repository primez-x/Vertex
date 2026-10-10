#include "sketch/roof_entity_codec.hpp"
#include "sketch/document.hpp"
#include "sketch/architecture.hpp"
#include "sketch/project_organization.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void identity(std::string_view value) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof identity is empty or invalid");
}
const Json& field(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end()) invalid("Roof required field is missing");
    return *found;
}
double number(const Json& value, const char* name) {
    const auto& raw = field(value, name);
    if (!raw.is_number()) invalid("Roof field must be a finite number");
    const double result = raw.get<double>();
    if (!std::isfinite(result)) invalid("Roof field must be a finite number");
    return result;
}
std::string text(const Json& value, const char* name) {
    const auto& raw = field(value, name);
    if (!raw.is_string()) invalid("Roof field must be a string");
    return raw.get<std::string>();
}
Vec3 position(const Json& value) {
    const auto& raw = field(value, "base_position_m");
    if (!raw.is_array() || raw.size() != 3) invalid("Roof position requires three finite coordinates");
    for (const auto& coordinate : raw)
        if (!coordinate.is_number() || !std::isfinite(coordinate.get<double>()))
            invalid("Roof position requires three finite coordinates");
    return {raw[0].get<double>(), raw[1].get<double>(), raw[2].get<double>()};
}
unsigned version(const Json& value) {
    const auto& raw = field(value, "version");
    if ((!raw.is_number_integer() && !raw.is_number_unsigned()) ||
        (raw != 1 && raw != 2 && raw != 3))
        invalid("Unsupported roof schema version");
    return raw.get<unsigned>();
}
std::vector<RoofOpening> openings(const Json& value, unsigned schema) {
    if (schema == 1) {
        if (value.contains("roof_openings")) invalid("Roof openings require building schema version 2");
        return {};
    }
    const auto& entries = field(value, "roof_openings");
    if (!entries.is_array() || entries.size() > 256)
        invalid("Roof openings must be an array of at most 256 entries");
    std::vector<RoofOpening> result;
    for (const auto& entry : entries) {
        if (!entry.is_object()) invalid("Roof opening must be an object");
        const auto id = text(entry, "id");
        identity(id);
        RoofOpening opening{id, number(entry, "x_m"), number(entry, "y_m"),
            number(entry, "width_m"), number(entry, "depth_m"), std::nullopt};
        if (entry.contains("skylight")) {
            if (schema != 3) invalid("Roof skylights require building schema version 3");
            const auto& skylight = entry.at("skylight");
            if (!skylight.is_object() || skylight.size() != 4 ||
                !skylight.contains("version") || !skylight.contains("frame_width_m") ||
                !skylight.contains("curb_height_m") || !skylight.contains("glazing_thickness_m"))
                invalid("Roof skylight requires exactly its version and three dimension fields");
            const auto& raw_version = skylight.at("version");
            if ((!raw_version.is_number_integer() && !raw_version.is_number_unsigned()) ||
                raw_version != 1) invalid("Unsupported roof skylight schema version");
            opening.skylight = RoofSkylight{number(skylight, "frame_width_m"),
                number(skylight, "curb_height_m"), number(skylight, "glazing_thickness_m")};
            if (opening.skylight->frame_width <= 0.0 || opening.skylight->curb_height < 0.0 ||
                opening.skylight->glazing_thickness <= 0.0)
                invalid("Roof skylight dimensions must be positive with nonnegative curb height");
        }
        result.push_back(std::move(opening));
    }
    return result;
}
template<class Object>
Object decode(const Entity& entity, unsigned schema) {
    const auto& p = entity.properties;
    Object object;
    object.id = entity.id;
    object.base_position = position(p);
    object.orientation_radians = number(p, "orientation_rad");
    if constexpr (std::is_same_v<Object, SlopedRoofPanel>) object.run = number(p, "run_m");
    else object.length = number(p, "length_m");
    object.span = number(p, "span_m");
    object.rise = number(p, "rise_m");
    object.pitch_radians = number(p, "pitch_rad");
    object.overhang = number(p, "overhang_m");
    object.thickness = number(p, "thickness_m");
    object.openings = openings(p, schema);
    return object;
}

constexpr std::size_t maximum_skylight_cohort_members = 4096;
constexpr std::size_t maximum_skylight_cohort_openings = 4096;
constexpr std::size_t maximum_skylight_cohort_fills = 256;
constexpr std::size_t maximum_skylight_cavity_rebuilds = 4096;
constexpr std::size_t maximum_skylight_intersections = 2048;
constexpr double skylight_volume_tolerance = default_geometry_tolerance_metres *
    default_geometry_tolerance_metres * default_geometry_tolerance_metres;

bool has_skylights(const RoofObject& object) {
    return std::visit([](const auto& roof) {
        return std::any_of(roof.openings.begin(), roof.openings.end(),
            [](const auto& opening) { return opening.skylight.has_value(); });
    }, object);
}

struct SkylightCohortShape {
    TopoDS_Shape shape;
    Bnd_Box bounds;
    double volume{};
};

double cohort_solid_volume(const TopoDS_Shape& shape) {
    // A successful common may contain only a face or edge at contact. Volume
    // integration of those open remnants is not an actual material overlap.
    if (shape.IsNull()) return 0.0;
    double volume = 0.0;
    for (TopExp_Explorer solid(shape, TopAbs_SOLID); solid.More(); solid.Next()) {
        const auto member_volume = solid_volume(solid.Current());
        if (!std::isfinite(member_volume) || member_volume <= skylight_volume_tolerance)
            invalid("Roof skylight cohort contains invalid solid volume");
        volume += member_volume;
        if (!std::isfinite(volume))
            invalid("Roof skylight cohort solid volume exceeds the supported numeric range");
    }
    return volume;
}

SkylightCohortShape cohort_shape(TopoDS_Shape shape) {
    if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid())
        invalid("Roof skylight cohort contains invalid geometry");
    const auto volume = cohort_solid_volume(shape);
    if (!std::isfinite(volume) || volume <= skylight_volume_tolerance)
        invalid("Roof skylight cohort contains invalid solid volume");
    Bnd_Box bounds;
    BRepBndLib::Add(shape, bounds, false);
    if (bounds.IsVoid() || bounds.IsWhole())
        invalid("Roof skylight cohort contains unbounded geometry");
    double x0, y0, z0, x1, y1, z1;
    bounds.Get(x0, y0, z0, x1, y1, z1);
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(z0) ||
        !std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(z1) ||
        x0 > x1 || y0 > y1 || z0 > z1)
        invalid("Roof skylight cohort contains invalid bounds");
    return {std::move(shape), bounds, volume};
}

void require_skylight_clearance(const SkylightCohortShape& first,
    const SkylightCohortShape& second, std::size_t& intersections, const char* reason) {
    // Boxes only discard impossible intersections; admitted pairs always use
    // actual solids, including cut roof mouths and the sloped manufactured fill.
    if (first.bounds.IsOut(second.bounds)) return;
    if (intersections >= maximum_skylight_intersections)
        invalid("Roof skylight cohort intersection budget exceeded");
    ++intersections;
    BRepAlgoAPI_Common common(first.shape, second.shape);
    common.Build();
    if (!common.IsDone() || common.HasErrors() ||
        (!common.Shape().IsNull() && !BRepCheck_Analyzer(common.Shape()).IsValid()))
        invalid("Roof skylight cohort intersection could not be resolved");
    const auto overlap = cohort_solid_volume(common.Shape());
    const auto maximum_overlap = std::min(first.volume, second.volume);
    if (!std::isfinite(overlap) || overlap > maximum_overlap +
        std::max(skylight_volume_tolerance, maximum_overlap * 1e-7))
        invalid("Roof skylight cohort intersection has invalid volume");
    if (overlap > skylight_volume_tolerance) invalid(reason);
}
} // namespace

RoofObject decode_roof_entity(const Entity& entity) {
    identity(entity.id);
    if (entity.type != "roof" || !entity.properties.is_object())
        invalid("Roof codec requires an existing roof entity");
    const auto schema = version(entity.properties);
    const auto form = text(entity.properties, "form");
    if (form == "sloped_roof_panel") return decode<SlopedRoofPanel>(entity, schema);
    if (form == "gable_roof") return decode<GableRoof>(entity, schema);
    if (form == "hip_roof") return decode<HipRoof>(entity, schema);
    invalid("Unsupported roof building form");
}

nlohmann::json encode_roof_properties(const RoofObject& object) {
    return std::visit([](const auto& roof) {
        using Object = std::decay_t<decltype(roof)>;
        Json p{{"version", 1}, {"base_position_m", {roof.base_position.x,
            roof.base_position.y, roof.base_position.z}}, {"orientation_rad", roof.orientation_radians},
            {"span_m", roof.span}, {"rise_m", roof.rise}, {"pitch_rad", roof.pitch_radians},
            {"overhang_m", roof.overhang}, {"thickness_m", roof.thickness}};
        if constexpr (std::is_same_v<Object, SlopedRoofPanel>) {
            p["form"] = "sloped_roof_panel";
            p["run_m"] = roof.run;
        } else {
            p["form"] = std::is_same_v<Object, GableRoof> ? "gable_roof" : "hip_roof";
            p["length_m"] = roof.length;
        }
        if (!roof.openings.empty()) {
            p["version"] = 2;
            p["roof_openings"] = Json::array();
        }
        for (const auto& opening : roof.openings) {
            identity(opening.id);
            Json row{{"id", opening.id}, {"x_m", opening.x}, {"y_m", opening.y},
                {"width_m", opening.width}, {"depth_m", opening.depth}};
            if (opening.skylight) {
                p["version"] = 3;
                row["skylight"] = {{"version", 1}, {"frame_width_m", opening.skylight->frame_width},
                    {"curb_height_m", opening.skylight->curb_height},
                    {"glazing_thickness_m", opening.skylight->glazing_thickness}};
            }
            p["roof_openings"].push_back(std::move(row));
        }
        return p;
    }, object);
}

TopoDS_Shape make_roof_structure_shape(const RoofObject& object) {
    return std::visit([](const auto& roof) -> TopoDS_Shape {
        using Object = std::decay_t<decltype(roof)>;
        if constexpr (std::is_same_v<Object, SlopedRoofPanel>) return make_sloped_roof_panel(roof);
        else if constexpr (std::is_same_v<Object, GableRoof>) return make_gable_roof(roof);
        else return make_hip_roof(roof);
    }, object);
}

TopoDS_Shape make_roof_skylight_shape(const RoofObject& object, const std::string& opening_id) {
    // Validate the entire cut host and sibling roster before deriving a fill.
    (void)make_roof_structure_shape(object);
    return std::visit([&](const auto& roof) -> TopoDS_Shape {
        const auto found = std::find_if(roof.openings.begin(), roof.openings.end(),
            [&](const auto& opening) { return opening.id == opening_id; });
        if (found == roof.openings.end()) invalid("Skylight opening identity is not in the roof");
        if (!found->skylight) invalid("Roof opening has no skylight assembly");
        return make_roof_skylight(roof, *found);
    }, object);
}

void validate_roof_skylight_cohort(std::span<const RoofObject> objects) {
    // Ordinary roof cohorts preserve their existing arithmetic and geometry
    // path. Apply the new resource budgets only when a skylight is present.
    if (std::none_of(objects.begin(), objects.end(), has_skylights)) return;
    if (objects.size() > maximum_skylight_cohort_members)
        invalid("Roof skylight cohort member budget exceeded");
    std::size_t opening_count = 0;
    std::size_t fill_count = 0;
    std::size_t cavity_rebuilds = 0;
    for (const auto& object : objects) {
        std::visit([&](const auto& roof) {
            if (roof.openings.size() > 256 ||
                roof.openings.size() > maximum_skylight_cohort_openings - opening_count)
                invalid("Roof skylight cohort opening budget exceeded");
            opening_count += roof.openings.size();
            for (const auto& opening : roof.openings) {
                if (!opening.skylight) continue;
                if (fill_count >= maximum_skylight_cohort_fills)
                    invalid("Roof skylight cohort fill budget exceeded");
                ++fill_count;
                const auto retained_openings = roof.openings.size() - 1;
                if (retained_openings > maximum_skylight_cavity_rebuilds - cavity_rebuilds)
                    invalid("Roof skylight cohort cavity rebuild budget exceeded");
                cavity_rebuilds += retained_openings;
            }
        }, object);
    }
    try {
        std::vector<SkylightCohortShape> structures;
        structures.reserve(objects.size());
        for (const auto& object : objects)
            structures.push_back(cohort_shape(make_roof_structure_shape(object)));
        struct OwnedFill {
            std::size_t owner;
            SkylightCohortShape geometry;
        };
        std::vector<OwnedFill> fills;
        fills.reserve(fill_count);
        std::size_t intersections = 0;
        for (std::size_t owner = 0; owner < objects.size(); ++owner) {
            std::visit([&](const auto& roof) {
                for (std::size_t opening_index = 0; opening_index < roof.openings.size(); ++opening_index) {
                    const auto& opening = roof.openings[opening_index];
                    if (!opening.skylight) continue;
                    auto fill = cohort_shape(make_roof_skylight(roof, opening));
                    // Match IFC's real removed cavity: restore only this row,
                    // retain every sibling cut, then subtract the admitted host.
                    auto restored_roof = roof;
                    restored_roof.openings.erase(restored_roof.openings.begin() + opening_index);
                    const auto restored = cohort_shape(make_roof_structure_shape(RoofObject{restored_roof}));
                    BRepAlgoAPI_Cut removed(restored.shape, structures[owner].shape);
                    removed.Build();
                    if (!removed.IsDone() || removed.HasErrors())
                        invalid("Roof skylight cohort cavity could not be resolved");
                    const auto cavity = cohort_shape(removed.Shape());
                    const auto expected = restored.volume - structures[owner].volume;
                    if (!std::isfinite(expected) || expected <= skylight_volume_tolerance ||
                        std::abs(cavity.volume - expected) >
                            std::max(skylight_volume_tolerance, restored.volume * 1e-7))
                        invalid("Roof skylight cohort cavity has inconsistent volume");
                    for (std::size_t other = 0; other < structures.size(); ++other) {
                        if (other == owner) continue;
                        require_skylight_clearance(fill, structures[other], intersections,
                            "Roof skylight fill intersects another roof's structure");
                        require_skylight_clearance(cavity, structures[other], intersections,
                            "Roof skylight cavity intersects another roof's structure");
                    }
                    for (const auto& prior : fills) {
                        // The host builder already enforces disjoint sibling
                        // opening domains, including their thickness clearance.
                        if (prior.owner == owner) continue;
                        require_skylight_clearance(fill, prior.geometry, intersections,
                            "Roof skylight fills from different roofs intersect");
                    }
                    fills.push_back({owner, std::move(fill)});
                }
            }, objects[owner]);
        }
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Roof skylight cohort geometry failed: ") + error.what());
    }
}

void validate_roof_join_skylights(const RoofJoin& join,
    const std::map<std::string, Entity, std::less<>>& actual_entities) {
    // The ordinary join path already validates its sources. Avoid resolving
    // placements or constructing a new cohort for a roster with no skylights.
    const auto has_skylight_row = [&](const auto& id) {
        const auto source = actual_entities.find(id);
        if (source == actual_entities.end()) invalid("Roof skylight join member is missing");
        const auto openings = source->second.properties.find("roof_openings");
        return openings != source->second.properties.end() && openings->is_array() &&
            std::any_of(openings->begin(), openings->end(), [](const auto& row) {
                return row.is_object() && row.contains("skylight");
            });
    };
    if (std::none_of(join.roof_ids.begin(), join.roof_ids.end(), has_skylight_row)) return;
    if (join.roof_ids.size() > maximum_skylight_cohort_members)
        invalid("Roof skylight cohort member budget exceeded");
    validate_roof_join_semantics(join);
    const auto resolved = resolve_vertical_placements(actual_entities, join.roof_ids);
    std::vector<RoofObject> objects;
    objects.reserve(join.roof_ids.size());
    for (const auto& id : join.roof_ids) {
        const auto source = resolved.find(id);
        if (source == resolved.end() || source->second.id != id || source->second.type != "roof")
            invalid("Roof skylight join requires its actual roof members");
        objects.push_back(decode_roof_entity(source->second));
    }
    validate_roof_skylight_cohort(objects);
}

TopoDS_Shape make_roof_shape(const RoofObject& object) {
    const auto structure = make_roof_structure_shape(object);
    return std::visit([&](const auto& roof) -> TopoDS_Shape {
        if (std::none_of(roof.openings.begin(), roof.openings.end(),
            [](const auto& opening) { return opening.skylight.has_value(); })) return structure;
        try {
            TopoDS_Compound compound;
            BRep_Builder builder;
            builder.MakeCompound(compound);
            builder.Add(compound, structure);
            double expected_volume = solid_volume(structure);
            for (const auto& opening : roof.openings) {
                if (!opening.skylight) continue;
                const auto assembly = make_roof_skylight(roof, opening);
                expected_volume += solid_volume(assembly);
                builder.Add(compound, assembly);
            }
            const auto volume = solid_volume(compound);
            if (!BRepCheck_Analyzer(compound).IsValid() || !std::isfinite(volume) ||
                !std::isfinite(expected_volume) || volume <= 0.0 ||
                std::abs(volume - expected_volume) > expected_volume * 1e-7)
                invalid("Complete roof assembly is invalid");
            return compound;
        } catch (const Standard_Failure& error) {
            throw std::invalid_argument(std::string("Complete roof assembly failed: ") + error.what());
        }
    }, object);
}
} // namespace sketch

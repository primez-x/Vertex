#include "sketch/constraint_wall_edit.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/corner_window.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {
namespace {
using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
[[noreturn]] void invalid(std::string message) { throw std::invalid_argument(std::move(message)); }
double finite_number(const json& value, std::string_view description) {
    if (!value.is_number()) {
        invalid(std::string(description) + " must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        invalid(std::string(description) + " must be finite");
    }
    return result;
}

// Alias scalars are admitted only for an owner's reciprocal bare corner cut.
bool managed_corner_cut(const Entity& opening,
                        const std::map<std::string, Entity, std::less<>>& entities) {
    const auto& p = opening.properties;
    const auto owner_id = p.find("corner_window_id");
    const auto leg = p.find("corner_leg");
    if (owner_id == p.end() || !owner_id->is_string() || leg == p.end() ||
        !leg->is_number_integer() || (*leg != 0 && *leg != 1) ||
        p.value("opening_kind", json{}) != "opening" ||
        p.contains("opening_assembly") || p.contains("door_operation") ||
        p.contains("vertical_placement")) return false;
    const auto owner = entities.find(owner_id->get_ref<const std::string&>());
    if (owner == entities.end() || owner->second.id != owner->first ||
        owner->second.type != "corner_window" || !owner->second.properties.is_object())
        return false;
    const auto& owner_properties = owner->second.properties;
    const auto children = owner_properties.find("opening_ids");
    const auto walls = owner_properties.find("wall_ids");
    const auto version = owner_properties.find("version");
    const auto index = *leg == 0 ? 0 : 1;
    return version != owner_properties.end() && version->is_number_integer() && *version == 1 &&
        children != owner_properties.end() && children->is_array() && children->size() == 2 &&
        walls != owner_properties.end() && walls->is_array() && walls->size() == 2 &&
        (*children)[index] == opening.id && p.contains("wall_id") &&
        (*walls)[index] == p.at("wall_id");
}

double opening_scalar(const json& properties, const char* canonical,
                      const char* legacy, const char* description, bool managed) {
    if (!managed) return finite_number(properties.at(canonical), description);
    const auto current = properties.find(canonical);
    const auto old = properties.find(legacy);
    // Match document_wall's canonical-first numeric codec without mutating
    // retained descriptors; corner_window admission also requires exact equality.
    const auto result = finite_number(current != properties.end() ? *current :
                            properties.at(legacy), description);
    if (current != properties.end() && old != properties.end() &&
        finite_number(*old, description) != result)
        invalid(std::string(description) + " has contradictory corner cut aliases");
    return result;
}

Vec2 point(const json& value, std::string_view description) {
    if (!value.is_array() || value.size() != 2) {
        invalid(std::string(description) + " must contain exactly two coordinates");
    }
    return {finite_number(value.at(0), description), finite_number(value.at(1), description)};
}

Segment read_baseline(const Entity& entity) {
    if (entity.type != "wall") {
        invalid("Constraint owner is not a wall: " + entity.id);
    }
    if (!entity.properties.is_object()) {
        invalid("Wall properties must be an object: " + entity.id);
    }
    try {
        const auto& baseline = entity.properties.at("baseline");
        if (!baseline.is_object()) {
            invalid("Wall baseline must be an object: " + entity.id);
        }
        return {point(baseline.at("start"), "Wall start"),
                point(baseline.at("end"), "Wall end"),
                finite_number(baseline.at("sweep_radians"), "Wall sweep")};
    } catch (const std::out_of_range&) {
        invalid("Wall baseline is missing required geometry: " + entity.id);
    }
}

std::string unit_name(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return "m";
        case Unit::millimetre:
            return "mm";
        case Unit::centimetre:
            return "cm";
        case Unit::foot:
            return "ft";
        case Unit::inch:
            return "in";
    }
    invalid("Unknown quantity unit");
}

Quantity normalize_positive_quantity(const Quantity& value) {
    Quantity parsed;
    try {
        parsed = parse_quantity(value.original_expression, value.entered_unit);
    } catch (const std::exception&) {
        invalid("Wall length quantity is not exactly parseable");
    }
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit ||
        parsed.original_expression != value.original_expression || !(parsed.metres > 0.0) ||
        !std::isfinite(parsed.metres)) {
        invalid("Wall length quantity is internally inconsistent or non-positive");
    }
    return parsed;
}

void update_baseline_json(json& target, const Segment& baseline) {
    target["start"] = {baseline.start.x, baseline.start.y};
    target["end"] = {baseline.end.x, baseline.end.y};
    target["sweep_radians"] = baseline.sweep_radians;
}

void set_baseline(Entity& wall, const Segment& baseline,
                  const PlanarTransform* transform = nullptr) {
    if (const auto plane = wall.properties.find("top_plane"); plane != wall.properties.end()) {
        auto gradient = parse_wall_top_plane(*plane);
        if (transform) {
            // Gradients follow the same orthogonal XY basis as the baseline;
            // pivots and translations have no effect on a direction.
            const PlanarTransform basis{{0, 0}, transform->rotation_radians,
                transform->flip_horizontal, transform->flip_vertical, {0, 0}};
            gradient = transform_point(gradient, basis);
            wall.properties["top_plane"] = wall_top_plane_json(gradient);
        }
        const auto rise = gradient.x * (baseline.end.x - baseline.start.x) +
                          gradient.y * (baseline.end.y - baseline.start.y);
        if (!std::isfinite(rise)) invalid("Wall top plane rise exceeds the supported range");
        wall.properties["slope_rise_m"] = rise;
        if (wall.properties.contains("slope_rise")) wall.properties["slope_rise"] = rise;
    }
    update_baseline_json(wall.properties.at("baseline"), baseline);
}

bool same_wall_top_properties(const Entity& expected, const Entity& actual) {
    for (const auto* key : {"top_plane", "slope_rise_m", "slope_rise"}) {
        const auto left = expected.properties.find(key);
        const auto right = actual.properties.find(key);
        if ((left == expected.properties.end()) != (right == actual.properties.end()) ||
            (left != expected.properties.end() && *left != *right)) return false;
    }
    return true;
}

// Unknown members are opaque metadata. Updating recognized fields preserves
// them; invalidating a receipt containing them must fail closed rather than
// destroy data whose meaning this version cannot establish.
bool validate_length_receipt(const json& receipt, const Entity& wall) {
    if (!receipt.is_object() || !receipt.contains("version") ||
        !receipt.at("version").is_number_integer() ||
        (receipt.at("version") != 1 && receipt.at("version") != 2)) {
        invalid("Wall has unsupported last_length_entry extension metadata: " + wall.id);
    }
    try {
        const auto& expression = receipt.at("original_expression");
        const auto& entered_unit = receipt.at("entered_unit");
        if (!expression.is_string() || !entered_unit.is_string()) {
            invalid("Wall length receipt expression and unit must be strings");
        }
        std::optional<Unit> unit;
        for (const auto candidate : {Unit::metre, Unit::millimetre, Unit::centimetre,
                                     Unit::foot, Unit::inch}) {
            if (entered_unit == unit_name(candidate)) {
                unit = candidate;
                break;
            }
        }
        if (!unit) {
            invalid("Wall length receipt has an unsupported entered unit");
        }
        const auto quantity = parse_quantity(expression.get_ref<const std::string&>(), *unit);
        const auto& exact = receipt.at("exact_metres");
        if (quantity.entered_unit != *unit || !std::isfinite(quantity.metres) ||
            quantity.metres <= 0.0 || !exact.is_object() ||
            !exact.at("numerator").is_number_integer() ||
            !exact.at("denominator").is_number_integer() ||
            exact.at("numerator") != quantity.exact_metres.numerator ||
            exact.at("denominator") != quantity.exact_metres.denominator) {
            invalid("Wall length receipt has inconsistent exact quantity metadata");
        }
        auto receipt_wall = wall;
        receipt_wall.properties["baseline"] = receipt.at("baseline");
        const auto recorded = read_baseline(receipt_wall);
        const auto current = read_baseline(wall);
        // Receipts store the coordinates produced by the command, not a
        // measurement approximation; any subsequent coordinate change stales it.
        const bool curved=receipt.at("version")==2;
        if ((curved ? recorded.sweep_radians==0.0 : recorded.sweep_radians!=0.0) ||
            recorded.sweep_radians!=current.sweep_radians ||
            recorded.start.x != current.start.x || recorded.start.y != current.start.y ||
            recorded.end.x != current.end.x || recorded.end.y != current.end.y ||
            std::abs(segment_length(recorded) - quantity.metres) >
                constraint_linear_tolerance_metres) {
            invalid("Wall length receipt does not match its stored baseline");
        }
        return receipt.size() != 5 || exact.size() != 2 || receipt.at("baseline").size() != 3;
    } catch (const json::exception&) {
        invalid("Wall length receipt is missing or has malformed required metadata: " + wall.id);
    }
}

void validate_or_clear_length_receipt(Entity& wall, bool write_receipt,
                                      const Quantity* quantity, const Segment& baseline) {
    auto section = wall.extensions.find("constraint_authoring");
    if (section != wall.extensions.end()) {
        if (!section->is_object() || !section->contains("version") ||
            !section->at("version").is_number_integer() || section->at("version") != 1) {
            invalid("Wall has unsupported constraint_authoring extension metadata: " + wall.id);
        }
        const auto receipt = section->find("last_length_entry");
        if (receipt != section->end()) {
            const bool opaque_metadata = validate_length_receipt(*receipt, wall);
            if (!write_receipt && opaque_metadata) {
                invalid("Wall edit would discard unsupported last_length_entry metadata: " + wall.id);
            }
        }
    }
    if (write_receipt) {
        if (quantity == nullptr) {
            invalid("Wall length receipt is missing its exact quantity");
        }
        if (section == wall.extensions.end()) {
            wall.extensions["constraint_authoring"] = ordered_json{{"version", 1}};
            section = wall.extensions.find("constraint_authoring");
        }
        auto& receipt = (*section)["last_length_entry"];
        receipt["version"] = baseline.sweep_radians==0 ? 1 : 2;
        receipt["original_expression"] = quantity->original_expression;
        receipt["entered_unit"] = unit_name(quantity->entered_unit);
        receipt["exact_metres"]["numerator"] = quantity->exact_metres.numerator;
        receipt["exact_metres"]["denominator"] = quantity->exact_metres.denominator;
        update_baseline_json(receipt["baseline"], baseline);
    } else if (section != wall.extensions.end()) {
        section->erase("last_length_entry");
    }
}

}

void validate_wall_length_input(const Entity& wall) {
    if (const auto archive=wall.extensions.find("wall_scale_length_archive"); archive!=wall.extensions.end()) {
        if (!archive->is_object() || archive->size()!=2 || !archive->contains("version") ||
            archive->at("version")!=1 || !archive->contains("entries") || !archive->at("entries").is_array() ||
            archive->at("entries").empty() || archive->at("entries").size()>4096 || archive->dump().size()>1024*1024)
            invalid("Unsupported wall scale length archive: " + wall.id);
        for (const auto& entry : archive->at("entries")) {
            if (!entry.is_object() || entry.size()!=5 || !entry.contains("receipt") ||
                !entry.contains("source_baseline") || !entry.contains("baseline") || !entry.contains("pivot") ||
                !entry.contains("scale") || !entry.at("pivot").is_array() || entry.at("pivot").size()!=3)
                invalid("Malformed wall scale length archive: " + wall.id);
            const auto factor=finite_number(entry.at("scale"),"Archived wall scale");
            if (!(factor>0) || factor==1) invalid("Archived wall scale must change length");
            const Vec3 pivot{finite_number(entry.at("pivot").at(0),"Archived scale pivot"),
                finite_number(entry.at("pivot").at(1),"Archived scale pivot"),
                finite_number(entry.at("pivot").at(2),"Archived scale pivot")};
            auto original=wall; original.properties["baseline"]=entry.at("source_baseline");
            (void)validate_length_receipt(entry.at("receipt"),original);
            const auto old=read_baseline(original);
            const auto scale_point=[&](Vec2 p) { return Vec2{pivot.x+(p.x-pivot.x)*factor,pivot.y+(p.y-pivot.y)*factor}; };
            auto expected=original;
            update_baseline_json(expected.properties["baseline"],{scale_point(old.start),scale_point(old.end),old.sweep_radians});
            const auto derived=read_baseline(expected);
            if (!std::isfinite(segment_length(derived)) || segment_length(derived)<=constraint_linear_tolerance_metres)
                invalid("Wall scale archive contains a collapsed or unrepresentable baseline");
            if (expected.properties.at("baseline")!=entry.at("baseline"))
                invalid("Wall scale archive does not reconstruct its source baseline: " + wall.id);
        }
    }
    const auto section=wall.extensions.find("constraint_authoring");
    if (section==wall.extensions.end() || !section->is_object() ||
        !section->contains("version") || !section->at("version").is_number_integer() || section->at("version")!=1) return;
    const auto receipt=section->find("last_length_entry");
    if (receipt==section->end() || !receipt->is_object() || !receipt->contains("version") ||
        !receipt->at("version").is_number_integer() ||
        (receipt->at("version")!=1 && receipt->at("version")!=2)) return;
    (void)validate_length_receipt(*receipt,wall);
}

void archive_scaled_wall_length_receipt(Entity& wall,const Segment& transformed,Vec3 pivot,double scale) {
    validate_wall_length_input(wall);
    auto section=wall.extensions.find("constraint_authoring");
    if (section==wall.extensions.end()) return;
    if (!section->is_object() || !section->contains("version") || section->at("version")!=1)
        invalid("Cannot scale unsupported wall measurement metadata: " + wall.id);
    const auto receipt=section->find("last_length_entry");
    if (receipt==section->end()) return;
    (void)validate_length_receipt(*receipt,wall);
    auto result=wall;
    if (!result.extensions.contains("wall_scale_length_archive"))
        result.extensions["wall_scale_length_archive"]={{"version",1},{"entries",json::array()}};
    auto baseline=wall.properties.at("baseline"); update_baseline_json(baseline,transformed);
    result.extensions["wall_scale_length_archive"]["entries"].push_back({{"receipt",*receipt},
        {"source_baseline",wall.properties.at("baseline")},{"baseline",std::move(baseline)},
        {"pivot",json::array({pivot.x,pivot.y,pivot.z})},{"scale",scale}});
    result.extensions["constraint_authoring"].erase("last_length_entry");
    validate_wall_length_input(result);
    wall=std::move(result);
}

std::map<std::string,Entity,std::less<>> stage_wall_group_scale_entities(
    const std::map<std::string,Entity,std::less<>>& source,const WallGroupScaleIntent& raw) {
    const auto intent=decode_wall_group_scale_intent(encode_wall_group_scale_intent(raw));
    auto result=source;
    const auto scope=constraint_phase_scope(source);
    const auto scaled_scalar=[&](json& values,const char* field) {
        if (!values.contains(field)) return;
        const auto amount=finite_number(values.at(field),field)*intent.scale;
        if (!std::isfinite(amount)) invalid("Wall group scale exceeds supported numeric range");
        values[field]=amount;
    };
    std::map<std::string,double,std::less<>> scales;
    for (const auto& id : intent.wall_ids) {
        const auto found=source.find(id);
        if (found==source.end() || found->second.type!="wall" || found->second.id!=id)
            invalid("Wall group scale requires actual persisted walls");
        if (scope.inactive_owner_ids.contains(id)) invalid("Wall group scale cannot edit an inactive wall");
        std::vector<const Entity*> openings;
        for (const auto& [child_id,child] : source) {
            (void)child_id;
            if (!scope.inactive_owner_ids.contains(child_id) &&
                (child.type=="opening" || child.type=="door" || child.type=="window") &&
                child.properties.contains("wall_id") && child.properties.at("wall_id")==id) openings.push_back(&child);
        }
        Wall wall; std::string error;
        if (!read_document_wall(found->second,openings,wall,error)) invalid(error);
        validate_wall_semantics(wall);
        const auto effective=resolve_vertical_placement(source,found->second);
        const auto elevation=[](const Entity& entity) {
            return finite_number(entity.properties.at(entity.properties.contains("elevation_m") ? "elevation_m" : "elevation"),"Wall elevation");
        };
        const auto shift=elevation(effective)-wall.elevation;
        const auto scale_point=[&](Vec2 p) { return Vec2{intent.pivot.x+(p.x-intent.pivot.x)*intent.scale,
            intent.pivot.y+(p.y-intent.pivot.y)*intent.scale}; };
        const Segment baseline{scale_point(wall.baseline.start),scale_point(wall.baseline.end),wall.baseline.sweep_radians};
        auto& entity=result.at(id);
        archive_scaled_wall_length_receipt(entity,baseline,intent.pivot,intent.scale);
        if (entity.extensions.contains("curve_input")) rebase_wall_curve_input(entity,baseline);
        set_baseline(entity,baseline);
        for (const auto* key : {"thickness_m","thickness","height_m","height"}) scaled_scalar(entity.properties,key);
        entity.properties["thickness_m"]=wall.thickness*intent.scale;
        entity.properties["height_m"]=wall.height*intent.scale;
        const double raw_elevation=intent.pivot.z+(wall.elevation+shift-intent.pivot.z)*intent.scale-shift;
        if (!std::isfinite(raw_elevation)) invalid("Wall scale elevation exceeds supported numeric range");
        entity.properties["elevation_m"]=raw_elevation;
        if (entity.properties.contains("elevation")) entity.properties["elevation"]=raw_elevation;
        // set_baseline already derives the rise from the unchanged gradient
        // and the scaled chord when a top plane is present.
        if (!entity.properties.contains("top_plane"))
            for (const auto* key : {"slope_rise_m","slope_rise"}) scaled_scalar(entity.properties,key);
        if (entity.properties.contains("layers"))
            for (auto& layer : entity.properties.at("layers")) scaled_scalar(layer,"thickness_m");
        if (entity.properties.contains("transform")) invalid("Wall scale cannot borrow opaque legacy transform metadata");
        for (const auto* child : openings) {
            if (child->properties.contains("corner_window_id")) continue;
            auto& p=result.at(child->id).properties;
            for (const auto* key : {"offset_m","offset","width_m","width","sill_m","sill","height_m","height"}) scaled_scalar(p,key);
            if (p.contains("opening_assembly")) {
                (void)parse_opening_assembly(p.at("opening_assembly"));
                for (const auto* key : {"frame_width_m","frame_depth_m","panel_thickness_m","glazing_thickness_m","inset_m","window_bay_projection_m","window_bow_projection_m"})
                    scaled_scalar(p.at("opening_assembly"),key);
                (void)parse_opening_assembly(p.at("opening_assembly"));
            }
        }
        validate_wall_curve_input(entity);
        validate_wall_length_input(entity);
        scales.emplace(id,intent.scale);
    }
    complete_corner_window_geometry(source,result,scales);
    for (auto& [id,entity] : result) {
        const auto& before=source.at(id);
        if (entity==before || !before.properties.contains("quantity_entries")) continue;
        if (before.type=="corner_window" || before.properties.contains("corner_window_id")) {
            // Coordinated corner replay owns its receipt dialect and exact
            // retained envelope. Preserve original measurements on an actual
            // scaled host without modifying that owner/cut envelope.
            const auto host=before.type=="corner_window" ? parse_corner_window(before).wall_ids[0] :
                before.properties.at("wall_id").get<std::string>();
            if (!scales.contains(host)) invalid("Corner scale receipt lacks a selected actual host");
            auto& archive=result.at(host).extensions["wall_scale_hosted_quantity_archive"];
            if (archive.is_null()) archive={{"version",1},{"entries",json::array()}};
            if (!archive.is_object() || archive.size()!=2 || !archive.contains("version") || archive.at("version")!=1 ||
                !archive.contains("entries") || !archive.at("entries").is_array() || archive.at("entries").size()>=4096)
                invalid("Unsupported hosted wall scale quantity archive");
            archive["entries"].push_back({{"source_owner_id",id},{"source_properties",before.properties},
                {"pivot",json::array({intent.pivot.x,intent.pivot.y,intent.pivot.z})},{"scale",intent.scale}});
            if (archive.dump().size()>1024*1024) invalid("Hosted scale quantity archive exceeds its budget");
            continue;
        }
        const auto original=before.properties.at("quantity_entries");
        if (!original.is_object() || !entity.properties.contains("quantity_entries") || !entity.properties.at("quantity_entries").is_object())
            invalid("Wall scale cannot discard opaque quantity metadata");
        for (const auto& [pointer,receipt] : original.items()) {
            (void)receipt;
            try {
                const json::json_pointer path(pointer);
                if (before.properties.contains(path) && (!entity.properties.contains(path) || before.properties.at(path)!=entity.properties.at(path)))
                    entity.properties.at("quantity_entries").erase(pointer);
            } catch (const json::exception&) { /* Opaque paths retain their exact payload. */ }
        }
        if (entity.properties.at("quantity_entries")==original) continue;
        auto& archive=entity.extensions["wall_scale_quantity_archive"];
        if (archive.is_null()) archive={{"version",1},{"entries",json::array()}};
        if (!archive.is_object() || archive.size()!=2 || !archive.contains("version") || archive.at("version")!=1 ||
            !archive.contains("entries") || !archive.at("entries").is_array() || archive.at("entries").size()>=4096)
            invalid("Unsupported wall scale quantity archive");
        archive["entries"].push_back({{"source_properties",before.properties},
            {"pivot",json::array({intent.pivot.x,intent.pivot.y,intent.pivot.z})},{"scale",intent.scale}});
        if (archive.dump().size()>1024*1024) invalid("Wall scale quantity archive exceeds its budget");
    }
    return result;
}

void clear_wall_length_input(Entity& wall) {
    auto candidate=wall;
    validate_or_clear_length_receipt(candidate,false,nullptr,read_baseline(wall));
    if (candidate.extensions.contains("constraint_authoring"))
        wall.extensions["constraint_authoring"]=std::move(candidate.extensions["constraint_authoring"]);
}

void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline) {
    auto section = wall.extensions.find("constraint_authoring");
    if (section == wall.extensions.end()) {
        return;
    }
    if (!section->is_object() || !section->contains("version") ||
        !section->at("version").is_number_integer() || section->at("version") != 1) {
        invalid("Wall has unsupported constraint_authoring extension metadata: " + wall.id);
    }
    auto receipt = section->find("last_length_entry");
    if (receipt == section->end()) {
        return;
    }
    (void)validate_length_receipt(*receipt, wall);
    const auto original = read_baseline(wall);
    const auto& transformed = transformed_baseline;
    const auto original_length = segment_length(original);
    const auto transformed_length = segment_length(transformed);
    if (std::abs(transformed.sweep_radians) != std::abs(original.sweep_radians) ||
        !std::isfinite(transformed.start.x) || !std::isfinite(transformed.start.y) ||
        !std::isfinite(transformed.end.x) || !std::isfinite(transformed.end.y) ||
        !std::isfinite(original_length) || !std::isfinite(transformed_length) ||
        transformed_length <= 0.0 ||
        std::abs(transformed_length - original_length) > constraint_linear_tolerance_metres) {
        invalid("Wall length receipt requires a finite length-preserving transform with unchanged sweep magnitude: " + wall.id);
    }
    auto updated = *receipt;
    update_baseline_json(updated.at("baseline"), transformed);
    auto transformed_wall = wall;
    set_baseline(transformed_wall, transformed);
    (void)validate_length_receipt(updated, transformed_wall);
    receipt->swap(updated);
}

namespace {
json baseline_json(const Segment& baseline) {
    json result=json::object(); update_baseline_json(result,baseline); return result;
}
bool same_baseline(const Segment& a,const Segment& b) {
    return a.start.x==b.start.x && a.start.y==b.start.y && a.end.x==b.end.x && a.end.y==b.end.y &&
        a.sweep_radians==b.sweep_radians;
}
void validate_input(const json& input,const Segment& baseline,const std::string& id) {
    if (!input.is_object() || !input.contains("version") || !input.at("version").is_number_integer() ||
        (input.at("version")!=1 && input.at("version")!=2) || !input.contains("start") ||
        !input.contains("end") || !input.contains("radians") ||
        input.at("start")!=json::array({baseline.start.x,baseline.start.y}) ||
        input.at("end")!=json::array({baseline.end.x,baseline.end.y}) || input.at("radians")!=baseline.sweep_radians)
        invalid("Wall has unsupported or stale curve input provenance: "+id);
    if (input.at("version")==2) {
        const auto construction=input.at("construction").get<std::string>();
        const auto measure=input.at("measure").get<std::string>();
        const auto value=finite_number(input.at("measure_value"),"Curve measure");
        Segment reconstructed;
        if (construction=="angle") {
            const auto angle=parse_angle(measure);
            if (std::abs(angle.radians-value)>constraint_angular_tolerance_radians)
                invalid("Curve angle receipt has inconsistent measure");
            reconstructed=arc_from_chord_angle(baseline.start,baseline.end,value);
        } else {
            bool matches=false;
            for (const auto unit : {Unit::metre,Unit::foot}) {
                try { matches=matches || parse_quantity(measure,unit).metres==value; }
                catch (const std::exception&) {}
            }
            if (!matches) invalid("Curve receipt has inconsistent exact measure");
            if (construction=="arc_length")
                reconstructed=arc_from_chord_arc_length(baseline.start,baseline.end,std::abs(value),input.at("clockwise").get<bool>());
            else if (construction=="arc_height") reconstructed=arc_from_chord_height(baseline.start,baseline.end,value);
            else invalid("Wall has unsupported curve construction: "+id);
        }
        if (std::abs(reconstructed.sweep_radians-baseline.sweep_radians)>constraint_angular_tolerance_radians)
            invalid("Curve construction receipt does not reproduce its baseline: "+id);
    }
}
constexpr std::size_t curve_edit_byte_limit=1024*1024;
void validate_curve_edit_size(const json& value) {
    if (value.dump().size()>curve_edit_byte_limit)
        invalid("Wall curve construction proof exceeds the supported byte limit");
}
bool understood_curve_input_field(const std::string& key) {
    for (const auto* field : {"version","construction","start","end","measure",
            "normalized_measure","measure_value","clockwise","sweep","normalized_sweep","radians"})
        if (key==field) return true;
    return false;
}
void validate_preserved_curve_metadata(const json* source,const json& input) {
    if (source) {
        for (const auto& [key,value] : source->items())
            if (!understood_curve_input_field(key) &&
                (!input.contains(key) || input.at(key).dump()!=value.dump()))
                invalid("Wall curve construction cannot discard or replace opaque input metadata");
    }
    for (const auto& [key,value] : input.items()) {
        (void)value;
        if (!understood_curve_input_field(key) && (!source || !source->contains(key)))
            invalid("Wall curve construction cannot introduce opaque input metadata");
    }
}
Segment explicit_curve_baseline(const json& input,const std::string& id) {
    validate_curve_edit_size(input);
    if (!input.is_object() || !input.contains("version") ||
        !input.at("version").is_number_integer() || input.at("version")!=2)
        invalid("Explicit wall curve construction requires version two input");
    for (const auto* key : {"construction","measure","normalized_measure","sweep","normalized_sweep"})
        if (!input.contains(key) || !input.at(key).is_string() ||
            input.at(key).get_ref<const std::string&>().empty())
            invalid("Explicit wall curve construction requires its entered and normalized expressions");
    if (!input.contains("clockwise") || !input.at("clockwise").is_boolean())
        invalid("Explicit wall curve construction requires its clockwise flag");
    const auto start=point(input.at("start"),"Curve construction start");
    const auto end=point(input.at("end"),"Curve construction end");
    const auto value=finite_number(input.at("measure_value"),"Curve construction measure");
    const auto& construction=input.at("construction").get_ref<const std::string&>();
    const auto& measure=input.at("measure").get_ref<const std::string&>();
    const auto& normalized=input.at("normalized_measure").get_ref<const std::string&>();
    Segment expected;
    if (construction=="angle") {
        if (parse_angle(measure).radians!=value || parse_angle(normalized).radians!=value)
            invalid("Explicit wall curve angle expressions do not reproduce their exact value");
        expected=arc_from_chord_angle(start,end,value);
    } else if (construction=="arc_length" || construction=="arc_height") {
        bool matches=false;
        for (const auto unit : {Unit::metre,Unit::foot}) {
            try { matches=matches || parse_quantity(measure,unit).metres==value; }
            catch (const std::exception&) {}
        }
        if (!matches || parse_quantity(normalized,Unit::metre).metres!=value)
            invalid("Explicit wall curve length expressions do not reproduce their signed value");
        expected=construction=="arc_length" ?
            arc_from_chord_arc_length(start,end,std::abs(value),input.at("clockwise").get<bool>()) :
            arc_from_chord_height(start,end,value);
    } else invalid("Explicit wall curve construction has an unsupported kind");
    if (finite_number(input.at("radians"),"Curve construction sweep")!=expected.sweep_radians ||
        parse_angle(input.at("sweep").get_ref<const std::string&>()).radians!=expected.sweep_radians ||
        parse_angle(input.at("normalized_sweep").get_ref<const std::string&>()).radians!=expected.sweep_radians)
        invalid("Explicit wall curve sweep expressions do not reproduce their exact baseline");
    validate_input(input,expected,id);
    return expected;
}
json derived_angle_input(const json& source,const Segment& baseline) {
    auto result=source;
    const auto angle=angle_from_radians(baseline.sweep_radians);
    result["version"]=2; result["construction"]="angle";
    result["measure"]=angle.original_expression; result["normalized_measure"]=angle.normalized_expression;
    result["measure_value"]=baseline.sweep_radians; result["clockwise"]=baseline.sweep_radians<0;
    result["sweep"]=angle.original_expression; result["normalized_sweep"]=angle.normalized_expression;
    result["radians"]=baseline.sweep_radians;
    result["start"]={baseline.start.x,baseline.start.y}; result["end"]={baseline.end.x,baseline.end.y};
    return result;
}
json encode_rigid_transform(const PlanarTransform& transform) {
    return {{"version",1},{"pivot",{transform.pivot.x,transform.pivot.y}},
        {"rotation_radians",transform.rotation_radians},{"flip_horizontal",transform.flip_horizontal},
        {"flip_vertical",transform.flip_vertical},{"offset",{transform.offset.x,transform.offset.y}}};
}
PlanarTransform decode_rigid_transform(const json& value) {
    if (!value.is_object() || value.size()!=6 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version")!=1 ||
        !value.contains("flip_horizontal") || !value.at("flip_horizontal").is_boolean() ||
        !value.contains("flip_vertical") || !value.at("flip_vertical").is_boolean())
        invalid("Wall curve rigid transform has unsupported fields");
    return {point(value.at("pivot"),"Curve transform pivot"),
        finite_number(value.at("rotation_radians"),"Curve transform rotation"),
        value.at("flip_horizontal").get<bool>(),value.at("flip_vertical").get<bool>(),
        point(value.at("offset"),"Curve transform offset")};
}
bool is_rigid_operation(const json& operation) {
    // Legacy raw baselines accept opaque members, including a vendor's kind.
    // Their geometry keys distinguish them from the new operation envelope.
    return operation.is_object() && operation.contains("kind") && !operation.contains("start");
}
Segment rigid_curve_baseline(const Segment& source,const PlanarTransform& transform) {
    const auto result=transform_segment(source,transform);
    (void)arc_from_chord_angle(result.start,result.end,result.sweep_radians);
    const auto original_length=segment_length(source), length=segment_length(result);
    if (!std::isfinite(original_length) || !std::isfinite(length) || length<=0 ||
        std::abs(length-original_length)>constraint_linear_tolerance_metres ||
        std::abs(result.sweep_radians)!=std::abs(source.sweep_radians))
        invalid("Wall curve transform must preserve finite physical length and sweep magnitude");
    return result;
}
Segment rigid_straight_baseline(const Segment& source,const PlanarTransform& transform) {
    const auto result=transform_segment(source,transform);
    const auto original_length=segment_length(source), length=segment_length(result);
    if (source.sweep_radians!=0.0 || result.sweep_radians!=0.0 ||
        !std::isfinite(source.start.x) || !std::isfinite(source.start.y) ||
        !std::isfinite(source.end.x) || !std::isfinite(source.end.y) ||
        !std::isfinite(result.start.x) || !std::isfinite(result.start.y) ||
        !std::isfinite(result.end.x) || !std::isfinite(result.end.y) ||
        !std::isfinite(original_length) || !std::isfinite(length) ||
        original_length<=constraint_linear_tolerance_metres ||
        length<=constraint_linear_tolerance_metres ||
        std::abs(length-original_length)>constraint_linear_tolerance_metres)
        invalid("Straight wall transform must preserve finite nondegenerate physical length");
    return result;
}
json rigid_curve_input(const json& source,const Segment& baseline,const PlanarTransform& transform) {
    if (transform.flip_horizontal!=transform.flip_vertical) return derived_angle_input(source,baseline);
    auto result=source;
    result["start"]={baseline.start.x,baseline.start.y};
    result["end"]={baseline.end.x,baseline.end.y};
    result["radians"]=baseline.sweep_radians;
    return result;
}
}

void validate_wall_curve_input(const Entity& wall) {
    const auto input=wall.extensions.find("curve_input");
    const auto derivation=wall.extensions.find("curve_input_derivation");
    if (input==wall.extensions.end()) {
        if (derivation!=wall.extensions.end()) invalid("Curve derivation requires its active input: "+wall.id);
        return;
    }
    const auto current=read_baseline(wall);
    validate_input(*input,current,wall.id);
    if (derivation==wall.extensions.end()) return;
    const auto& proof=*derivation;
    if (!proof.is_object() || proof.size()!=4 ||
        (proof.at("version")!=1 && proof.at("version")!=2 && proof.at("version")!=3) ||
        (proof.at("version")!=1 && !proof.at("version").is_number_integer()) ||
        !proof.at("operations").is_array() || proof.at("operations").empty())
        invalid("Wall has unsupported curve input derivation: "+wall.id);
    auto source=wall; source.properties["baseline"]=proof.at("source_baseline");
    auto baseline=read_baseline(source);
    const bool line_origin=proof.at("version")==3;
    if (line_origin) {
        if (!proof.at("source_input").is_null() || baseline.sweep_radians!=0 ||
            !std::isfinite(segment_length(baseline)) || segment_length(baseline)<=constraint_linear_tolerance_metres)
            invalid("Line-origin curve derivation requires its finite original straight baseline and null input");
    } else validate_input(proof.at("source_input"),baseline,wall.id);
    auto expected_input=proof.at("source_input");
    bool has_rigid=false;
    bool first=true;
    for (const auto& operation : proof.at("operations")) {
        if (is_rigid_operation(operation)) {
            if ((proof.at("version")!=2 && !line_origin) || (line_origin && first) ||
                operation.size()!=3 || operation.at("kind")!="rigid_transform" ||
                !operation.contains("transform") || !operation.contains("baseline"))
                invalid("Wall curve derivation has an unsupported rigid operation");
            const auto transform=decode_rigid_transform(operation.at("transform"));
            const auto expected=rigid_curve_baseline(baseline,transform);
            source.properties["baseline"]=operation.at("baseline");
            const auto next=read_baseline(source);
            if (!same_baseline(expected,next) || same_baseline(baseline,next))
                invalid("Wall curve rigid operation does not reproduce its changed baseline");
            expected_input=rigid_curve_input(expected_input,next,transform);
            validate_input(expected_input,next,wall.id);
            baseline=next; has_rigid=true; first=false;
            continue;
        }
        const bool construction=operation.contains("input");
        if (construction && (operation.size()!=2 || !operation.contains("baseline")))
            invalid("Curve reconstruction operation contains unexpected fields");
        source.properties["baseline"]=construction ? operation.at("baseline") : operation;
        const auto next=read_baseline(source);
        if ((!construction && next.sweep_radians!=baseline.sweep_radians) || next.sweep_radians==0)
            invalid("Curve derivation changed its signed sweep");
        (void)arc_from_chord_angle(next.start,next.end,next.sweep_radians);
        if (construction) { validate_input(operation.at("input"),next,wall.id); expected_input=operation.at("input"); }
        else expected_input=derived_angle_input(expected_input,next);
        if (line_origin && first && (!construction ||
            expected_input!=derived_angle_input(json::object(),next)))
            invalid("Line-origin curve derivation must begin with its canonical derived physical angle input");
        baseline=next;
        first=false;
    }
    if (proof.at("version")==2 && !has_rigid)
        invalid("Wall curve derivation version two requires a rigid transform");
    if (!same_baseline(baseline,current) || *input!=expected_input)
        invalid("Curve derivation does not reproduce its active geometry/input: "+wall.id);
}

void transform_wall_curve_input(Entity& wall,const PlanarTransform& transform) {
    validate_wall_curve_input(wall);
    const auto input=wall.extensions.find("curve_input");
    if (input==wall.extensions.end()) return;
    const auto original=read_baseline(wall);
    const auto transformed=rigid_curve_baseline(original,transform);
    if (same_baseline(original,transformed)) return;
    auto candidate=wall;
    auto proof=candidate.extensions.find("curve_input_derivation");
    if (proof==candidate.extensions.end()) {
        candidate.extensions["curve_input_derivation"]={{"version",2},{"source_input",*input},
            {"source_baseline",wall.properties.at("baseline")},{"operations",json::array()}};
        proof=candidate.extensions.find("curve_input_derivation");
    } else if (proof->at("version")!=3) (*proof)["version"]=2;
    auto recorded_baseline=wall.properties.at("baseline");
    update_baseline_json(recorded_baseline,transformed);
    (*proof)["operations"].push_back({{"kind","rigid_transform"},
        {"transform",encode_rigid_transform(transform)},{"baseline",recorded_baseline}});
    candidate.extensions["curve_input"]=rigid_curve_input(*input,transformed,transform);
    set_baseline(candidate,transformed,&transform);
    validate_wall_curve_input(candidate);
    // Publish only after the entire detached reconstruction has validated.
    wall.extensions["curve_input"]=std::move(candidate.extensions["curve_input"]);
    wall.extensions["curve_input_derivation"]=std::move(candidate.extensions["curve_input_derivation"]);
}

void rebase_wall_curve_input(Entity& wall,const Segment& transformed) {
    validate_wall_curve_input(wall);
    auto input=wall.extensions.find("curve_input");
    if (input==wall.extensions.end()) return;
    const auto old=read_baseline(wall);
    if (old.sweep_radians!=transformed.sweep_radians)
        invalid("Endpoint editing must preserve its exact signed sweep: "+wall.id);
    (void)arc_from_chord_angle(transformed.start,transformed.end,transformed.sweep_radians);
    const auto chord=[](const Segment& b) { return std::hypot(b.end.x-b.start.x,b.end.y-b.start.y); };
    const bool deformed=std::abs(chord(old)-chord(transformed))>constraint_linear_tolerance_metres;
    auto proof=wall.extensions.find("curve_input_derivation");
    const bool measured=input->at("version")==2 && input->at("construction")!="angle";
    if (proof!=wall.extensions.end() || (deformed && measured)) {
        if (proof==wall.extensions.end()) {
            wall.extensions["curve_input_derivation"]={{"version",1},{"source_input",*input},
                {"source_baseline",wall.properties.at("baseline")},{"operations",json::array()}};
            proof=wall.extensions.find("curve_input_derivation");
            input=wall.extensions.find("curve_input");
        }
        if (!deformed && measured) {
            auto rebased=*input; rebased["start"]={transformed.start.x,transformed.start.y};
            rebased["end"]={transformed.end.x,transformed.end.y};
            (*proof)["operations"].push_back({{"baseline",baseline_json(transformed)},{"input",rebased}});
            *input=std::move(rebased);
        } else {
            (*proof)["operations"].push_back(baseline_json(transformed));
            *input=derived_angle_input(*input,transformed);
        }
    } else {
        (*input)["start"]={transformed.start.x,transformed.start.y};
        (*input)["end"]={transformed.end.x,transformed.end.y};
    }
}

void preserve_wall_curve_construction(Entity& candidate,const Entity& source) {
    if (!source.extensions.contains("curve_input_derivation")) return;
    validate_wall_curve_input(source);
    if (candidate.id!=source.id || candidate.type!="wall") invalid("Curve construction owner identity changed");
    const auto next=read_baseline(candidate);
    validate_input(candidate.extensions.at("curve_input"),next,candidate.id);
    candidate.extensions["curve_input_derivation"]=source.extensions.at("curve_input_derivation");
    candidate.extensions["curve_input_derivation"]["operations"].push_back(
        {{"baseline",candidate.properties.at("baseline")},{"input",candidate.extensions.at("curve_input")}});
    validate_wall_curve_input(candidate);
}

Entity reconstruct_exterior_corner_wall(const Entity& source, const Segment& baseline) {
    validate_wall_curve_input(source);
    validate_wall_length_input(source);
    const auto old = read_baseline(source);
    if (same_baseline(old, baseline)) return source;
    if ((old.sweep_radians == 0) != (baseline.sweep_radians == 0) ||
        !std::isfinite(segment_length(baseline)) ||
        segment_length(baseline) <= constraint_linear_tolerance_metres)
        invalid("Exterior corner reconstruction changed analytical wall kind or collapsed its baseline");
    auto result = source;
    // Preserve known length receipts only when their physical measure survives.
    if (std::abs(segment_length(old) - segment_length(baseline)) > constraint_linear_tolerance_metres)
        clear_wall_length_input(result);
    else if (old.sweep_radians == baseline.sweep_radians) rebase_wall_length_receipt(result, baseline);
    else if (result.extensions.contains("constraint_authoring")) {
        auto& section = result.extensions["constraint_authoring"];
        if (!section.is_object() || !section.contains("version") ||
            !section.at("version").is_number_integer() || section.at("version") != 1)
            invalid("Exterior corner cannot rewrite unsupported constraint_authoring metadata: " + source.id);
        if (section.contains("last_length_entry")) {
            (void)validate_length_receipt(section.at("last_length_entry"),source);
            update_baseline_json(section["last_length_entry"]["baseline"], baseline);
        }
    }
    if (old.sweep_radians != 0 && source.extensions.contains("curve_input")) {
        (void)arc_from_chord_angle(baseline.start, baseline.end, baseline.sweep_radians);
        if (!result.extensions.contains("curve_input_derivation"))
            result.extensions["curve_input_derivation"] = {{"version", 1},
                {"source_input", source.extensions.at("curve_input")},
                {"source_baseline", source.properties.at("baseline")}, {"operations", json::array()}};
        const auto input = derived_angle_input(source.extensions.at("curve_input"), baseline);
        auto recorded = source.properties.at("baseline");
        update_baseline_json(recorded, baseline);
        result.extensions["curve_input_derivation"]["operations"].push_back(
            {{"baseline", recorded}, {"input", input}});
        result.extensions["curve_input"] = input;
    }
    set_baseline(result, baseline);
    validate_wall_curve_input(result);
    validate_wall_length_input(result);
    return result;
}

Entity reconstruct_exterior_segment_arc_wall(const Entity& source,const Segment& baseline) {
    const auto old=read_baseline(source);
    if (old.sweep_radians!=0 || baseline.sweep_radians==0)
        return reconstruct_exterior_corner_wall(source,baseline);
    validate_wall_curve_input(source);
    validate_wall_length_input(source);
    if (source.extensions.contains("curve_input") || source.extensions.contains("curve_input_derivation"))
        invalid("Straight-origin arc reconstruction cannot discard existing curve provenance: "+source.id);
    if (!std::isfinite(segment_length(baseline)) || segment_length(baseline)<=constraint_linear_tolerance_metres)
        invalid("Measured arc reconstruction collapsed its physical baseline: "+source.id);
    (void)arc_from_chord_angle(baseline.start,baseline.end,baseline.sweep_radians);
    auto result=source;
    clear_wall_length_input(result);
    const auto input=derived_angle_input(json::object(),baseline);
    auto recorded=source.properties.at("baseline");
    update_baseline_json(recorded,baseline);
    result.extensions["curve_input_derivation"]={{"version",3},{"source_input",nullptr},
        {"source_baseline",source.properties.at("baseline")},
        {"operations",json::array({json{{"baseline",recorded},{"input",input}}})}};
    result.extensions["curve_input"]=input;
    set_baseline(result,baseline);
    validate_wall_curve_input(result);
    validate_wall_length_input(result);
    return result;
}

void validate_wall_split_archive(const Entity& wall) {
    const auto archive=wall.extensions.find("wall_split_archive");
    if(archive==wall.extensions.end())return;
    if(!archive->is_object() || archive->size()!=2 || !archive->contains("version") ||
        archive->at("version")!=1 || !archive->contains("pieces") || !archive->at("pieces").is_array() ||
        archive->at("pieces").empty() || archive->at("pieces").size()>10000)
        invalid("Unsupported wall split archive: "+wall.id);
    for(const auto& piece:archive->at("pieces")) {
        if(!piece.is_object() || piece.size()!=6 || !piece.contains("source_wall_id") ||
            !piece.contains("source_baseline") || !piece.contains("fraction") || !piece.contains("second_piece") ||
            !piece.contains("partition_baseline") || !piece.contains("length_entry") ||
            !piece.at("source_wall_id").is_string() || piece.at("source_wall_id").get_ref<const std::string&>().empty() ||
            !piece.at("second_piece").is_boolean())
            invalid("Malformed wall split archive: "+wall.id);
        auto source=wall; source.properties["baseline"]=piece.at("source_baseline");
        const auto old=read_baseline(source);
        auto partition=wall; partition.properties["baseline"]=piece.at("partition_baseline");
        const auto child=read_baseline(partition);
        const auto fraction=finite_number(piece.at("fraction"),"Archived split fraction");
        if(!(fraction>0 && fraction<1))invalid("Archived split fraction is not interior: "+wall.id);
        const bool second=piece.at("second_piece").get<bool>();
        const auto ratio=second ? 1-fraction : fraction;
        Vec2 seam{std::lerp(old.start.x,old.end.x,fraction),std::lerp(old.start.y,old.end.y,fraction)};
        if(old.sweep_radians!=0) {
            const auto dx=old.end.x-old.start.x,dy=old.end.y-old.start.y,k=0.5/std::tan(old.sweep_radians/2);
            const Vec2 center{old.start.x+dx/2-dy*k,old.start.y+dy/2+dx*k};
            const auto angle=old.sweep_radians*fraction,x=old.start.x-center.x,y=old.start.y-center.y;
            seam={center.x+x*std::cos(angle)-y*std::sin(angle),center.y+x*std::sin(angle)+y*std::cos(angle)};
        }
        const Segment expected=second ? Segment{seam,old.end,old.sweep_radians*ratio} : Segment{old.start,seam,old.sweep_radians*ratio};
        if(!same_baseline(child,expected))invalid("Archived wall split does not reconstruct its directed child: "+wall.id);
        if(std::abs(segment_length(child)-segment_length(old)*ratio)>constraint_linear_tolerance_metres ||
            child.sweep_radians!=old.sweep_radians*ratio)
            invalid("Archived wall split does not partition its source measure: "+wall.id);
        if(!piece.at("length_entry").is_null())(void)validate_length_receipt(piece.at("length_entry"),source);
    }
}

Entity reconstruct_split_wall(const Entity& source,const Segment& baseline,double fraction,bool second_piece) {
    validate_wall_split_archive(source);
    json receipt=nullptr;
    if(const auto section=source.extensions.find("constraint_authoring");section!=source.extensions.end()) {
        if(!section->is_object() || !section->contains("version") || section->at("version")!=1)
            invalid("Wall split cannot archive unsupported constraint_authoring metadata: "+source.id);
        if(section->contains("last_length_entry"))receipt=section->at("last_length_entry");
    }
    auto result=reconstruct_exterior_corner_wall(source,baseline);
    if(!result.extensions.contains("wall_split_archive"))
        result.extensions["wall_split_archive"]={{"version",1},{"pieces",json::array()}};
    result.extensions["wall_split_archive"]["pieces"].push_back({{"source_wall_id",source.id},
        {"source_baseline",source.properties.at("baseline")},{"fraction",fraction},{"second_piece",second_piece},
        {"partition_baseline",result.properties.at("baseline")},{"length_entry",std::move(receipt)}});
    validate_wall_split_archive(result);
    return result;
}

void validate_constraint_wall_geometry_transition(const std::map<std::string,Entity,std::less<>>& before,
    const std::map<std::string,Entity,std::less<>>& after,bool qualified,bool qualified_line_origin,
    const std::set<std::string,std::less<>>& curve_construction_owner_ids) {
    for (const auto& id : curve_construction_owner_ids)
        if (!qualified || !before.contains(id) || before.at(id).type!="wall" ||
            !after.contains(id) || after.at(id).type!="wall")
            invalid("Explicit wall curve construction requires its qualified existing owner: "+id);
    for (const auto& [id,source] : before) {
        const auto found=after.find(id);
        if (source.type!="wall" || found==after.end() || found->second.type!="wall") continue;
        if(source.extensions.contains("wall_merge_archive") &&
            (!found->second.extensions.contains("wall_merge_archive") ||
                source.extensions.at("wall_merge_archive")!=found->second.extensions.at("wall_merge_archive")))
            invalid("Wall edit cannot discard or rewrite its merged source archive: "+id);
        if(!source.extensions.contains("wall_merge_archive") && found->second.extensions.contains("wall_merge_archive"))
            invalid("Wall merge archive requires a typed source reconstruction: "+id);
        if(source.extensions.contains("wall_split_archive") &&
            (!found->second.extensions.contains("wall_split_archive") ||
                source.extensions.at("wall_split_archive")!=found->second.extensions.at("wall_split_archive")))
            invalid("Wall edit cannot discard or rewrite its split input archive: "+id);
        if(!source.extensions.contains("wall_split_archive") && found->second.extensions.contains("wall_split_archive"))
            invalid("Wall split archive requires a typed source reconstruction: "+id);
        if (curve_construction_owner_ids.contains(id)) {
            const auto& candidate=found->second;
            if (!candidate.extensions.contains("curve_input"))
                invalid("Explicit wall curve construction requires its retained input: "+id);
            ConstraintWallGeometryEdit edit{id,read_baseline(candidate),std::nullopt};
            edit.version=6;
            edit.curve_construction=candidate.extensions.at("curve_input");
            const auto classification=candidate.properties.find("classification");
            if (classification!=candidate.properties.end() &&
                (!source.properties.contains("classification") ||
                    source.properties.at("classification").dump()!=classification->dump())) {
                if (!classification->is_string())
                    invalid("Explicit wall curve construction classification must be a string: "+id);
                edit.wall_classification=classification->get<std::string>();
            }
            const auto expected=replay_constraint_wall_edit(source,edit);
            if (candidate.id!=expected.id || candidate.required!=expected.required ||
                candidate.properties.dump()!=expected.properties.dump() ||
                candidate.extensions.dump()!=expected.extensions.dump())
                invalid("Explicit wall curve construction differs from its exact source reconstruction: "+id);
            continue;
        }
        // Legacy wall markers may contain only material/dimension metadata.
        // They have no curve provenance to rebase; do not promote them into
        // physical wall geometry during an unrelated entity edit.
        if (!source.properties.contains("baseline")) {
            if (source.extensions.contains("curve_input_derivation") ||
                found->second.extensions.contains("curve_input_derivation"))
                invalid("Curve derivation requires its original baseline: "+id);
            continue;
        }
        const auto& source_baseline=source.properties.at("baseline");
        if (!source.extensions.contains("curve_input_derivation") &&
            !found->second.extensions.contains("curve_input_derivation") &&
            (!source_baseline.is_object() || !source_baseline.contains("sweep_radians") ||
             !source_baseline.at("sweep_radians").is_number() ||
             source_baseline.at("sweep_radians")==0.0)) continue;
        const auto old=read_baseline(source); const auto next=read_baseline(found->second);
        const bool derived=source.extensions.contains("curve_input_derivation");
        const bool next_derived=found->second.extensions.contains("curve_input_derivation");
        if (next_derived) validate_wall_curve_input(found->second);
        const auto previous_count=derived ? source.extensions.at("curve_input_derivation").at("operations").size() : 0;
        bool rigid_append=false;
        if (next_derived) {
            const auto& operations=found->second.extensions.at("curve_input_derivation").at("operations");
            bool new_rigid=false;
            for (std::size_t i=previous_count;i<operations.size();++i)
                new_rigid=new_rigid || is_rigid_operation(operations.at(i));
            rigid_append=operations.size()==previous_count+1 && is_rigid_operation(operations.back());
            if (new_rigid && !rigid_append)
                invalid("Wall rigid transform must append exactly one operation: "+id);
        }
        if (!derived && found->second.extensions.contains("curve_input_derivation")) {
            const auto& proof=found->second.extensions.at("curve_input_derivation");
            const bool line_origin=proof.at("version")==3;
            if (line_origin) {
                const auto expected=reconstruct_exterior_segment_arc_wall(source,next);
                if (!qualified_line_origin || old.sweep_radians!=0 ||
                    expected.extensions.at("curve_input_derivation")!=proof ||
                    expected.extensions.at("curve_input")!=found->second.extensions.at("curve_input"))
                    invalid("Line-origin curve derivation requires the new typed measured arc proof: "+id);
            } else if ((!qualified && !rigid_append) || !source.extensions.contains("curve_input") ||
                proof.at("source_input")!=source.extensions.at("curve_input") ||
                proof.at("source_baseline")!=source.properties.at("baseline"))
                invalid("New curve derivation must replay its exact source input through a typed proof: "+id);
        }
        if (derived && !found->second.extensions.contains("curve_input_derivation"))
            invalid("Wall edit cannot discard original curve input derivation: "+id);
        if (derived && source.extensions.at("curve_input_derivation").at("source_input")!=
            found->second.extensions.at("curve_input_derivation").at("source_input"))
            invalid("Wall edit cannot rewrite original curve construction input: "+id);
        bool reconstruction_proof=false;
        if (derived) {
            const auto& previous=source.extensions.at("curve_input_derivation");
            const auto& proof=found->second.extensions.at("curve_input_derivation");
            if (previous.at("version")!=proof.at("version") &&
                !(previous.at("version")==1 && proof.at("version")==2 && rigid_append))
                invalid("Wall curve derivation cannot downgrade or change version without a rigid append");
            if (previous.at("source_baseline")!=proof.at("source_baseline") ||
                proof.at("operations").size()<previous.at("operations").size())
                invalid("Wall edit cannot discard source curve derivation");
            for (std::size_t i=0;i<previous.at("operations").size();++i)
                if (previous.at("operations").at(i)!=proof.at("operations").at(i))
                    invalid("Wall edit cannot rewrite prior curve derivation operations");
            reconstruction_proof=proof.at("operations").size()==previous.at("operations").size()+1 &&
                proof.at("operations").back().contains("input");
            if (!qualified && reconstruction_proof) { validate_wall_curve_input(found->second); continue; }
            if (!qualified && same_baseline(old,next) && proof!=previous)
                invalid("Unchanged curve geometry cannot rewrite its derivation");
        }
        if (rigid_append) {
            const auto& operation=found->second.extensions.at("curve_input_derivation").at("operations").back();
            auto expected=source;
            const auto transform=decode_rigid_transform(operation.at("transform"));
            const auto transformed=rigid_curve_baseline(old,transform);
            transform_wall_curve_input(expected,transform);
            set_baseline(expected,transformed,&transform);
            if (expected.properties.at("baseline")!=found->second.properties.at("baseline") ||
                !same_wall_top_properties(expected,found->second) ||
                expected.extensions.at("curve_input")!=found->second.extensions.at("curve_input") ||
                expected.extensions.at("curve_input_derivation")!=found->second.extensions.at("curve_input_derivation"))
                invalid("Wall rigid transform did not retain its exact source and independently reconstructed provenance: "+id);
            continue;
        }
        if (old.sweep_radians==0 || next.sweep_radians!=old.sweep_radians || same_baseline(old,next)) continue;
        const bool deformed=std::abs(std::hypot(old.end.x-old.start.x,old.end.y-old.start.y)-
            std::hypot(next.end.x-next.start.x,next.end.y-next.start.y))>constraint_linear_tolerance_metres;
        if (deformed && !qualified && !derived && found->second.extensions.contains("curve_input_derivation"))
            invalid("New curve endpoint derivation requires a typed wall proof: "+id);
        bool constrained=false;
        for (const auto& [relation_id,entity] : before) {
            if (entity.type!="constraint" || !after.contains(relation_id) || after.at(relation_id).type!="constraint") continue;
            const auto decoded=decode_constraint_entity(entity);
            if (decoded.constraint && std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                [&](const auto& binding) { return binding.owner_id==id; })) { constrained=true; break; }
        }
        if (deformed && !qualified && !derived) {
            const auto input=found->second.extensions.find("curve_input");
            if (input!=found->second.extensions.end()) validate_wall_curve_input(found->second);
            bool reconstruction=false;
            const auto original=source.extensions.find("curve_input");
            if (input!=found->second.extensions.end() && original!=source.extensions.end()) {
                for (const auto* key : {"construction","measure","measure_value"})
                    if (input->contains(key) && original->contains(key) && input->at(key)!=original->at(key)) reconstruction=true;
            }
            if (constrained && !reconstruction) invalid("Constrained curved endpoint deformation requires a typed wall proof: "+id);
            // Explicit construction of an unconstrained curve keeps the normal
            // editor workflow. Its new input was independently reconstructed.
            if (!constrained || reconstruction) continue;
        }
        if (deformed && !qualified && derived) invalid("Derived curve endpoint deformation requires a typed wall proof: "+id);
        auto expected=source; rebase_wall_curve_input(expected,next);
        for (const auto* key : {"curve_input","curve_input_derivation"}) {
            const auto expected_value=expected.extensions.find(key); const auto value=found->second.extensions.find(key);
            if ((expected_value==expected.extensions.end())!=(value==found->second.extensions.end()) ||
                (expected_value!=expected.extensions.end() && *expected_value!=*value))
                invalid("Wall endpoint edit did not retain its curve provenance: "+id);
        }
    }
}

namespace {
bool valid_wall_identifier(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(),id.end(),[](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        });
}
void validate_edit(const ConstraintWallGeometryEdit& edit) {
    const auto& b = edit.baseline;
    const auto baseline_length = std::hypot(b.end.x-b.start.x,b.end.y-b.start.y);
    const bool straight=edit.version==1 || edit.version==5;
    const bool rigid=edit.version==4 || edit.version==5;
    if (!valid_wall_identifier(edit.wall_id) || (edit.version!=1 && edit.version!=2 && edit.version!=3 && edit.version!=4 && edit.version!=5 && edit.version!=6) ||
        (straight ? b.sweep_radians!=0.0 : b.sweep_radians==0.0) || !std::isfinite(b.sweep_radians) ||
        !std::isfinite(b.start.x) || !std::isfinite(b.start.y) ||
        !std::isfinite(b.end.x) || !std::isfinite(b.end.y) ||
        !std::isfinite(baseline_length) || baseline_length <= constraint_linear_tolerance_metres)
        invalid("Wall constraint edit requires an identified finite versioned nondegenerate baseline");
    if (!straight) (void)arc_from_chord_angle(b.start,b.end,b.sweep_radians);
    if (rigid != edit.rigid_transform.has_value())
        invalid("Selected rigid wall proof requires its exact transform and version four or five");
    if (edit.rigid_transform) (void)decode_rigid_transform(encode_rigid_transform(*edit.rigid_transform));
    if (edit.version==6) {
        if (!edit.curve_construction || edit.length_entry)
            invalid("Explicit curve proof requires its construction input and no physical length entry");
        if (!same_baseline(explicit_curve_baseline(*edit.curve_construction,edit.wall_id),b))
            invalid("Explicit curve proof does not exactly reconstruct its supplied baseline");
        if (edit.wall_classification && (edit.wall_classification->empty() ||
            edit.wall_classification->size()>256 || edit.wall_classification->find('\0')!=std::string::npos))
            invalid("Explicit curve proof classification must be a bounded nonempty string without NUL");
        validate_curve_edit_size(json{{"version",6},{"wall_id",edit.wall_id},
            {"baseline",baseline_json(b)},{"length_entry",nullptr},
            {"curve_construction",*edit.curve_construction},
            {"wall_classification",edit.wall_classification ? json(*edit.wall_classification) : json(nullptr)}});
    } else if (edit.curve_construction || edit.wall_classification)
        invalid("Curve construction and classification require wall proof version six");
    if (edit.version==3 && !edit.length_entry) invalid("Curved physical length proof requires an exact length entry");
    if (edit.length_entry) {
        if (edit.version==2) invalid("Curved endpoint edits cannot contain physical length entries");
        const auto length = normalize_positive_quantity(*edit.length_entry);
        if (std::abs(segment_length(b)-length.metres) >
            constraint_linear_tolerance_metres)
            invalid("Wall constraint edit exact quantity does not match its baseline");
    }
}
void exact_fields(const json& value, std::initializer_list<const char*> fields) {
    if (!value.is_object() || value.size() != fields.size())
        invalid("Wall constraint edit contains unexpected fields");
    for (const auto* field : fields)
        if (!value.contains(field)) invalid("Wall constraint edit is missing a required field");
}
}

Entity replay_constraint_wall_edit(const Entity& source, const ConstraintWallGeometryEdit& edit) {
    validate_edit(edit);
    if (source.id != edit.wall_id || source.type != "wall")
        invalid("Wall constraint edit owner is not its original wall");
    const auto old = read_baseline(source);
    if (edit.version==6) {
        if (old.sweep_radians==0.0)
            invalid("Explicit curve proof requires an existing curved source wall");
        (void)arc_from_chord_angle(old.start,old.end,old.sweep_radians);
        validate_wall_curve_input(source);
        validate_wall_length_input(source);
        const auto input=source.extensions.find("curve_input");
        validate_preserved_curve_metadata(input==source.extensions.end() ? nullptr : &*input,
            *edit.curve_construction);
        auto result=source;
        const bool geometry_changed=!same_baseline(old,edit.baseline);
        const bool input_changed=input==source.extensions.end() ||
            input->dump()!=edit.curve_construction->dump();
        if (geometry_changed) {
            clear_wall_length_input(result);
            set_baseline(result,edit.baseline);
        }
        if (geometry_changed || input_changed) {
            result.extensions["curve_input"]=*edit.curve_construction;
            preserve_wall_curve_construction(result,source);
        }
        if (edit.wall_classification) result.properties["classification"]=*edit.wall_classification;
        validate_wall_curve_input(result);
        validate_wall_length_input(result);
        return result;
    }
    if (edit.version==4 || edit.version==5) {
        const bool straight=edit.version==5;
        if (straight ? old.sweep_radians!=0.0 : old.sweep_radians==0.0)
            invalid("Selected rigid wall proof does not match its original straight or curved wall");
        const auto expected=straight ? rigid_straight_baseline(old,*edit.rigid_transform) :
            rigid_curve_baseline(old,*edit.rigid_transform);
        if (!same_baseline(expected,edit.baseline) || (!straight && same_baseline(old,expected)))
            invalid("Selected rigid wall proof does not exactly reconstruct its changed baseline");
        const auto section=source.extensions.find("constraint_authoring");
        const json* receipt=nullptr;
        if (section!=source.extensions.end() && section->is_object() && section->contains("last_length_entry"))
            receipt=&section->at("last_length_entry");
        if (static_cast<bool>(receipt)!=edit.length_entry.has_value())
            invalid("Selected rigid wall proof must retain its existing exact length entry");
        if (receipt) {
            (void)validate_length_receipt(*receipt,source);
            const auto& entry=*edit.length_entry;
            if (receipt->at("original_expression")!=entry.original_expression ||
                receipt->at("entered_unit")!=unit_name(entry.entered_unit) ||
                receipt->at("exact_metres").at("numerator")!=entry.exact_metres.numerator ||
                receipt->at("exact_metres").at("denominator")!=entry.exact_metres.denominator)
                invalid("Selected rigid wall proof cannot replace its existing exact length entry");
        }
        auto result=source;
        if (!straight) transform_wall_curve_input(result,*edit.rigid_transform);
        rebase_wall_length_receipt(result,expected);
        set_baseline(result,expected,&*edit.rigid_transform);
        validate_wall_curve_input(result);
        validate_wall_length_input(result);
        // A straight-axis reflection can preserve both endpoints while changing
        // the transverse grade. Canonicalizing an omitted scalar under an
        // identity transform is not a physical edit.
        bool changed_plane=false;
        if (straight && source.properties.contains("top_plane")) {
            const auto before=parse_wall_top_plane(source.properties.at("top_plane"));
            const auto after=parse_wall_top_plane(result.properties.at("top_plane"));
            changed_plane=before.x!=after.x || before.y!=after.y;
        }
        if (straight && same_baseline(old,expected) && !changed_plane)
            invalid("Selected rigid straight wall proof must change its source wall");
        return result;
    }
    if ((edit.version==1 && old.sweep_radians!=0.0) ||
        (edit.version>=2 && (old.sweep_radians==0.0 || old.sweep_radians!=edit.baseline.sweep_radians)))
        invalid("Wall constraint proof must preserve the source signed sweep");
    const auto near = [](Vec2 a, Vec2 b) {
        return std::hypot(a.x-b.x,a.y-b.y) <= constraint_linear_tolerance_metres;
    };
    if (near(old.start, edit.baseline.end) && near(old.end, edit.baseline.start))
        invalid("Wall constraint edit would reverse wall endpoint identity");
    auto result = source;
    validate_or_clear_length_receipt(result, edit.length_entry.has_value(),
        edit.length_entry ? &*edit.length_entry : nullptr, edit.baseline);
    if (edit.version>=2) rebase_wall_curve_input(result,edit.baseline);
    set_baseline(result, edit.baseline);
    if (edit.version>=2) validate_wall_curve_input(result);
    validate_wall_length_input(result);
    return result;
}

nlohmann::json encode_constraint_wall_edit(const ConstraintWallGeometryEdit& edit) {
    validate_edit(edit);
    json b = json::object();
    update_baseline_json(b, edit.baseline);
    json receipt = nullptr;
    if (edit.length_entry) {
        const auto& q = *edit.length_entry;
        receipt = {{"original_expression",q.original_expression}, {"entered_unit",unit_name(q.entered_unit)},
            {"exact_metres",{{"numerator",q.exact_metres.numerator},{"denominator",q.exact_metres.denominator}}}};
    }
    json result={{"wall_id",edit.wall_id},{"baseline",b},{"length_entry",receipt}};
    if (edit.version>=2) result["version"]=edit.version;
    if (edit.version==4 || edit.version==5) result["rigid_transform"]=encode_rigid_transform(*edit.rigid_transform);
    if (edit.version==6) {
        result["curve_construction"]=*edit.curve_construction;
        result["wall_classification"]=edit.wall_classification ? json(*edit.wall_classification) : json(nullptr);
        validate_curve_edit_size(result);
    }
    return result;
}

ConstraintWallGeometryEdit decode_constraint_wall_edit(const nlohmann::json& value) {
    if (value.contains("version")) {
        if (value.at("version")==4 || value.at("version")==5) exact_fields(value,{"version","wall_id","baseline","length_entry","rigid_transform"});
        else if (value.at("version")==6) {
            exact_fields(value,{"version","wall_id","baseline","length_entry","curve_construction","wall_classification"});
            validate_curve_edit_size(value);
        }
        else exact_fields(value,{"version","wall_id","baseline","length_entry"});
        if (!value.at("version").is_number_integer() || (value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4 && value.at("version")!=5 && value.at("version")!=6))
            invalid("Unsupported wall constraint proof version");
    } else exact_fields(value,{"wall_id","baseline","length_entry"});
    exact_fields(value.at("baseline"),{"start","end","sweep_radians"});
    if (!value.at("wall_id").is_string()) invalid("Wall constraint owner ID must be a string");
    Entity temporary{value.at("wall_id").get<std::string>(),"wall",{{"baseline",value.at("baseline")}}};
    ConstraintWallGeometryEdit result{temporary.id,read_baseline(temporary),std::nullopt};
    result.version=value.contains("version") ? value.at("version").get<std::uint64_t>() : 1;
    if (result.version==4 || result.version==5) result.rigid_transform=decode_rigid_transform(value.at("rigid_transform"));
    if (result.version==6) {
        result.curve_construction=value.at("curve_construction");
        const auto& classification=value.at("wall_classification");
        if (!classification.is_null()) {
            if (!classification.is_string()) invalid("Explicit curve proof classification must be a string or null");
            result.wall_classification=classification.get<std::string>();
        }
    }
    const auto& entry = value.at("length_entry");
    if (!entry.is_null()) {
        exact_fields(entry,{"original_expression","entered_unit","exact_metres"});
        exact_fields(entry.at("exact_metres"),{"numerator","denominator"});
        if (!entry.at("original_expression").is_string() || !entry.at("entered_unit").is_string())
            invalid("Wall constraint length expression and unit must be strings");
        std::optional<Unit> unit;
        for (const auto candidate : {Unit::metre,Unit::millimetre,Unit::centimetre,Unit::foot,Unit::inch})
            if (entry.at("entered_unit") == unit_name(candidate)) unit = candidate;
        if (!unit) invalid("Wall constraint length has an unsupported unit");
        const auto q = parse_quantity(entry.at("original_expression").get<std::string>(),*unit);
        if (q.entered_unit != *unit || !entry.at("exact_metres").at("numerator").is_number_integer() ||
            !entry.at("exact_metres").at("denominator").is_number_integer() ||
            entry.at("exact_metres").at("numerator") != q.exact_metres.numerator ||
            entry.at("exact_metres").at("denominator") != q.exact_metres.denominator)
            invalid("Wall constraint length has inconsistent exact quantity metadata");
        result.length_entry = q;
    }
    validate_edit(result);
    return result;
}
void validate_constraint_wall_host(const std::string& wall_id, const std::map<std::string, Entity, std::less<>>& entities) {
    const auto& entity = entities.at(wall_id);
    const auto baseline = read_baseline(entity);
    try {
        Wall wall{
            wall_id,
            baseline,
            finite_number(entity.properties.at("thickness_m"), "Wall thickness"),
            finite_number(entity.properties.at("height_m"), "Wall height"),
            finite_number(entity.properties.at("elevation_m"), "Wall elevation"),
            {},
        };
        if (const auto layers = entity.properties.find("layers");
            layers != entity.properties.end()) {
            wall.layers = parse_wall_layers(layers.value(), wall.thickness);
        }
        std::string top_error;
        if (!read_document_wall_top_profile(entity,wall,top_error)) invalid(top_error);
        for (const auto& [id, candidate] : entities) {
            if (candidate.type != "opening" || !candidate.properties.is_object()) {
                continue;
            }
            const auto host = candidate.properties.find("wall_id");
            if (host == candidate.properties.end() || !host->is_string() ||
                host->get_ref<const std::string&>() != wall_id) {
                continue;
            }
            const bool managed = candidate.id == id && managed_corner_cut(candidate, entities);
            const auto& p = candidate.properties;
            wall.openings.push_back(
                {id, opening_scalar(p, "offset_m", "offset", "Opening offset", managed),
                 opening_scalar(p, "width_m", "width", "Opening width", managed),
                 opening_scalar(p, "sill_m", "sill", "Opening sill", managed),
                 opening_scalar(p, "height_m", "height", "Opening height", managed)});
        }
        validate_wall_semantics(wall);
    } catch (const std::out_of_range&) {
        invalid("Wall or hosted opening is missing required geometry: " + wall_id);
    }
}

// Wall scaling captures complete old properties as passive quantity provenance.
// Only closed, bounded dialects qualify historical slots on an inspection copy.
void wall_scale_quantity_reference_remainder(Entity& entity) {
    const auto reject = [](const std::string& reason) {
        invalid(reason);
    };
    const auto field = [](const json& value, const char* name) -> const json* {
        if (!value.is_object()) return nullptr;
        const auto found=value.find(name); return found==value.end() ? nullptr : &*found;
    };
    for (const auto* key : {"wall_scale_quantity_archive", "wall_scale_hosted_quantity_archive"}) {
        const auto found=entity.extensions.find(key);
        if (found==entity.extensions.end()) continue;
        const bool hosted=std::string_view(key)=="wall_scale_hosted_quantity_archive";
        if ((hosted && entity.type!="wall") || (!hosted && entity.type!="wall" &&
            entity.type!="opening" && entity.type!="door" && entity.type!="window"))
            reject("wall scale quantity archive has unsupported owner: " + entity.id);
        auto& archive=*found;
        const auto version=field(archive,"version"),entries=field(archive,"entries");
        if (!archive.is_object() || archive.size()!=2 || !version || !version->is_number_integer() ||
            *version!=1 || !entries || !entries->is_array() || entries->empty() || entries->size()>4096)
            reject("unsupported wall scale quantity archive: " + entity.id);
        // Keep the original retirement traversal limits before serialization:
        // depth 64, 4 Mi nodes, 64 MiB strings/keys/binary, finite scalars.
        constexpr std::size_t node_limit=4*1024*1024,byte_limit=64*1024*1024;
        std::size_t nodes{},bytes{};
        const auto text=[&](const std::string& value) {
            if (value.size()>byte_limit-bytes) reject("source string/key budget exceeded");
            bytes+=value.size();
        };
        std::vector<std::pair<const json*,std::size_t>> pending{{&archive,0}};
        while (!pending.empty()) {
            const auto [value,depth]=pending.back(); pending.pop_back();
            if (depth>64 || ++nodes>node_limit) reject("source JSON node/nesting budget exceeded");
            if (value->is_number_float() && !std::isfinite(value->get<double>())) reject("source has nonfinite scalar");
            if (value->is_string()) text(value->get_ref<const std::string&>());
            if (value->is_binary()) {
                if (value->get_binary().size()>byte_limit-bytes) reject("source binary budget exceeded");
                bytes+=value->get_binary().size();
            }
            if (!value->is_structured()) continue;
            if (value->size()>node_limit-nodes || pending.size()>node_limit-nodes-value->size())
                reject("source JSON pending-node budget exceeded");
            if (value->is_object()) for (const auto& [name,child] : value->items()) {
                text(name); pending.emplace_back(&child,depth+1);
            } else for (const auto& child : *value) pending.emplace_back(&child,depth+1);
        }
        if (archive.dump().size()>1024*1024)
            reject("wall scale quantity archive exceeds its byte budget: " + entity.id);
        for (const auto& row : *entries) {
            const auto properties=field(row,"source_properties"),pivot=field(row,"pivot"),scale=field(row,"scale");
            const auto quantities=properties ? field(*properties,"quantity_entries") : nullptr;
            if (!row.is_object() || row.size()!=(hosted ? 4 : 3) || !properties || !properties->is_object() ||
                !quantities || !quantities->is_object() || !pivot || !pivot->is_array() || pivot->size()!=3 ||
                !scale || !scale->is_number() || !std::isfinite(scale->get<double>()) ||
                scale->get<double>()<=0 || scale->get<double>()==1)
                reject("malformed wall scale quantity archive row: " + entity.id);
            for (const auto& coordinate : *pivot)
                if (!coordinate.is_number() || !std::isfinite(coordinate.get<double>()))
                    reject("wall scale quantity archive has invalid pivot: " + entity.id);
            if (hosted) {
                const auto owner=field(row,"source_owner_id");
                if (!owner || !owner->is_string()) reject("hosted wall scale archive lacks source identity: " + entity.id);
                const auto& id=owner->get_ref<const std::string&>();
                if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
                    return (c>='a' && c<='z') || (c>='A' && c<='Z') ||
                        (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c==':';
                })) reject("identity must contain 1..128 supported ASCII characters: " + id);
            }
        }
        for (auto& row : archive.at("entries")) {
            row.erase("source_properties");
            if (hosted) row.erase("source_owner_id");
        }
    }
}

} // namespace sketch

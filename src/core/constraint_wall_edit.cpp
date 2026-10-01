#include "sketch/constraint_wall_edit.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/constraint_entity.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

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

void set_baseline(Entity& wall, const Segment& baseline) {
    update_baseline_json(wall.properties.at("baseline"), baseline);
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
    const auto section=wall.extensions.find("constraint_authoring");
    if (section==wall.extensions.end() || !section->is_object() ||
        !section->contains("version") || !section->at("version").is_number_integer() || section->at("version")!=1) return;
    const auto receipt=section->find("last_length_entry");
    if (receipt==section->end() || !receipt->is_object() || !receipt->contains("version") ||
        !receipt->at("version").is_number_integer() ||
        (receipt->at("version")!=1 && receipt->at("version")!=2)) return;
    (void)validate_length_receipt(*receipt,wall);
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
        (proof.at("version")!=1 && proof.at("version")!=2) ||
        (proof.at("version")==2 && !proof.at("version").is_number_integer()) ||
        !proof.at("operations").is_array() || proof.at("operations").empty())
        invalid("Wall has unsupported curve input derivation: "+wall.id);
    auto source=wall; source.properties["baseline"]=proof.at("source_baseline");
    auto baseline=read_baseline(source);
    validate_input(proof.at("source_input"),baseline,wall.id);
    auto expected_input=proof.at("source_input");
    bool has_rigid=false;
    for (const auto& operation : proof.at("operations")) {
        if (is_rigid_operation(operation)) {
            if (proof.at("version")!=2 || operation.size()!=3 || operation.at("kind")!="rigid_transform" ||
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
            baseline=next; has_rigid=true;
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
        baseline=next;
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
    } else (*proof)["version"]=2;
    auto recorded_baseline=wall.properties.at("baseline");
    update_baseline_json(recorded_baseline,transformed);
    (*proof)["operations"].push_back({{"kind","rigid_transform"},
        {"transform",encode_rigid_transform(transform)},{"baseline",recorded_baseline}});
    candidate.extensions["curve_input"]=rigid_curve_input(*input,transformed,transform);
    set_baseline(candidate,transformed);
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

void validate_constraint_wall_geometry_transition(const std::map<std::string,Entity,std::less<>>& before,
    const std::map<std::string,Entity,std::less<>>& after,bool qualified) {
    for (const auto& [id,source] : before) {
        const auto found=after.find(id);
        if (source.type!="wall" || found==after.end() || found->second.type!="wall") continue;
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
            if ((!qualified && !rigid_append) || !source.extensions.contains("curve_input") ||
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
            set_baseline(expected,transformed);
            if (expected.properties.at("baseline")!=found->second.properties.at("baseline") ||
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
    if (!valid_wall_identifier(edit.wall_id) || (edit.version!=1 && edit.version!=2 && edit.version!=3) ||
        (edit.version==1 ? b.sweep_radians!=0.0 : b.sweep_radians==0.0) || !std::isfinite(b.sweep_radians) ||
        !std::isfinite(b.start.x) || !std::isfinite(b.start.y) ||
        !std::isfinite(b.end.x) || !std::isfinite(b.end.y) ||
        !std::isfinite(baseline_length) || baseline_length <= constraint_linear_tolerance_metres)
        invalid("Wall constraint edit requires an identified finite versioned nondegenerate baseline");
    if (edit.version>=2) (void)arc_from_chord_angle(b.start,b.end,b.sweep_radians);
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
    return result;
}

ConstraintWallGeometryEdit decode_constraint_wall_edit(const nlohmann::json& value) {
    if (value.contains("version")) {
        exact_fields(value,{"version","wall_id","baseline","length_entry"});
        if (!value.at("version").is_number_integer() || (value.at("version")!=2 && value.at("version")!=3))
            invalid("Unsupported wall constraint proof version");
    } else exact_fields(value,{"wall_id","baseline","length_entry"});
    exact_fields(value.at("baseline"),{"start","end","sweep_radians"});
    if (!value.at("wall_id").is_string()) invalid("Wall constraint owner ID must be a string");
    Entity temporary{value.at("wall_id").get<std::string>(),"wall",{{"baseline",value.at("baseline")}}};
    ConstraintWallGeometryEdit result{temporary.id,read_baseline(temporary),std::nullopt};
    result.version=value.contains("version") ? value.at("version").get<std::uint64_t>() : 1;
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
        if (const auto slope = entity.properties.find("slope_rise_m");
            slope != entity.properties.end()) {
            if (!slope->is_number()) {
                invalid("Wall slope_rise_m must be a finite number");
            }
            wall.slope_rise = slope->get<double>();
        }
        for (const auto& [id, candidate] : entities) {
            if (candidate.type != "opening" || !candidate.properties.is_object()) {
                continue;
            }
            const auto host = candidate.properties.find("wall_id");
            if (host == candidate.properties.end() || !host->is_string() ||
                host->get_ref<const std::string&>() != wall_id) {
                continue;
            }
            wall.openings.push_back(
                {id, finite_number(candidate.properties.at("offset_m"), "Opening offset"),
                 finite_number(candidate.properties.at("width_m"), "Opening width"),
                 finite_number(candidate.properties.at("sill_m"), "Opening sill"),
                 finite_number(candidate.properties.at("height_m"), "Opening height")});
        }
        validate_wall_semantics(wall);
    } catch (const std::out_of_range&) {
        invalid("Wall or hosted opening is missing required geometry: " + wall_id);
    }
}

} // namespace sketch

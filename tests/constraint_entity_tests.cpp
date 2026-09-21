#include "sketch/constraint_entity.hpp"
#include "support/noninteractive_errors.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace sketch;
using json = nlohmann::json;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "constraint_entity_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void require_invalid(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    } catch (const std::exception& error) {
        std::cerr << "constraint_entity_tests: " << message << ": wrong exception: "
                  << error.what() << '\n';
        std::exit(1);
    }
    fail(message);
}

const PersistentConstraint& require_known(const ConstraintEntityDecodeResult& result,
                                          std::string_view message) {
    if (result.constraint.has_value()) {
        return *result.constraint;
    }
    fail(message);
}

const ConstraintEntityDecodeResult& require_unsupported(
    const ConstraintEntityDecodeResult& result, std::string_view message) {
    if (!result.constraint.has_value() && !result.unsupported_reason.empty() &&
        result.original_entity.has_value()) {
        return result;
    }
    fail(message);
}

WallEndpointBinding endpoint(std::string owner_id, WallEndpointRole role) {
    return WallEndpointBinding{std::move(owner_id), role};
}

std::vector<WallEndpointBinding> one_wall_bindings() {
    return {endpoint("wall-a", WallEndpointRole::start),
            endpoint("wall-a", WallEndpointRole::end)};
}

std::vector<WallEndpointBinding> two_wall_bindings() {
    return {endpoint("wall-a", WallEndpointRole::start),
            endpoint("wall-a", WallEndpointRole::end),
            endpoint("wall-b", WallEndpointRole::start),
            endpoint("wall-b", WallEndpointRole::end)};
}

PersistentConstraint model_for(ConstraintRelationKind relation) {
    PersistentConstraint model;
    model.id = "constraint-1";
    model.relation = relation;
    if (relation == ConstraintRelationKind::fixed_anchor) {
        model.bindings = {endpoint("wall-a", WallEndpointRole::start)};
        model.anchor = Vec2{1.25, -2.5};
    } else if (relation == ConstraintRelationKind::parallel ||
               relation == ConstraintRelationKind::perpendicular) {
        model.bindings = two_wall_bindings();
    } else {
        model.bindings = one_wall_bindings();
    }
    if (relation == ConstraintRelationKind::fixed_length) {
        model.length = parse_quantity("1/3 ft", Unit::foot);
    }
    return model;
}

std::vector<ConstraintRelationKind> all_relations() {
    return {ConstraintRelationKind::horizontal,     ConstraintRelationKind::vertical,
            ConstraintRelationKind::coincident,     ConstraintRelationKind::fixed_length,
            ConstraintRelationKind::parallel,       ConstraintRelationKind::perpendicular,
            ConstraintRelationKind::fixed_anchor};
}

void require_same_model(const PersistentConstraint& actual, const PersistentConstraint& expected,
                        std::string_view context) {
    require(actual.id == expected.id, context);
    require(actual.relation == expected.relation, context);
    require(actual.bindings == expected.bindings, context);
    require(actual.length.has_value() == expected.length.has_value(), context);
    if (actual.length && expected.length) {
        require(actual.length->metres == expected.length->metres, context);
        require(actual.length->exact_metres == expected.length->exact_metres, context);
        require(actual.length->entered_unit == expected.length->entered_unit, context);
        require(actual.length->original_expression == expected.length->original_expression, context);
    }
    require(actual.anchor.has_value() == expected.anchor.has_value(), context);
    if (actual.anchor && expected.anchor) {
        require(actual.anchor->x == expected.anchor->x && actual.anchor->y == expected.anchor->y,
                context);
    }
}

void test_all_relations_round_trip() {
    for (const auto relation : all_relations()) {
        const auto model = model_for(relation);
        const auto encoded = encode_constraint_entity(model);
        require(encoded.type == "constraint", "encoded entity must be a constraint");
        require(encoded.id == model.id, "encoded entity must retain the stable constraint id");
        require(encoded.properties.at("version") == 1, "encoded version must be v1");
        require(encoded.properties.at("relation").get<std::string>() ==
                    constraint_relation_name(relation),
                "encoded relation name must be canonical");

        const auto decoded = decode_constraint_entity(encoded);
        const auto& decoded_model = require_known(decoded, "known relation should decode as known");
        require_same_model(decoded_model, model, "relation round trip must preserve semantics");

        const auto reencoded = encode_constraint_entity(decoded_model, &encoded);
        require(reencoded == encoded, "known relation should have a deterministic round trip");
    }
}

void test_wall_ids_are_sorted_and_complete() {
    auto model = model_for(ConstraintRelationKind::parallel);
    model.bindings = {endpoint("wall-z", WallEndpointRole::start),
                      endpoint("wall-z", WallEndpointRole::end),
                      endpoint("wall-a", WallEndpointRole::start),
                      endpoint("wall-a", WallEndpointRole::end)};
    const auto entity = encode_constraint_entity(model);
    require(entity.properties.at("wall_ids") == json({"wall-a", "wall-z"}),
            "wall_ids must be sorted independently of binding order");

    auto malformed = entity;
    malformed.properties["wall_ids"] = json({"wall-a"});
    require_invalid([&] { (void)decode_constraint_entity(malformed); },
                    "wall_ids missing a binding owner must reject");

    malformed = entity;
    malformed.properties["wall_ids"] = json({"wall-a", "wall-a", "wall-z"});
    require_invalid([&] { (void)decode_constraint_entity(malformed); },
                    "duplicate wall_ids must reject");

    malformed = entity;
    malformed.properties["wall_ids"] = json({"wall-z", "wall-a"});
    require_invalid([&] { (void)decode_constraint_entity(malformed); },
                    "unsorted wall_ids must reject");
}

void test_exact_fixed_length_receipt_is_preserved() {
    const auto model = model_for(ConstraintRelationKind::fixed_length);
    const auto entity = encode_constraint_entity(model);
    const auto& length = entity.properties.at("length_m");
    require(length.is_number_float() || length.is_number_integer(),
            "fixed length must include canonical metre geometry");
    require(std::abs(length.get<double>() - (127.0 / 1250.0)) < 1e-15,
            "fixed length metre value must match the exact quantity");
    const auto& receipt = entity.properties.at("quantity_entries").at("/length_m");
    require(receipt.at("version") == 1, "quantity receipt version must be one");
    require(receipt.at("original_expression") == "1/3 ft",
            "quantity receipt must retain the entered expression");
    require(receipt.at("entered_unit") == "ft", "quantity receipt must retain the entered unit");
    require(receipt.at("exact_metres").at("numerator") == 127,
            "quantity receipt numerator must be exact");
    require(receipt.at("exact_metres").at("denominator") == 1250,
            "quantity receipt denominator must be exact");

    const auto decoded_result = decode_constraint_entity(entity);
    const auto& decoded = require_known(decoded_result, "fixed length should decode as known");
    require(decoded.length.has_value(), "fixed length decode must retain the quantity");
    require(decoded.length->exact_metres == ExactRational{127, 1250},
            "decoded fixed length must retain exact metres");
    require(decoded.length->original_expression == "1/3 ft",
            "decoded fixed length must retain original expression");
}

void test_merge_preserves_unrelated_metadata_and_extensions() {
    auto model = model_for(ConstraintRelationKind::fixed_length);
    auto original = encode_constraint_entity(model);
    original.required = true;
    original.properties["future_semantics"] = {"opaque", 7, true};
    original.properties["bindings"][0]["future_binding_metadata"] = {{"slot", "start"}};
    original.properties["bindings"][1]["future_binding_metadata"] = {{"slot", "end"}};
    original.properties["quantity_entries"]["/length_m"]["future_receipt_metadata"] =
        {{"source", "survey"}};
    original.properties["quantity_entries"]["/length_m"]["exact_metres"]["future_exact_metadata"] =
        {{"precision", "survey"}};
    original.properties["quantity_entries"]["/future_dimension"] = {
        {"version", 99}, {"opaque", "future"}};
    original.extensions["vendor_extension"] = {{"keep", "exactly"}};

    const auto decoded_result = decode_constraint_entity(original);
    const auto& decoded = require_known(decoded_result, "metadata fixture should decode as known");
    const auto merged = encode_constraint_entity(decoded, &original);
    require(merged.required, "merge should preserve the original required flag");
    require(merged.properties.at("future_semantics") == original.properties.at("future_semantics"),
            "merge should preserve unrelated future properties");
    require(merged.properties.at("quantity_entries").at("/future_dimension") ==
                original.properties.at("quantity_entries").at("/future_dimension"),
            "merge should preserve opaque future quantity records");
    require(merged.extensions == original.extensions,
            "merge should preserve unrelated entity extensions");
    require(merged.properties.at("quantity_entries").at("/length_m") ==
                original.properties.at("quantity_entries").at("/length_m"),
            "merge should replace the canonical receipt with the same validated receipt");
    require(merged.properties.at("bindings").at(0).at("future_binding_metadata") ==
                original.properties.at("bindings").at(0).at("future_binding_metadata") &&
                merged.properties.at("bindings").at(1).at("future_binding_metadata") ==
                    original.properties.at("bindings").at(1).at("future_binding_metadata"),
            "merge should preserve opaque metadata attached to stable bindings");
    require(merged.properties.at("quantity_entries").at("/length_m")
                .at("future_receipt_metadata") ==
                original.properties.at("quantity_entries").at("/length_m")
                    .at("future_receipt_metadata") &&
                merged.properties.at("quantity_entries").at("/length_m").at("exact_metres")
                        .at("future_exact_metadata") ==
                    original.properties.at("quantity_entries").at("/length_m")
                        .at("exact_metres").at("future_exact_metadata"),
            "merge should preserve opaque metadata nested in an unchanged receipt");

    auto changed_relation = model_for(ConstraintRelationKind::horizontal);
    changed_relation.bindings = model.bindings;
    const auto changed_relation_entity = encode_constraint_entity(changed_relation, &original);
    require(changed_relation_entity.properties.at("quantity_entries").at("/future_dimension") ==
                original.properties.at("quantity_entries").at("/future_dimension"),
            "changing relation should preserve opaque quantity entries");
    const auto changed_relation_result = decode_constraint_entity(changed_relation_entity);
    require_known(changed_relation_result,
                  "opaque noncanonical quantity entries must not invalidate a simple relation");

    auto changed_length = model_for(ConstraintRelationKind::fixed_length);
    changed_length.length = parse_quantity("2 ft", Unit::foot);
    const auto changed_length_entity = encode_constraint_entity(changed_length, &original);
    const auto& changed_receipt =
        changed_length_entity.properties.at("quantity_entries").at("/length_m");
    require(changed_receipt.at("original_expression") == "2 ft" &&
                !changed_receipt.contains("future_receipt_metadata") &&
                !changed_receipt.at("exact_metres").contains("future_exact_metadata"),
            "changing a fixed length should replace stale nested receipt metadata");
}

void test_unsupported_semantics_are_explicit_and_lossless() {
    auto unknown_relation = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    unknown_relation.properties["relation"] = "future_curve_lock";
    unknown_relation.properties["curve_payload"] = {{"kind", "arc"}, {"radius_m", 3.5}};
    const auto relation_result = decode_constraint_entity(unknown_relation);
    const auto& relation_unsupported = require_unsupported(
        relation_result, "unknown relation must be classified as unsupported");
    require(relation_unsupported.version == 1, "unsupported relation must retain its version");
    require(relation_unsupported.relation == "future_curve_lock",
            "unsupported relation name must be exposed");
    require(relation_unsupported.original_entity.has_value() &&
                *relation_unsupported.original_entity == unknown_relation,
            "unsupported relation data must remain lossless");

    auto unknown_version = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    unknown_version.properties["version"] = std::uint64_t{99};
    unknown_version.properties["future_lock"] = {{"mode", "topology"}};
    const auto version_result = decode_constraint_entity(unknown_version);
    const auto& version_unsupported = require_unsupported(
        version_result, "unknown version must be classified as unsupported");
    require(version_unsupported.version == 99, "unsupported version must be exposed");
    require(version_unsupported.relation == "horizontal",
            "unsupported version must retain the relation name");
    require(version_unsupported.original_entity.has_value() &&
                *version_unsupported.original_entity == unknown_version,
            "unknown version data must remain lossless");

    auto future_relation_with_new_binding_shape = unknown_relation;
    future_relation_with_new_binding_shape.properties["bindings"] = {
        {{"owner_id", "wall-a"}, {"feature", "baseline"}, {"role", "start"}},
        {{"owner_id", "wall-a"}, {"feature", "baseline"}, {"role", "end"}},
        {{"owner_id", "wall-b"}, {"feature", "baseline"}, {"role", "start"}},
    };
    future_relation_with_new_binding_shape.properties["bindings"][0]["feature"] = "curve";
    future_relation_with_new_binding_shape.properties["bindings"][0]["role"] = "control";
    future_relation_with_new_binding_shape.properties["wall_ids"] = {"wall-a", "wall-b"};
    const auto future_result = decode_constraint_entity(future_relation_with_new_binding_shape);
    require_unsupported(future_result,
                        "well-formed future relation binding count must remain unsupported");
}

void test_malformed_envelopes_reject() {
    const auto base = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    const std::vector<std::pair<std::string, json>> malformed = {
        {"missing version", [&] { return json{{"relation", "horizontal"}}; }()},
        {"negative version", [&] {
             auto value = base.properties;
             value["version"] = -1;
             return value;
         }()},
        {"non-integer version", [&] {
             auto value = base.properties;
             value["version"] = 1.5;
             return value;
         }()},
        {"missing relation", [&] {
             auto value = base.properties;
             value.erase("relation");
             return value;
         }()},
        {"non-string relation", [&] {
             auto value = base.properties;
             value["relation"] = 7;
             return value;
         }()},
        {"missing bindings", [&] {
             auto value = base.properties;
             value.erase("bindings");
             return value;
         }()},
        {"bindings not array", [&] {
             auto value = base.properties;
             value["bindings"] = json::object();
             return value;
         }()},
        {"binding not object", [&] {
             auto value = base.properties;
             value["bindings"] = {1, 2};
             return value;
         }()},
        {"missing owner", [&] {
             auto value = base.properties;
             value["bindings"][0].erase("owner_id");
             return value;
         }()},
        {"invalid owner", [&] {
             auto value = base.properties;
             value["bindings"][0]["owner_id"] = "wall/a";
             return value;
         }()},
        {"non-string feature", [&] {
             auto value = base.properties;
             value["bindings"][0]["feature"] = 4;
             return value;
         }()},
        {"unsupported feature", [&] {
             auto value = base.properties;
             value["bindings"][0]["feature"] = "outline";
             return value;
         }()},
        {"invalid role", [&] {
             auto value = base.properties;
             value["bindings"][0]["role"] = "middle";
             return value;
         }()},
        {"duplicate binding", [&] {
             auto value = base.properties;
             value["bindings"][1] = value["bindings"][0];
             return value;
         }()},
        {"wrong simple count", [&] {
             auto value = base.properties;
             value["bindings"].erase(1);
             return value;
         }()},
        {"missing wall ids", [&] {
             auto value = base.properties;
             value.erase("wall_ids");
             return value;
         }()},
        {"wall ids not array", [&] {
             auto value = base.properties;
             value["wall_ids"] = "wall-a";
             return value;
         }()},
        {"wall id not string", [&] {
             auto value = base.properties;
             value["wall_ids"] = {1};
             return value;
         }()},
        {"wall ids owner mismatch", [&] {
             auto value = base.properties;
             value["wall_ids"] = {"wall-b"};
             return value;
         }()},
    };

    for (const auto& [name, properties] : malformed) {
        auto entity = base;
        entity.properties = properties;
        require_invalid([&] { (void)decode_constraint_entity(entity); }, name);
    }

    auto malformed_entity = base;
    malformed_entity.type = "wall";
    require_invalid([&] { (void)decode_constraint_entity(malformed_entity); },
                    "a non-constraint entity must reject");
    malformed_entity = base;
    malformed_entity.id = "wall/id";
    require_invalid([&] { (void)decode_constraint_entity(malformed_entity); },
                    "an invalid constraint id must reject");
}

void test_semantic_payload_validation_rejects_malformed_known_data() {
    const auto fixed_length = encode_constraint_entity(model_for(ConstraintRelationKind::fixed_length));

    const std::vector<std::pair<std::string, json>> malformed_length = {
        {"missing length", [&] {
             auto value = fixed_length.properties;
             value.erase("length_m");
             return value;
         }()},
        {"missing quantity entries", [&] {
             auto value = fixed_length.properties;
             value.erase("quantity_entries");
             return value;
         }()},
        {"quantity entries not object", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"] = 3;
             return value;
         }()},
        {"missing length receipt", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"].erase("/length_m");
             return value;
         }()},
        {"receipt not object", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"]["/length_m"] = 3;
             return value;
         }()},
        {"receipt version mismatch", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"]["/length_m"]["version"] = 2;
             return value;
         }()},
        {"receipt expression mismatch", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"]["/length_m"]["original_expression"] = "2/3 ft";
             return value;
         }()},
        {"receipt unit mismatch", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"]["/length_m"]["entered_unit"] = "m";
             return value;
         }()},
        {"receipt exact mismatch", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"]["/length_m"]["exact_metres"]["numerator"] = 1;
             return value;
         }()},
        {"receipt missing exact", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"]["/length_m"].erase("exact_metres");
             return value;
         }()},
        {"receipt denominator zero", [&] {
             auto value = fixed_length.properties;
             value["quantity_entries"]["/length_m"]["exact_metres"]["denominator"] = 0;
             return value;
         }()},
        {"length nonfinite", [&] {
             auto value = fixed_length.properties;
             value["length_m"] = std::numeric_limits<double>::quiet_NaN();
             return value;
         }()},
        {"length negative", [&] {
             auto value = fixed_length.properties;
             value["length_m"] = -1.0;
             return value;
         }()},
    };
    for (const auto& [name, properties] : malformed_length) {
        auto entity = fixed_length;
        entity.properties = properties;
        require_invalid([&] { (void)decode_constraint_entity(entity); }, name);
    }

    auto anchor = encode_constraint_entity(model_for(ConstraintRelationKind::fixed_anchor));
    const std::vector<std::pair<std::string, json>> malformed_anchor = {
        {"missing anchor", [&] {
             auto value = anchor.properties;
             value.erase("anchor_m");
             return value;
         }()},
        {"anchor wrong count", [&] {
             auto value = anchor.properties;
             value["anchor_m"] = {1.0};
             return value;
         }()},
        {"anchor nonfinite", [&] {
             auto value = anchor.properties;
             value["anchor_m"] = {1.0, std::numeric_limits<double>::infinity()};
             return value;
         }()},
    };
    for (const auto& [name, properties] : malformed_anchor) {
        auto entity = anchor;
        entity.properties = properties;
        require_invalid([&] { (void)decode_constraint_entity(entity); }, name);
    }

    auto simple = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    simple.properties["length_m"] = 1.0;
    require_invalid([&] { (void)decode_constraint_entity(simple); },
                    "length payload on a simple relation must not be silently ignored");
    simple = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    simple.properties["anchor_m"] = {0.0, 0.0};
    require_invalid([&] { (void)decode_constraint_entity(simple); },
                    "anchor payload on a simple relation must not be silently ignored");
}

void test_numeric_integer_boundaries_and_receipts_reject_safely() {
    auto entity = encode_constraint_entity(model_for(ConstraintRelationKind::fixed_length));
    entity.properties["quantity_entries"]["/length_m"]["exact_metres"]["numerator"] =
        std::numeric_limits<std::uint64_t>::max();
    require_invalid([&] { (void)decode_constraint_entity(entity); },
                    "unsigned numerator beyond int64 must reject without wrapping");

    entity = encode_constraint_entity(model_for(ConstraintRelationKind::fixed_length));
    entity.properties["quantity_entries"]["/length_m"]["exact_metres"]["denominator"] =
        std::numeric_limits<std::uint64_t>::max();
    require_invalid([&] { (void)decode_constraint_entity(entity); },
                    "unsigned denominator beyond int64 must reject without wrapping");

    entity = encode_constraint_entity(model_for(ConstraintRelationKind::fixed_length));
    entity.properties["quantity_entries"]["/length_m"]["version"] =
        std::numeric_limits<std::uint64_t>::max();
    require_invalid([&] { (void)decode_constraint_entity(entity); },
                    "receipt version overflow must reject");

    entity = encode_constraint_entity(model_for(ConstraintRelationKind::fixed_length));
    entity.properties["length_m"] = std::numeric_limits<double>::infinity();
    require_invalid([&] { (void)decode_constraint_entity(entity); },
                    "infinite fixed length must reject");
}

void test_encode_rejects_double_only_or_inconsistent_locks() {
    auto model = model_for(ConstraintRelationKind::fixed_length);
    model.length.reset();
    require_invalid([&] { (void)encode_constraint_entity(model); },
                    "fixed length cannot be encoded without an exact quantity");

    model = model_for(ConstraintRelationKind::fixed_length);
    model.length->metres += 0.001;
    require_invalid([&] { (void)encode_constraint_entity(model); },
                    "inconsistent quantity double must reject");

    model = model_for(ConstraintRelationKind::fixed_length);
    model.length->exact_metres = ExactRational{1, 1};
    require_invalid([&] { (void)encode_constraint_entity(model); },
                    "inconsistent exact receipt must reject");

    model = model_for(ConstraintRelationKind::horizontal);
    model.length = parse_quantity("1 m", Unit::metre);
    require_invalid([&] { (void)encode_constraint_entity(model); },
                    "irrelevant length semantics must reject");

    model = model_for(ConstraintRelationKind::fixed_anchor);
    model.anchor->x = std::numeric_limits<double>::quiet_NaN();
    require_invalid([&] { (void)encode_constraint_entity(model); },
                    "nonfinite anchor semantics must reject");

    auto original = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    original.type = "wall";
    require_invalid([&] {
        (void)encode_constraint_entity(model_for(ConstraintRelationKind::horizontal), &original);
    }, "merging an entity of another type must reject");
    original.type = "constraint";
    original.id = "other-id";
    require_invalid([&] {
        (void)encode_constraint_entity(model_for(ConstraintRelationKind::horizontal), &original);
    }, "merging an entity with another stable id must reject");

    original = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    original.properties["version"] = 99;
    require_invalid([&] {
        (void)encode_constraint_entity(model_for(ConstraintRelationKind::horizontal), &original);
    }, "encoding over an unsupported original version must reject");

    original = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
    original.properties["relation"] = "future_lock";
    require_invalid([&] {
        (void)encode_constraint_entity(model_for(ConstraintRelationKind::horizontal), &original);
    }, "encoding over an unsupported original relation must reject");
}

void test_large_binding_envelopes_have_bounded_validation() {
    for (const int count : {2000, 4000, 8000}) {
        auto entity = encode_constraint_entity(model_for(ConstraintRelationKind::horizontal));
        entity.properties["version"] = 99;
        auto& values = entity.properties["bindings"];
        values = json::array();
        for (int index = 0; index < count; ++index)
            values.push_back({{"owner_id", "wall-a"}, {"feature", "future"},
                              {"role", "endpoint-" + std::to_string(index)}});
        entity.properties["wall_ids"] = json::array({"wall-a"});
        const auto start = std::chrono::steady_clock::now();
        const auto future = decode_constraint_entity(entity);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        require(!future.supported() && future.original_entity == entity,
                "large future binding envelope must remain preserved and unsupported");
        // A duplicate at the end exercises the complete envelope validation,
        // not an early malformed-item shortcut.
        values.push_back(values.front());
        require_invalid([&] { (void)decode_constraint_entity(entity); }, "large envelope terminal duplicate must reject");
        values.erase(values.end() - 1);
        entity.properties["version"] = 1;
        require_invalid([&] { (void)decode_constraint_entity(entity); }, "known relation must reject oversized binding count");
        std::cout << "future binding envelope " << count << ": " << elapsed << " ms\n";
    }
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    {
        PersistentConstraint boundary;
        boundary.id = "boundary-level";
        boundary.bindings = {{"outline", WallEndpointRole::start, "edge", "a"},
                             {"outline", WallEndpointRole::end, "edge", "b"}};
        auto entity = encode_constraint_entity(boundary);
        require(entity.properties.at("version") == 2 && !entity.properties.contains("wall_ids") &&
                    entity.properties.at("entity_ids") == nlohmann::json::array({"outline"}),
                "boundary lock must use version two generic owner references");
        require(decode_constraint_entity(entity).constraint->bindings == boundary.bindings,
                "boundary stable IDs did not round trip");
        entity.properties["bindings"][0]["vendor"] = "retained";
        const auto merged = encode_constraint_entity(boundary, &entity);
        require(merged.properties.at("bindings")[0].at("vendor") == "retained",
                "boundary binding opaque metadata was lost");
        entity.properties["bindings"][0].erase("vertex_id");
        require_invalid([&] { (void)decode_constraint_entity(entity); }, "boundary vertex ID is required");
        boundary.bindings[0].vertex_id.clear();
        require_invalid([&] { (void)encode_constraint_entity(boundary); }, "incomplete boundary binding encoded");
    }
    test_all_relations_round_trip();
    test_wall_ids_are_sorted_and_complete();
    test_exact_fixed_length_receipt_is_preserved();
    test_merge_preserves_unrelated_metadata_and_extensions();
    test_unsupported_semantics_are_explicit_and_lossless();
    test_malformed_envelopes_reject();
    test_semantic_payload_validation_rejects_malformed_known_data();
    test_numeric_integer_boundaries_and_receipts_reject_safely();
    test_encode_rejects_double_only_or_inconsistent_locks();
    test_large_binding_envelopes_have_bounded_validation();
    return 0;
}

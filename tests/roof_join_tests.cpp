#include "sketch/architecture.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/roof_join_semantics.hpp"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>

#include <cmath>
#include <limits>
#include <numbers>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sketch::Entity;
using sketch::RoofJoin;
using sketch::RoofJoinStyle;
using sketch::SlopedRoofPanel;
using Json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

SlopedRoofPanel panel(std::string id, double x) {
    return {std::move(id), {x, 0.0, 0.0}, 0.0, 2.0, 4.0, 0.0, 0.0,
            0.0, 0.2, {}};
}

Json join_properties(std::vector<std::string> ids) {
    return {{"version", 1}, {"style", "fused"}, {"roof_ids", std::move(ids)}};
}

void test_join_codec_is_versioned_and_lossless() {
    const auto encoded = join_properties({"roof-a", "roof-b"});
    const auto decoded = sketch::parse_roof_join(encoded, "join-1");
    require(decoded.id == "join-1" && decoded.style == RoofJoinStyle::fused &&
                decoded.roof_ids == std::vector<std::string>{"roof-a", "roof-b"},
            "roof join codec changed the canonical identity or ordering");
    require(sketch::roof_join_json(decoded) == encoded,
            "roof join JSON did not round-trip canonically");

    auto malformed = encoded;
    malformed["roof_ids"] = Json::array({"roof-a", "roof-a"});
    rejected([&] { (void)sketch::parse_roof_join(malformed, "join-1"); },
             "duplicate roof IDs must be rejected");
    malformed = encoded;
    malformed["version"] = 3;
    rejected([&] { (void)sketch::parse_roof_join(malformed, "join-1"); },
             "unsupported roof join versions must be rejected");
    malformed = encoded;
    malformed["style"] = "miter";
    rejected([&] { (void)sketch::parse_roof_join(malformed, "join-1"); },
             "unsupported roof join styles must be rejected");
}

void test_join_material_v2_codec_is_closed() {
    auto encoded = join_properties({"roof-a", "roof-b"});
    encoded["version"] = 2;
    encoded["material_assignment"] = {{"version", 1}, {"catalog_id", "catalog"}, {"material_id", "red"}};
    const auto join = sketch::parse_roof_join(encoded, "join-1");
    require(join.material_assignment && join.material_assignment->material_id == "red" &&
        sketch::roof_join_json(join) == encoded, "V2 assignment must round trip with authored member order");
    for (const auto& malformed : std::vector<Json>{
        Json{{"version", 1}, {"style", "fused"}, {"roof_ids", {"roof-a", "roof-b"}},
            {"material_assignment", encoded["material_assignment"]}},
        Json{{"version", 2}, {"style", "fused"}, {"roof_ids", {"roof-a", "roof-b"}},
            {"material_assignment", {{"catalog_id", "catalog"}, {"material_id", "red"}, {"version", 2}}}},
        Json{{"version", 2}, {"style", "fused"}, {"roof_ids", {"roof-a", "roof-b"}},
            {"material_assignment", {{"catalog_id", ""}, {"material_id", "red"}}}},
        Json{{"version", 2}, {"style", "fused"}, {"roof_ids", {"roof-a", "roof-b"}}, {"extra", true}}})
        rejected([&] { (void)sketch::parse_roof_join(malformed, "join-1"); },
            "malformed or unsupported V2 assignment must be refused");
    encoded["material_assignment"]["material_id"] = std::string(129, 'a');
    rejected([&] { (void)sketch::parse_roof_join(encoded, "join-1"); }, "assignment ID bounds must be enforced");
    encoded["material_assignment"]["material_id"] = "red";
    encoded["material_assignment"]["unexpected"] = true;
    rejected([&] { (void)sketch::parse_roof_join(encoded, "join-1"); }, "unknown assignment fields must be refused");
    encoded.erase("material_assignment");
    require(sketch::roof_join_json(sketch::parse_roof_join(encoded, "join-1"))["version"] == 1,
        "an unassigned join retains canonical V1 storage");
}

double independent_common(const TopoDS_Shape& first, const TopoDS_Shape& second) {
    BRepAlgoAPI_Common common(first, second);
    common.Build();
    require(common.IsDone() && !common.HasErrors(), "independent common must succeed");
    return sketch::solid_volume(common.Shape());
}

void test_ordered_material_partition_uses_real_disjoint_solids() {
    SlopedRoofPanel a{"roof-a", {0,0,0}, 0, 2, 4, 1, std::atan(.5), 0, .1, {}};
    auto b = a; b.id = "roof-b"; b.base_position = {1.9,0,.95};
    const auto sa = sketch::make_sloped_roof_panel(a), sb = sketch::make_sloped_roof_panel(b);
    const auto va = sketch::solid_volume(sa), vb = sketch::solid_volume(sb);
    const auto overlap = independent_common(sa, sb);
    require(overlap > 0, "sloped fixture must have positive overlap");
    const RoofJoin join{"join", {a.id,b.id}};
    const auto partition = sketch::make_roof_join_partition(join, std::vector<TopoDS_Shape>{sa,sb});
    require(partition.regions.size() == 2 && std::abs(partition.fused_volume - (va+vb-overlap)) < 1e-8 &&
        std::abs(partition.regions[0].net_volume - va) < 1e-8 &&
        std::abs(partition.regions[1].net_volume - (vb-overlap)) < 1e-8,
        "independent source/common volumes must match deterministic region ownership");
    double total = 0;
    for (const auto& region : partition.regions) {
        require(!region.shape.IsNull() && BRepCheck_Analyzer(region.shape).IsValid() &&
            TopExp_Explorer(region.shape, TopAbs_SOLID).More(), "positive regions must contain real valid solids");
        total += sketch::solid_volume(region.shape);
    }
    require(std::abs(total-partition.fused_volume) < 1e-8 &&
        independent_common(partition.regions[0].shape,partition.regions[1].shape) < 1e-8,
        "independent region volumes conserve union and contain no shared material");
    const auto reversed = sketch::make_roof_join_partition(RoofJoin{"reverse", {b.id,a.id}},
        std::vector<TopoDS_Shape>{sb,sa});
    require(std::abs(reversed.fused_volume-partition.fused_volume) < 1e-8 &&
        std::abs(reversed.regions[1].net_volume-(va-overlap)) < 1e-8,
        "order reversal changes ownership but preserves fused truth");
    const auto covered = sketch::make_roof_join_partition(join, std::vector<TopoDS_Shape>{sa,sa});
    require(covered.regions[1].shape.IsNull() && covered.regions[1].net_volume == 0 &&
        std::abs(covered.fused_volume-va) < 1e-8, "fully covered member remains a zero quantity provenance region");
    rejected([&] { (void)sketch::make_roof_join_partition(join, std::vector<TopoDS_Shape>{sa,{}}); },
        "null source must refuse the entire partition");
    b.base_position.x = std::numeric_limits<double>::infinity();
    rejected([&] { (void)sketch::make_sloped_roof_panel(b); }, "nonfinite source must fail before partition");
}

void test_partition_preserves_gable_compound_and_openings() {
    sketch::GableRoof gable{"gable", {0,0,0}, 0, 4, 4, 1, std::atan(.5), 0, .1,
        {{"void", -.5,-1.5,.5,.5}}};
    SlopedRoofPanel extension{"panel", {1.9,-2,0}, std::numbers::pi/2, 2, 2, 1,
        std::atan(.5), 0, .1, {}};
    const auto sg = sketch::make_gable_roof(gable), sp = sketch::make_sloped_roof_panel(extension);
    const auto partition = sketch::make_roof_join_partition(RoofJoin{"gable-join", {gable.id,extension.id}},
        std::vector<TopoDS_Shape>{sg,sp});
    require(std::abs(partition.fused_volume - (sketch::solid_volume(sg)+sketch::solid_volume(sp)-
        independent_common(sg,sp))) < 1e-8, "compound/opening join must conserve actual union quantity");
    auto no_opening = gable; no_opening.openings.clear();
    require(partition.regions[0].gross_volume < sketch::solid_volume(sketch::make_gable_roof(no_opening)),
        "member openings must survive region geometry and gross quantity");
}

void test_fused_join_requires_touching_roofs_and_returns_real_solid() {
    const auto first = panel("roof-a", 0.0);
    const auto second = panel("roof-b", 1.9);
    const auto first_shape = sketch::make_building_shape(first);
    const auto second_shape = sketch::make_building_shape(second);
    const RoofJoin join{"join-1", {"roof-a", "roof-b"}, RoofJoinStyle::fused};
    const auto shape = sketch::make_roof_join(join,
                                               std::vector<TopoDS_Shape>{first_shape, second_shape});
    const auto volume = sketch::solid_volume(shape);
    const auto source_volume = sketch::solid_volume(first_shape) +
                               sketch::solid_volume(second_shape);
    require(!shape.IsNull() && std::isfinite(volume) && volume > 0.0,
            "fused roof join did not produce a valid solid");
    require(volume <= source_volume + 1e-8,
            "fused roof join volume exceeded its source roof volumes");

    const auto disconnected = panel("roof-c", 20.0);
    const RoofJoin invalid{"join-2", {"roof-a", "roof-c"}, RoofJoinStyle::fused};
    rejected([&] {
        (void)sketch::make_roof_join(
            invalid, std::vector<TopoDS_Shape>{first_shape, sketch::make_building_shape(disconnected)});
    }, "disconnected roofs must not be accepted as a join");
}

void test_join_rejects_two_disconnected_pairs() {
    const std::vector<TopoDS_Shape> roofs{
        sketch::make_building_shape(panel("roof-a", 0.0)),
        sketch::make_building_shape(panel("roof-b", 1.9)),
        sketch::make_building_shape(panel("roof-c", 20.0)),
        sketch::make_building_shape(panel("roof-d", 21.9))};
    const RoofJoin join{"join-pairs", {"roof-a", "roof-b", "roof-c", "roof-d"},
                        RoofJoinStyle::fused};
    rejected([&] { (void)sketch::make_roof_join(join, roofs); },
             "two disconnected roof pairs must not be accepted as one join");
}

void test_join_accepts_chain_in_nonadjacent_order() {
    const std::vector<TopoDS_Shape> roofs{
        sketch::make_building_shape(panel("roof-a", 0.0)),
        sketch::make_building_shape(panel("roof-c", 3.8)),
        sketch::make_building_shape(panel("roof-b", 1.9))};
    const RoofJoin join{"join-chain", {"roof-a", "roof-c", "roof-b"}, RoofJoinStyle::fused};
    const auto shape = sketch::make_roof_join(join, roofs);
    const auto volume = sketch::solid_volume(shape);
    require(!shape.IsNull() && std::isfinite(volume) && volume > 0.0,
            "transitively connected roof chain must produce a solid regardless of member order");
    const auto partition = sketch::make_roof_join_partition(join, roofs);
    require(std::abs(partition.fused_volume-volume) < 1e-8,
        "partition preserves transitive connection with a disconnected authored prefix");
}

void test_document_validates_join_references_and_persists_record() {
    const auto first = sketch::encode_building_entity(panel("roof-a", 0.0));
    const auto second = sketch::encode_building_entity(panel("roof-b", 1.9));
    const Entity join{"join-1", "roof_join", join_properties({"roof-a", "roof-b"}), false,
                      Json::object()};
    const auto document = sketch::Document::create({first, second, join});
    require(document.snapshot().entities().at("join-1") == join,
            "document changed the roof join record during admission");

    auto missing = join;
    missing.properties["roof_ids"] = Json::array({"roof-a", "missing-roof"});
    rejected([&] { (void)sketch::Document::create({first, second, missing}); },
             "document accepted a roof join with a dangling roof reference");

    auto wrong_type = join;
    wrong_type.properties["roof_ids"] = Json::array({"roof-a", "property-1"});
    Entity property{"property-1", "property", Json::object(), false, Json::object()};
    rejected([&] { (void)sketch::Document::create({first, property, wrong_type}); },
             "document accepted a roof join whose target is not a roof");
}

}  // namespace

int main() {
    try {
        test_join_codec_is_versioned_and_lossless();
        test_join_material_v2_codec_is_closed();
        test_ordered_material_partition_uses_real_disjoint_solids();
        test_partition_preserves_gable_compound_and_openings();
        test_fused_join_requires_touching_roofs_and_returns_real_solid();
        test_join_accepts_chain_in_nonadjacent_order();
        test_join_rejects_two_disconnected_pairs();
        test_document_validates_join_references_and_persists_record();
        std::cout << "Roof join tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

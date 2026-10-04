#include "sketch/annotation_catalog.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
template<class F> void rejected(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    require(false,"Expected invalid_argument");
}
}

int main() {
    using namespace sketch;
    const auto catalog = default_symbol_catalog();
    {
        const auto svg = filter_symbol_catalog(catalog, "Basin Oval", "01_bathroom").front();
        AnnotationState palette_state;
        palette_state.symbols.push_back({"palette-symbol", svg.id,
            {{4, 7}, 0.8, 1.5, "ground"}, {}, false});
        auto& symbol = palette_state.symbols.front();
        symbol.definition = svg;
        symbol.pinned_svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"><path fill=\"#ffffff\" d=\"M0 0L1 1\"/></svg>";
        symbol.width_scale = 1.7;
        symbol.depth_scale = 0.4;
        symbol.flip_horizontal = symbol.flip_vertical = true;
        const auto original_wire = encode_annotation_state(palette_state, catalog);
        require(original_wire.at("version") == 3 && original_wire["symbols"][0].size() == 11 &&
                !original_wire["symbols"][0].contains("svg_palette") &&
                !decode_annotation_state(original_wire, catalog).symbols.front().svg_palette,
            "Absent palettes must retain version 3 representation without materializing defaults");
        const auto original_geometry = transformed_symbol_preview(svg, symbol);
        auto palette_wire = original_wire;
        palette_wire["version"] = 7;
        palette_wire["symbols"][0]["svg_palette"] = {{"version", 1},
            {"profile", "white-outline-2"}, {"outline_color", "#123456"},
            {"surface_color", "#AbCdEf"}};
        const auto palette_decoded = decode_annotation_state(palette_wire, catalog);
        require(palette_decoded.symbols.front().svg_palette ==
                    SymbolSvgPalette{"white-outline-2", "#123456", "#AbCdEf"} &&
                encode_annotation_state(palette_decoded, catalog) == palette_wire,
            "Explicit SVG palette must survive version 7 decode and re-encode");
        auto without_palette = palette_wire;
        without_palette["version"] = 3;
        without_palette["symbols"][0].erase("svg_palette");
        require(without_palette == original_wire &&
                palette_decoded.symbols.front().pinned_svg == symbol.pinned_svg &&
                palette_wire["symbols"][0]["definition"] == original_wire["symbols"][0]["definition"],
            "Palette metadata must preserve exact pinned bytes, hash, definition and instance properties");
        const auto palette_geometry = transformed_symbol_preview(svg, palette_decoded.symbols.front());
        require(palette_geometry.size() == original_geometry.size(), "Palette changed preview stroke count");
        for (std::size_t index = 0; index < original_geometry.size(); ++index) {
            require(palette_geometry[index].start.x == original_geometry[index].start.x &&
                    palette_geometry[index].start.y == original_geometry[index].start.y &&
                    palette_geometry[index].end.x == original_geometry[index].end.x &&
                    palette_geometry[index].end.y == original_geometry[index].end.y,
                "Palette must preserve rotated, resized and mirrored geometry exactly");
        }
        symbol.svg_palette = SymbolSvgPalette{};
        const auto defaults_wire = encode_annotation_state(palette_state, catalog);
        require(defaults_wire.at("version") == 7 && defaults_wire["symbols"][0]["svg_palette"] ==
                nlohmann::json({{"version", 1}, {"profile", "white-outline-2"},
                    {"outline_color", "#111111"}, {"surface_color", "#ffffff"}}) &&
                decode_annotation_state(defaults_wire, catalog).symbols.front().svg_palette == SymbolSvgPalette{},
            "Explicit default palette colors must remain explicit author intent");
        auto mixed = palette_state;
        mixed.symbols.push_back({"procedural-sibling", catalog.front().id, {}, {}, true});
        const auto mixed_wire = encode_annotation_state(mixed, catalog);
        require(mixed_wire["symbols"][0].size() == 12 && mixed_wire["symbols"][1].size() == 11 &&
                !mixed_wire["symbols"][1].contains("svg_palette") &&
                encode_annotation_state(decode_annotation_state(mixed_wire, catalog), catalog) == mixed_wire,
            "Version 7 permits palette and uncolored siblings without adding defaults");
        auto v7_absent = original_wire;
        v7_absent["version"] = 7;
        require(encode_annotation_state(decode_annotation_state(v7_absent, catalog), catalog) == original_wire,
            "Version 7 without an explicit palette retains the legacy encoder version selection");
        for (const auto version : {3, 4, 5, 6}) {
            AnnotationState legacy_state = palette_state;
            legacy_state.symbols.front().svg_palette.reset();
            if (version >= 4) {
                legacy_state.overrides.push_back({"area", "area-1", {}, true});
                legacy_state.overrides.front().plan_label_offset = Vec2{1, 2};
            }
            if (version >= 5) {
                legacy_state.labels.push_back(instantiate_label(default_label_templates().front(), "label-1"));
                legacy_state.labels.front().model_plan = true;
            }
            if (version >= 6) legacy_state.overrides.push_back({"wall_dimension", "wall-1", {}, true});
            const auto old_wire = encode_annotation_state(legacy_state, catalog);
            require(old_wire.at("version") == version && !old_wire["symbols"][0].contains("svg_palette") &&
                    encode_annotation_state(decode_annotation_state(old_wire, catalog), catalog) == old_wire,
                "Palette absence must preserve each existing version selection and roundtrip");
            legacy_state.symbols.front().svg_palette = SymbolSvgPalette{};
            const auto new_wire = encode_annotation_state(legacy_state, catalog);
            require(new_wire.at("version") == 7 &&
                    encode_annotation_state(decode_annotation_state(new_wire, catalog), catalog) == new_wire,
                "Explicit palette takes version 7 precedence over existing presentation features");
        }
        for (int version = 1; version <= 6; ++version) {
            auto old = palette_wire;
            old["version"] = version;
            if (version <= 2)
                for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
                    old["symbols"][0].erase(key);
            if (version == 1) {
                old["symbols"][0].erase("definition");
                old["symbols"][0].erase("pinned_svg");
            }
            rejected([&] { (void)decode_annotation_state(old, catalog); });
        }
        const auto reject_palette = [&](const nlohmann::json& value) {
            auto malformed = palette_wire;
            malformed["symbols"][0]["svg_palette"] = value;
            rejected([&] { (void)decode_annotation_state(malformed, catalog); });
        };
        const auto envelope = palette_wire["symbols"][0]["svg_palette"];
        for (const auto value : {nlohmann::json(nullptr), nlohmann::json(true), nlohmann::json(1),
                nlohmann::json("palette"), nlohmann::json::array(), nlohmann::json::object()})
            reject_palette(value);
        for (const auto* field : {"version", "profile", "outline_color", "surface_color"}) {
            auto malformed = envelope;
            malformed.erase(field);
            reject_palette(malformed);
            malformed["unknown"] = "replacement";
            reject_palette(malformed);
        }
        auto extra = envelope;
        extra["unknown"] = true;
        reject_palette(extra);
        for (const auto value : {nlohmann::json(0), nlohmann::json(2), nlohmann::json(-1),
                nlohmann::json(1.0), nlohmann::json("1"), nlohmann::json(true), nlohmann::json(nullptr)}) {
            auto malformed = envelope;
            malformed["version"] = value;
            reject_palette(malformed);
        }
        for (const auto* field : {"profile", "outline_color", "surface_color"}) {
            for (const auto value : {nlohmann::json(nullptr), nlohmann::json(123), nlohmann::json(true),
                    nlohmann::json::array(), nlohmann::json::object()}) {
                auto malformed = envelope;
                malformed[field] = value;
                reject_palette(malformed);
            }
        }
        for (const auto* profile : {"", "white-outline-1", "WHITE-OUTLINE-2", "white-outline-2 "}) {
            auto malformed = envelope;
            malformed["profile"] = profile;
            reject_palette(malformed);
            auto invalid = palette_state;
            invalid.symbols.front().svg_palette->profile = profile;
            rejected([&] { validate_annotation_state(invalid, catalog); });
        }
        for (const auto* field : {"outline_color", "surface_color"}) {
            for (const auto* value : {"", "red", "#123", "123456", "#12345678", "#12G456", "#123456 "}) {
                auto malformed = envelope;
                malformed[field] = value;
                reject_palette(malformed);
                auto invalid = palette_state;
                if (std::string_view(field) == "outline_color") invalid.symbols.front().svg_palette->outline_color = value;
                else invalid.symbols.front().svg_palette->surface_color = value;
                rejected([&] { validate_annotation_state(invalid, catalog); });
            }
        }
        auto unknown_symbol_key = palette_wire;
        unknown_symbol_key["symbols"][0]["unknown"] = true;
        rejected([&] { (void)decode_annotation_state(unknown_symbol_key, catalog); });
        unknown_symbol_key["symbols"][0].erase("id");
        rejected([&] { (void)decode_annotation_state(unknown_symbol_key, catalog); });
        auto procedural_wire = mixed_wire;
        procedural_wire["symbols"][1]["svg_palette"] = envelope;
        rejected([&] { (void)decode_annotation_state(procedural_wire, catalog); });
        auto procedural_state = mixed;
        procedural_state.symbols.back().svg_palette = SymbolSvgPalette{};
        rejected([&] { validate_annotation_state(procedural_state, catalog); });
        auto saved_procedural = palette_state;
        saved_procedural.symbols.front().definition->svg_asset.reset();
        saved_procedural.symbols.front().pinned_svg.clear();
        rejected([&] { validate_annotation_state(saved_procedural, catalog); });
        auto resolved_svg = palette_state;
        resolved_svg.symbols.front().definition.reset();
        resolved_svg.symbols.front().pinned_svg.clear();
        require(encode_annotation_state(resolved_svg, catalog).at("version") == 7,
            "Catalog SVG definitions qualify without requiring an existing saved snapshot");
        require(encode_annotation_state(decode_annotation_state(palette_wire, {}), {}) == palette_wire,
            "Saved SVG palette intent survives removal of the installed catalog definition");
        auto stale_catalog = svg;
        ++stale_catalog.artwork_revision;
        auto stale_palette = palette_state;
        stale_palette.symbols.front().pinned_svg.clear();
        validate_annotation_state(stale_palette, {stale_catalog});
        require(!resolved_symbol_definition(stale_palette.symbols.front(), {stale_catalog}).svg_asset &&
                stale_palette.symbols.front().svg_palette == SymbolSvgPalette{},
            "Unavailable stale SVG source preserves saved palette intent for explicit migration");
        const auto migrated = migrate_symbol_definition(stale_palette, "palette-symbol", {stale_catalog}, "<svg/>");
        require(migrated.symbols.front().svg_palette == SymbolSvgPalette{} &&
                !symbol_requires_migration(migrated.symbols.front(), {stale_catalog}),
            "Explicit artwork migration preserves palette intent");
    }
    {
        AnnotationState dimension_state;
        PresentationOverride dimension;
        dimension.target_kind="wall_dimension";
        dimension.target_id="wall-a";
        dimension.visible=false;
        dimension.style.font_family="Inter";
        dimension.style.stroke_color="#123456";
        dimension.style.bold=true;
        dimension.plan_label_offset=Vec2{0.75,-0.25};
        dimension.paper_text_height_mm=4.5;
        dimension.plan_label_rotation_radians=std::numbers::pi/4;
        dimension_state.overrides.push_back(dimension);
        const auto dimension_wire=encode_annotation_state(dimension_state,catalog);
        require(dimension_wire.at("version")==6 &&
            encode_annotation_state(decode_annotation_state(dimension_wire,catalog),catalog)==dimension_wire,
            "Independent automatic wall-dimension presentation must round-trip as annotation v6");
        AnnotationState minimal_dimension_state;
        minimal_dimension_state.overrides.push_back({"wall_dimension","wall-default",{},true});
        const auto minimal_dimension_wire=encode_annotation_state(minimal_dimension_state,catalog);
        require(minimal_dimension_wire.at("version")==6 &&
            encode_annotation_state(decode_annotation_state(minimal_dimension_wire,catalog),catalog)==minimal_dimension_wire,
            "A wall dimension target alone must require v6 without materializing optional settings");
        for(int version=1;version<=5;++version) {
            auto legacy=dimension_wire;legacy["version"]=version;
            rejected([&]{(void)decode_annotation_state(legacy,catalog);});
        }
        for(const auto* field:{"paper_text_height_mm","plan_label_rotation_radians"}) {
            for(const auto invalid:{nlohmann::json("4.5"),nlohmann::json(true),nlohmann::json(nullptr),
                nlohmann::json(std::numeric_limits<double>::infinity())}) {
                auto bad=dimension_wire;bad["overrides"][0][field]=invalid;
                rejected([&]{(void)decode_annotation_state(bad,catalog);});
            }
        }
        for(const auto invalid:{0.0,-1.0,100.001}) {
            auto bad=dimension_wire;bad["overrides"][0]["paper_text_height_mm"]=invalid;
            rejected([&]{(void)decode_annotation_state(bad,catalog);});
        }
        auto boundary_height=dimension_wire;boundary_height["overrides"][0]["paper_text_height_mm"]=100.0;
        boundary_height["overrides"][0]["plan_label_rotation_radians"]=-12.0*std::numbers::pi;
        require(encode_annotation_state(decode_annotation_state(boundary_height,catalog),catalog)==boundary_height,
            "Wall dimension height accepts its inclusive maximum and finite rotations remain unnormalized");
        for(const auto* kind:{"area","object","output_view"}) {
            for(const auto* field:{"paper_text_height_mm","plan_label_rotation_radians"}) {
                auto bad=dimension_wire;auto& record=bad["overrides"][0];
                record["target_kind"]=kind;record.erase("plan_label_offset_m");
                record.erase(field==std::string_view("paper_text_height_mm") ? "plan_label_rotation_radians" : "paper_text_height_mm");
                rejected([&]{(void)decode_annotation_state(bad,catalog);});
            }
        }
        for(const auto* field:{"hatch_scale","paper_line_width_mm"}) {
            auto bad=dimension_wire;bad["overrides"][0][field]=1.0;
            rejected([&]{(void)decode_annotation_state(bad,catalog);});
        }
        auto inherited=dimension_wire;inherited["overrides"][0]["inherit_appearance"]=true;
        require(encode_annotation_state(decode_annotation_state(inherited,catalog),catalog)==inherited,
            "Placement-only wall dimensions must retain theme-safe appearance inheritance");
        inherited["overrides"][0]["inherit_appearance"]=false;
        require(!decode_annotation_state(inherited,catalog).overrides.front().inherit_appearance,
            "Explicit wall dimension appearance overrides must admit a false inheritance mode");
        for(const auto* kind:{"object","output_view"}) {
            for(const auto mode:{false,true}) {
                auto bad=dimension_wire;auto& record=bad["overrides"][0];record["target_kind"]=kind;
                record.erase("plan_label_offset_m");record.erase("paper_text_height_mm");record.erase("plan_label_rotation_radians");
                record["inherit_appearance"]=mode;rejected([&]{(void)decode_annotation_state(bad,catalog);});
            }
        }
        auto bad=dimension_wire;bad["overrides"][0]["plan_label_offset_m"]={1.0,std::numeric_limits<double>::infinity()};
        rejected([&]{(void)decode_annotation_state(bad,catalog);});
        bad=dimension_wire;bad["overrides"].push_back(bad["overrides"][0]);
        rejected([&]{(void)decode_annotation_state(bad,catalog);});
        bad=dimension_wire;bad["overrides"][0]["style"]["stroke_color"]="invalid";
        rejected([&]{(void)decode_annotation_state(bad,catalog);});
        auto mixed=dimension_state;
        auto label=instantiate_label(default_label_templates().front(),"world-plan-label");label.model_plan=true;
        mixed.labels.push_back(label);
        PresentationOverride area;area.target_kind="area";area.target_id="area-a";
        area.plan_label_offset=Vec2{0.125,0.25};area.inherit_appearance=true;mixed.overrides.push_back(area);
        const auto mixed_wire=encode_annotation_state(mixed,catalog);
        require(mixed_wire.at("version")==6 && mixed_wire.at("labels")[0].at("model_plan")==true &&
            encode_annotation_state(decode_annotation_state(mixed_wire,catalog),catalog)==mixed_wire,
            "v6 wall dimensions, v5 world-plan text and v4 area placement must coexist without loss");
        auto invalid_model_plan=mixed_wire;invalid_model_plan["labels"][0]["model_plan"]=1;
        rejected([&]{(void)decode_annotation_state(invalid_model_plan,catalog);});
        mixed.overrides.erase(mixed.overrides.begin());
        require(encode_annotation_state(mixed,catalog).at("version")==5,"removing wall presentation must retain v5 plan labels");
        mixed.labels.clear();require(encode_annotation_state(mixed,catalog).at("version")==4,"area placement alone must retain v4");
        mixed.overrides.clear();require(encode_annotation_state(mixed,catalog).at("version")==3,"ordinary annotation state must retain v3");
    }
    {
        AnnotationState presentation;
        presentation.overrides.push_back({"area", "room-a", {}, true});
        auto wire = encode_annotation_state(presentation, catalog);
        wire["version"] = 4;
        wire["overrides"][0]["plan_label_offset_m"] = {0.125, -2.5};
        bool retained = false;
        try {
            retained = encode_annotation_state(decode_annotation_state(wire, catalog), catalog) == wire;
        } catch (const std::invalid_argument&) {}
        require(retained, "Authored plan label offsets must round-trip without changing area styles or values");
        for (const auto invalid : {nlohmann::json::array({1.0}),
                                  nlohmann::json::array({1.0, 2.0, 3.0}),
                                  nlohmann::json::array({"1", 2.0}),
                                  nlohmann::json::array({std::numeric_limits<double>::infinity(), 2.0}),
                                  nlohmann::json(nullptr)}) {
            auto bad = wire; bad["overrides"][0]["plan_label_offset_m"] = invalid;
            rejected([&] { (void)decode_annotation_state(bad, catalog); });
        }
        for (const auto* kind : {"object", "output_view"}) {
            auto bad = wire; bad["overrides"][0]["target_kind"] = kind;
            rejected([&] { (void)decode_annotation_state(bad, catalog); });
        }
        auto legacy = wire; legacy["version"] = 3;
        rejected([&] { (void)decode_annotation_state(legacy, catalog); });
        wire["overrides"][0].erase("plan_label_offset_m");
        require(encode_annotation_state(decode_annotation_state(wire, catalog), catalog)["version"] == 3,
                "Removing the last placement override must retain compatibility with the ordinary annotation version");
        wire["overrides"][0]["inherit_appearance"] = true;
        require(encode_annotation_state(decode_annotation_state(wire,catalog),catalog)==wire,
                "Placement-only records must preserve semantic appearance inheritance");
        auto invalid_inheritance=wire; invalid_inheritance["overrides"][0]["inherit_appearance"]="true";
        rejected([&]{(void)decode_annotation_state(invalid_inheritance,catalog);});
        invalid_inheritance=wire; invalid_inheritance["version"]=3;
        rejected([&]{(void)decode_annotation_state(invalid_inheritance,catalog);});
        invalid_inheritance=wire; invalid_inheritance["overrides"][0]["target_kind"]="object";
        rejected([&]{(void)decode_annotation_state(invalid_inheritance,catalog);});
    }
    {
        AnnotationState presentation;
        presentation.overrides.push_back({"area", "area-a", {}, true});
        auto wire = encode_annotation_state(presentation, catalog);
        wire["overrides"][0]["paper_line_width_mm"] = 0.75;
        wire["overrides"][0]["hatch_scale"] = 2.5;
        require(encode_annotation_state(decode_annotation_state(wire, catalog), catalog) == wire,
                "Area appearance must preserve explicit paper line width and hatch scale through persistence");
        for (const auto* key : {"paper_line_width_mm", "hatch_scale"}) {
            for (const auto invalid : {0.0, -1.0, 11.0, std::numeric_limits<double>::infinity()}) {
                auto bad = wire; bad["overrides"][0][key] = invalid;
                rejected([&] { (void)decode_annotation_state(bad, catalog); });
            }
            auto bad = wire; bad["overrides"][0][key] = "invalid";
            rejected([&] { (void)decode_annotation_state(bad, catalog); });
        }
        const auto legacy = encode_annotation_state(presentation, catalog);
        require(!legacy["overrides"][0].contains("paper_line_width_mm") &&
                !legacy["overrides"][0].contains("hatch_scale") &&
                encode_annotation_state(decode_annotation_state(legacy, catalog), catalog) == legacy,
                "legacy styles must retain absent optional presentation defaults exactly");
    }
    {
        AnnotationState placed;
        placed.symbols.push_back({"sized", catalog.front().id, {}, {}, true});
        const auto saved = encode_annotation_state(placed, catalog);
        require(saved.at("version") == 3 && saved.at("symbols")[0].contains("width_scale") &&
                    saved.at("symbols")[0].contains("depth_scale") &&
                    saved.at("symbols")[0].contains("flip_horizontal") &&
                    saved.at("symbols")[0].contains("flip_vertical"),
                "Symbol independent dimensions and mirrors must have a versioned persisted representation");
    }
    {
        const auto original = filter_symbol_catalog(catalog, "Toilet Close Coupled", "01_bathroom").front();
        AnnotationState placed;
        placed.symbols.push_back({"pinned", original.id, {{3, 4}, 0.5, 1.4, "ground"}, {}, false});
        const auto saved = encode_annotation_state(placed, {original});
        require(saved.at("symbols")[0].contains("definition"), "Placed symbols must persist a definition snapshot");
        auto hash_changed = original;
        hash_changed.svg_asset->sha256[0] =
            hash_changed.svg_asset->sha256[0] == '0' ? '1' : '0';
        const auto hash_reopened = decode_annotation_state(saved, {hash_changed});
        require(symbol_requires_migration(hash_reopened.symbols.front(), {hash_changed}),
                "Artwork digest change must require explicit migration without a revision bump");
        auto updated = original;
        updated.artwork_revision = 2;
        updated.name = "Revised toilet";
        auto reopened = decode_annotation_state(saved, {updated});
        auto legacy_saved = saved;
        legacy_saved["version"] = 1;
        legacy_saved["symbols"][0].erase("definition");
        legacy_saved["symbols"][0].erase("pinned_svg");
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
            legacy_saved["symbols"][0].erase(key);
        require(encode_annotation_state(decode_annotation_state(legacy_saved, {original}), {original}) == saved,
                "Legacy state must pin its original definition on upgrade");
        rejected([&] { (void)decode_annotation_state(legacy_saved, {updated}); });
        require(symbol_requires_migration(reopened.symbols.front(), {updated}), "Revision change must be flagged");
        require(!resolved_symbol_definition(reopened.symbols.front(), {updated}).svg_asset,
                "Stale path must never load replacement artwork");
        require(resolved_symbol_definition(reopened.symbols.front(), {updated}).name == original.name,
                "Reopen must retain original definition");
        require(encode_annotation_state(reopened, {updated}) == saved, "Saving must not migrate a pinned instance");
        const auto migrated = migrate_symbol_definition(reopened, "pinned", {updated}, "<svg/>");
        require(!symbol_requires_migration(migrated.symbols.front(), {updated}) &&
                    migrated.symbols.front().pinned_svg == "<svg/>", "Explicit migration must pin new artwork");
        const auto migrated_saved = encode_annotation_state(migrated, {updated});
        auto pinned_v2 = migrated_saved;
        pinned_v2["version"] = 2;
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
            pinned_v2["symbols"][0].erase(key);
        const auto pinned_upgrade = decode_annotation_state(pinned_v2, {original});
        require(pinned_upgrade.symbols.front().pinned_svg == "<svg/>" &&
                    encode_annotation_state(pinned_upgrade, {original}) == migrated_saved,
                "Version2 upgrade must retain exact SVG bytes and historical definition when catalog artwork differs");
        auto migrated_json = encode_annotation_state(migrated, {updated});
        migrated_json["symbols"][0]["definition"] = saved["symbols"][0]["definition"];
        migrated_json["symbols"][0]["pinned_svg"] = "";
        require(migrated_json == saved, "Migration changed instance properties");
        require(symbol_requires_migration(reopened.symbols.front(), {}), "Removed definitions must be flagged");
        require(encode_annotation_state(decode_annotation_state(saved, {}), {}) == saved,
                "Removed definitions must still reopen and save");
        rejected([&] { (void)migrate_symbol_definition(reopened, "pinned", {}); });
        updated.maximum_scale = 1;
        rejected([&] { (void)migrate_symbol_definition(reopened, "pinned", {updated}); });
        auto malformed_pin = saved;
        malformed_pin["symbols"][0]["definition"]["id"] = "different";
        rejected([&] { (void)decode_annotation_state(malformed_pin, {original}); });
        malformed_pin = saved;
        malformed_pin["symbols"][0]["pinned_svg"] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>";
        rejected([&] { (void)decode_annotation_state(malformed_pin, {original}); });
        malformed_pin["symbols"][0]["pinned_svg"] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><path fill=\"url(https://example.test/a)\"/></svg>";
        rejected([&] { (void)decode_annotation_state(malformed_pin, {original}); });
    }
    {
        // An off-origin, asymmetric marker catches use of the wrong anchor,
        // world-axis reflection, and scaling after rotation.
        SymbolDefinition marker{"marker", "markers", "test", 4, 6, {2, -3},
                                0.01, 100, {{{3, -1}, {0, -4}}}};
        SymbolInstance instance{"marker-1", marker.id, {{10, 20}, std::numbers::pi / 2, 2}, {}, true};
        instance.width_scale = 1.5;
        instance.depth_scale = 0.25;
        const auto close = [](Vec2 actual, Vec2 expected) {
            return std::abs(actual.x - expected.x) < 1e-12 &&
                   std::abs(actual.y - expected.y) < 1e-12;
        };
        const auto assert_stroke = [&](Vec2 start, Vec2 end) {
            const auto result = transformed_symbol_preview(marker, instance);
            require(result.size() == 1 && close(result[0].start, start) && close(result[0].end, end),
                    "Symbol transform must resize and mirror local axes about the saved anchor before rotation");
            require(close(transformed_symbol_point(marker, instance, marker.preview[0].start), start),
                    "Footprint point transform must agree with artwork geometry");
            require(close(transformed_symbol_point(marker, instance, marker.anchor), {10, 20}),
                    "Independent dimensions and mirrors must preserve the placement anchor");
        };
        assert_stroke({9, 23}, {10.5, 14});
        instance.flip_horizontal = true;
        assert_stroke({9, 17}, {10.5, 26});
        instance.flip_horizontal = false;
        instance.flip_vertical = true;
        assert_stroke({11, 23}, {9.5, 14});
        instance.flip_horizontal = true;
        assert_stroke({11, 17}, {9.5, 26});
        AnnotationState edited;
        edited.symbols.push_back(instance);
        const auto saved = encode_annotation_state(edited, {marker});
        require(encode_annotation_state(decode_annotation_state(saved, {}), {}) == saved,
                "Independent dimensions and both mirrors must roundtrip with a removed pinned catalog definition");
        for (const auto invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(), 0.001, 100.0}) {
            auto bad = instance;
            bad.width_scale = invalid;
            rejected([&] { (void)transformed_symbol_preview(marker, bad); });
            bad = instance;
            bad.depth_scale = invalid;
            rejected([&] { (void)transformed_symbol_point(marker, bad, marker.anchor); });
        }
        // Limits apply to each effective axis, not to the independent factor.
        auto within_limits = instance;
        within_limits.placement.scale = 200;
        within_limits.width_scale = within_limits.depth_scale = 0.25;
        require(!transformed_symbol_preview(marker, within_limits).empty(),
                "Combined axis scale must govern limits even when uniform scale alone exceeds them");
        within_limits.placement.scale = 1;
        within_limits.width_scale = marker.minimum_scale;
        within_limits.depth_scale = marker.maximum_scale;
        require(!transformed_symbol_preview(marker, within_limits).empty(),
                "Both inclusive per-axis catalog boundaries must be supported");
        auto overflowing = instance;
        overflowing.width_scale = std::numeric_limits<double>::max();
        rejected([&] { (void)transformed_symbol_preview(marker, overflowing); });
        AnnotationState invalid_state;
        invalid_state.symbols.push_back(overflowing);
        rejected([&] { validate_annotation_state(invalid_state, {marker}); });
        auto oversized_definition = marker;
        oversized_definition.width_metres = std::numeric_limits<double>::max();
        rejected([&] { (void)transformed_symbol_preview(oversized_definition, instance); });
        rejected([&] { (void)transformed_symbol_point(marker, instance,
            {std::numeric_limits<double>::max(), 0}); });
        rejected([&] { (void)transformed_symbol_point(marker, instance,
            {std::numeric_limits<double>::infinity(), 0}); });
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"}) {
            auto malformed = saved;
            malformed["symbols"][0].erase(key);
            rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        }
        auto malformed = saved;
        malformed["symbols"][0]["extra_transform"] = 1;
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        malformed = saved;
        malformed["symbols"][0]["flip_horizontal"] = 1;
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        malformed = saved;
        malformed["symbols"][0]["width_scale"] = "2";
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        malformed = saved;
        malformed["version"] = 2;
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        AnnotationState original;
        original.symbols.push_back({"legacy", marker.id, {{10, 20}, std::numbers::pi / 2, 2}, {}, true});
        const auto original_saved = encode_annotation_state(original, {marker});
        auto legacy = original_saved;
        legacy["version"] = 2;
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
            legacy["symbols"][0].erase(key);
        const auto upgraded = decode_annotation_state(legacy, {});
        const auto geometry = transformed_symbol_preview(marker, upgraded.symbols[0]);
        require(close(geometry[0].start, {6, 22}) && close(geometry[0].end, {12, 16}) &&
                    encode_annotation_state(upgraded, {}) == original_saved,
                "Version2 upgrade must preserve exact old geometry and pinned definition");
        legacy["version"] = 1;
        legacy["symbols"][0].erase("definition");
        legacy["symbols"][0].erase("pinned_svg");
        require(encode_annotation_state(decode_annotation_state(legacy, {marker}), {marker}) == original_saved,
                "Version1 upgrade must preserve old geometry and pin the original catalog definition");
    }
    require(catalog.size() == 1151, "Preserve 809 legacy symbols and expose 342 SVG symbols");
    const auto svg_toilets = filter_symbol_catalog(catalog, "Toilet Close Coupled", "01_bathroom");
    require(svg_toilets.size() == 1 &&
                svg_toilets.front().id == "svg-v2-01_bathroom-toilet-close-coupled",
            "SVG human names must be searchable with category-qualified stable IDs");
    std::size_t svg_count = 0, nominal_count = 0;
    std::set<std::string> svg_categories, svg_paths;
    for (const auto& definition : catalog) {
        if (!definition.svg_asset) continue;
        ++svg_count;
        const auto& asset = *definition.svg_asset;
        nominal_count += asset.dimensions_are_nominal ? 1 : 0;
        svg_categories.insert(definition.category);
        require(svg_paths.insert(asset.relative_path).second, "Every SVG must retain its own asset");
        require(!definition.name.empty() && asset.view_box[2] > 0 && asset.view_box[3] > 0,
                "SVG renderers require human names and positive intrinsic bounds");
    }
    require(svg_count == 342 && nominal_count == 230 && svg_categories.size() == 25,
            "Import complete SVG category coverage without inventing nominal dimensions");
    AnnotationState every_symbol;
    for (const auto& definition : catalog)
        every_symbol.symbols.push_back({"instance-" + definition.id, definition.id, {}, {}, true});
    const auto every_symbol_encoded = encode_annotation_state(every_symbol, catalog);
    require(encode_annotation_state(decode_annotation_state(every_symbol_encoded, catalog), catalog) ==
                every_symbol_encoded,
            "Every legacy and SVG ID must survive project annotation serialization");
    require(filter_symbol_catalog(catalog, "svg-v2-03_laundry_utility-radiator").size() == 1 &&
                filter_symbol_catalog(catalog, "svg-v2-20_hvac_plumbing-radiator").size() == 1,
            "Repeated source IDs in different categories must not overwrite one another");
    const auto basin = filter_symbol_catalog(catalog, "svg-v2-01_bathroom-basin-oval").front();
    require(basin.width_metres == 0.55 && basin.depth_metres == 0.44 &&
                basin.svg_asset->footprint_view_box == std::array<double, 4>{0, 0, 550, 440},
            "Source nominal millimetres must map to metres without artwork padding");
    auto invalid_svg = basin;
    invalid_svg.svg_asset->relative_path = "../escape.svg";
    rejected([&]{validate_symbol_catalog({invalid_svg});});
    invalid_svg = basin; invalid_svg.svg_asset->view_box[2] = 0;
    rejected([&]{validate_symbol_catalog({invalid_svg});});
    invalid_svg = basin; invalid_svg.svg_asset->footprint_view_box[0] = std::numeric_limits<double>::infinity();
    rejected([&]{validate_symbol_catalog({invalid_svg});});
    require(catalog.size() >= 600,"Expected the expanded production symbol catalog");
    validate_symbol_catalog(catalog);
    const std::set<std::string> required_categories{
        "plumbing", "furniture", "fixtures", "appliances", "accessibility",
        "lighting", "doors_windows", "structural", "site", "commercial"};
    std::set<std::string> categories;
    std::set<std::string> families;
    for (const auto& definition : catalog) {
        categories.insert(definition.category);
        families.insert(definition.family);
        require(definition.width_metres > 0 && definition.depth_metres > 0,
                "Every symbol must expose physical dimensions");
        require(definition.minimum_scale < 1.0 && definition.maximum_scale > 1.0,
                "Every symbol must support practical resizing");
    }
    for (const auto& category : required_categories)
        require(categories.contains(category), "Required symbol category is missing");
    require(families.size() >= 300, "Catalog must expose at least 300 distinct component families");
    const auto plumbing = filter_symbol_catalog(catalog, "", "plumbing");
    require(plumbing.size() >= 36, "Category filtering must return all plumbing variants");
    require(filter_symbol_catalog(catalog, "", "PLUMBING").size() == plumbing.size(),
            "Symbol category filtering must be case-insensitive for field use");
    const auto toilets = filter_symbol_catalog(catalog, "toilet");
    require(toilets.size() >= 18, "Query filtering must match toilet families and variants");
    require(filter_symbol_catalog(catalog, "TOILET").size() == toilets.size(),
            "Symbol search must be case-insensitive for field use");
    const auto has_family = [&](const char* family) {
        return std::any_of(catalog.begin(), catalog.end(), [&](const auto& definition) {
            return definition.family == family && definition.preview.size() >= 5;
        });
    };
    require(has_family("toilet") && has_family("single-bed") && has_family("sofa") &&
                has_family("floor-drain"),
            "Core residential and plumbing families must have usable vector motifs");
    const auto nominal = [&](const char* family) -> const SymbolDefinition& {
        const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& definition) {
            return definition.family == family && definition.id.ends_with("-w2-d2");
        });
        require(found != catalog.end(), "Every representative family needs a nominal variant");
        return *found;
    };
    const auto normalized_preview = [](const SymbolDefinition& definition) {
        std::vector<std::array<long long, 4>> result;
        result.reserve(definition.preview.size());
        const auto normalize = [](double value, double half_extent) {
            return static_cast<long long>(std::llround(value / half_extent * 1000.0));
        };
        for (const auto& stroke : definition.preview) {
            result.push_back({normalize(stroke.start.x - definition.anchor.x,
                                        definition.width_metres / 2.0),
                             normalize(stroke.start.y - definition.anchor.y,
                                       definition.depth_metres / 2.0),
                             normalize(stroke.end.x - definition.anchor.x,
                                       definition.width_metres / 2.0),
                             normalize(stroke.end.y - definition.anchor.y,
                                       definition.depth_metres / 2.0)});
        }
        return result;
    };
    require(normalized_preview(nominal("range")) != normalized_preview(nominal("dishwasher")) &&
                normalized_preview(nominal("washer")) != normalized_preview(nominal("dryer")) &&
                normalized_preview(nominal("wardrobe")) != normalized_preview(nominal("bookcase")) &&
                normalized_preview(nominal("checkout-counter")) !=
                    normalized_preview(nominal("service-counter")),
            "Named symbol families must retain distinguishable vector motifs");
    const auto repeated = default_symbol_catalog();
    require(std::abs(nominal("sofa").width_metres - 2.1) < 1e-12 &&
                std::abs(nominal("sofa").depth_metres - 0.9) < 1e-12 &&
                std::abs(nominal("double-bed").width_metres - 1.4) < 1e-12 &&
                std::abs(nominal("double-bed").depth_metres - 2.0) < 1e-12 &&
                std::abs(nominal("toilet").width_metres - 0.4) < 1e-12 &&
                std::abs(nominal("toilet").depth_metres - 0.7) < 1e-12,
            "Artwork refinements must preserve nominal real-world footprints");
    // Upholstery and sanitary ware need actual curved silhouettes, rather than
    // the old shared box with family decorations drawn over it.
    for (const auto* family : {"sofa", "armchair", "chair", "single-bed", "double-bed",
                               "toilet", "bidet", "bathtub", "sink"}) {
        const auto strokes = normalized_preview(nominal(family));
        require(std::any_of(strokes.begin(), strokes.end(), [](const auto& stroke) {
                    return stroke[0] != stroke[2] && stroke[1] != stroke[3];
                }), "Soft furniture and bowls must retain curved outline segments");
        require(std::none_of(strokes.begin(), strokes.end(), [](const auto& stroke) {
                    return std::abs(stroke[0]) == 1000 && std::abs(stroke[2]) == 1000 &&
                           std::abs(stroke[1]) == 1000 && std::abs(stroke[3]) == 1000;
                }), "Curved families must not regain a generic rectangular footprint overlay");
    }
    std::set<std::vector<std::array<long long, 4>>> residential_signatures;
    for (const auto* family : {"sofa", "armchair", "chair", "bench", "single-bed", "double-bed",
                               "desk", "dining-table", "coffee-table", "side-table", "toilet",
                               "bidet", "urinal", "sink", "double-sink", "bathtub", "shower",
                               "range", "refrigerator", "dishwasher", "washer", "dryer"})
        require(residential_signatures.insert(normalized_preview(nominal(family))).second,
                "Residential family silhouettes must remain structurally distinguishable");
    for (std::size_t i=0;i<catalog.size();++i) {
        require(catalog[i].id == repeated[i].id && catalog[i].width_metres == repeated[i].width_metres,
                "Catalog must be deterministic");
        require(!placed_symbol_preview(catalog[i],{}).empty(),"Every catalog entry must produce preview strokes");
        require(normalized_preview(catalog[i]) == normalized_preview(repeated[i]),
                "All vector coordinates must be deterministic");
        require(catalog[i].svg_asset || normalized_preview(catalog[i]) == normalized_preview(nominal(catalog[i].family.c_str())),
                "Dimension variants must retain their family structure");
    }
    const auto manifest = encode_symbol_catalog_manifest(catalog);
    const auto svg_manifest = encode_symbol_catalog_manifest({basin});
    require(svg_manifest.at("entries").at(0).at("svg_asset").at("relative_path") ==
                "symbols/architectural_v2/symbols/01_bathroom/basin-oval.svg" &&
                svg_manifest.at("entries").at(0).at("name") == "Basin Oval",
            "Offline manifest must preserve renderer metadata and human names");
    require(manifest.at("schema_version") == 1 && manifest.at("catalog_id") == "vertex.symbol-catalog",
            "Symbol catalog manifest must identify its schema");
    require(manifest.at("catalog_revision") == kSymbolCatalogRevision,
            "Symbol catalog manifest must identify its revision");
    require(manifest.at("entry_count") == catalog.size() && manifest.at("family_count") == families.size() &&
                manifest.at("entries").size() == catalog.size(),
            "Symbol catalog manifest must enumerate every entry and family");
    require(manifest.at("category_counts").at("fixtures") >= 63 &&
                manifest.at("category_counts").at("commercial") >= 36,
            "Symbol catalog manifest must retain category coverage");
    const auto& manifest_families = manifest.at("families");
    require(manifest_families.at(0).at("id") == "accessible-bathtub" &&
                manifest_families.at(0).at("variant_count") == 1,
            "Symbol catalog manifest families must be stable and sorted");
    const auto legacy_family = std::find_if(manifest_families.begin(), manifest_families.end(),
        [](const auto& value) { return value.at("id") == "accessible-shower"; });
    require(legacy_family != manifest_families.end() && legacy_family->at("variant_count") == 9,
            "Existing preset families must retain all nine size variants");
    require(manifest == encode_symbol_catalog_manifest(repeated),
            "Symbol catalog manifest must be deterministic");
    require(manifest.at("entries").at(0).at("preview").size() >= 5,
            "Symbol catalog manifest must retain vector previews");
    auto labels = default_label_templates();
    require(filter_label_templates(labels,"room","rooms").size() >= 2,"Label filtering failed");
    require(filter_label_templates(labels,"","missing").empty(),"Category filtering failed");
    AnnotationState state;
    state.labels.push_back(instantiate_label(labels.front(),"label-1"));
    state.labels[0].content = "Guest bedroom\nNorth wing";
    state.labels[0].visible = false;
    state.labels[0].placement = {{4,3},0.25,2};
    state.labels[0].style.bold = true;
    state.labels[0].style.fill_pattern = "hatch";
    state.symbols.push_back({"symbol-1",catalog.front().id,{{10,20},std::numbers::pi/2,2},{},false});
    state.overrides.push_back({"area","area-1",{},false});
    state.overrides.push_back({"output_view","print-1",{},true});
    auto encoded = encode_annotation_state(state,catalog);
    auto plan_state=state;
    plan_state.labels.front().model_plan=true;
    plan_state.overrides.front().plan_label_offset=Vec2{.25,-.5};
    auto plan_wire=encode_annotation_state(plan_state,catalog);
    require(plan_wire.at("version")==5 && plan_wire.at("labels").at(0).at("model_plan")==true &&
        encode_annotation_state(decode_annotation_state(plan_wire,catalog),catalog)==plan_wire,
        "plan anchoring and existing area offsets must round-trip together as version 5");
    for(const auto version:{1,2,3,4}) {
        auto old=plan_wire;old["version"]=version;
        rejected([&]{(void)decode_annotation_state(old,catalog);});
    }
    auto invalid_plan=plan_wire;invalid_plan["labels"][0]["model_plan"]="true";
    rejected([&]{(void)decode_annotation_state(invalid_plan,catalog);});
    require(encoded.at("catalog_revision") == kSymbolCatalogRevision,
            "Annotation state must pin the symbol catalog revision");
    const auto decoded = decode_annotation_state(nlohmann::json::parse(encoded.dump()),catalog);
    require(encode_annotation_state(decoded,catalog) == encoded,"JSON roundtrip loses edits");
    auto legacy = encoded;
    legacy.erase("catalog_revision");
    require(encode_annotation_state(decode_annotation_state(legacy,catalog),catalog) == encoded,
            "Legacy annotation state without a catalog revision must upgrade deterministically");
    auto unsupported_revision = encoded;
    unsupported_revision["version"] = 1;
    unsupported_revision["catalog_revision"] = kSymbolCatalogRevision + 1;
    rejected([&]{(void)decode_annotation_state(unsupported_revision,catalog);});
    require(labels.front().content == "Bedroom","Instance edits mutated library template");
    auto transformed = placed_symbol_preview(catalog.front(),state.symbols.front().placement);
    const auto local = catalog.front().preview.front().start;
    require(std::abs(transformed.front().start.x-(10-2*local.y)) < 1e-12 &&
            std::abs(transformed.front().start.y-(20+2*local.x)) < 1e-12,"Placement rotation/scale/translation wrong");
    const std::array representatives{"toilet", "double-bed", "sofa", "checkout-counter"};
    const std::array resize_scales{0.5, 1.0, 2.4};
    const std::array rotations{0.0, std::numbers::pi / 4.0, std::numbers::pi / 2.0};
    for (std::size_t family_index = 0; family_index < representatives.size(); ++family_index) {
        const auto& definition = nominal(representatives[family_index]);
        for (const auto scale : resize_scales) {
            for (const auto rotation : rotations) {
                const AnnotationPlacement placement{{10.0 + static_cast<double>(family_index),
                                                     20.0 + scale}, rotation, scale};
                const auto placed = placed_symbol_preview(definition, placement);
                require(!placed.empty(),
                        "Representative symbols must render at every supported fixture scale and rotation");
                const auto first = definition.preview.front().start;
                const auto cosine = std::cos(rotation);
                const auto sine = std::sin(rotation);
                const auto local_x = (first.x - definition.anchor.x) * scale;
                const auto local_y = (first.y - definition.anchor.y) * scale;
                const Vec2 expected{placement.position.x + cosine * local_x - sine * local_y,
                                    placement.position.y + sine * local_x + cosine * local_y};
                require(std::abs(placed.front().start.x - expected.x) < 1e-12 &&
                            std::abs(placed.front().start.y - expected.y) < 1e-12,
                        "Representative symbol resize/rotation must preserve exact placement mathematics");
                for (const auto& stroke : placed) {
                    require(std::isfinite(stroke.start.x) && std::isfinite(stroke.start.y) &&
                                std::isfinite(stroke.end.x) && std::isfinite(stroke.end.y),
                            "Representative symbol transforms must remain finite");
                }
            }
        }
    }
    auto bad = state;
    bad.symbols[0].symbol_id = "missing";
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.symbols[0].id = bad.labels[0].id;
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.overrides.push_back(bad.overrides[0]);
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.labels[0].style.stroke_color = "red";
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.labels[0].placement.position.x = std::numeric_limits<double>::infinity();
    rejected([&]{validate_annotation_state(bad,catalog);});
    rejected([&]{(void)placed_symbol_preview(catalog.front(),{{},0,0.001});});
    auto malformed = encoded; malformed["version"] = 1.0;
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    malformed = encoded; malformed["version"] = 8;
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    malformed = encoded; malformed["labels"][0]["visible"] = "false";
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    malformed = encoded; malformed["symbols"] = nlohmann::json::object();
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    auto invalid_catalog = catalog; invalid_catalog.push_back(catalog.front());
    rejected([&]{validate_symbol_catalog(invalid_catalog);});
    invalid_catalog = catalog; invalid_catalog[0].preview.clear();
    rejected([&]{validate_symbol_catalog(invalid_catalog);});
    invalid_catalog = catalog;
    invalid_catalog[0].preview.front().start.x = invalid_catalog[0].width_metres;
    rejected([&]{validate_symbol_catalog(invalid_catalog);});
    std::cout << "annotation_catalog_tests passed\n";
}

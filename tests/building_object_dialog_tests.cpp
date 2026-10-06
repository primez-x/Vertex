#include "sketch/desktop/building_object_dialog.hpp"
#include "sketch/quantity.hpp"

#include "sketch/building_entity.hpp"
#include "sketch/vertical_levels.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sketch::Entity;
using sketch::desktop::BuildingObjectDialog;

std::optional<QString> capture_directory;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void require_near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(message));
    }
}

QComboBox& combo(BuildingObjectDialog& dialog, const char* object_name) {
    auto* result = dialog.findChild<QComboBox*>(object_name);
    require(result != nullptr, std::string("missing combo: ") + object_name);
    return *result;
}

QLineEdit& field(BuildingObjectDialog& dialog, const char* object_name) {
    auto* result = dialog.findChild<QLineEdit*>(object_name);
    require(result != nullptr, std::string("missing field: ") + object_name);
    return *result;
}

QCheckBox& check(BuildingObjectDialog& dialog, const char* object_name) {
    auto* result = dialog.findChild<QCheckBox*>(object_name);
    require(result != nullptr, std::string("missing check box: ") + object_name);
    return *result;
}

void select_form(BuildingObjectDialog& dialog, std::string_view form) {
    std::string_view type;
    if (form == "rectangular_column" || form == "circular_column") {
        type = "column";
    } else if (form == "straight_beam") {
        type = "beam";
    } else if (form == "straight_stair_flight" || form == "multi_flight_stair") {
        type = "stair";
    } else if (form == "straight_railing" || form == "stair_flight_railing") {
        type = "railing";
    } else {
        type = "roof";
    }
    auto& types = combo(dialog, "buildingObjectType");
    const auto type_index = types.findData(QString::fromUtf8(type.data(),
                                                                static_cast<qsizetype>(type.size())));
    require(type_index >= 0, "requested building type is not listed");
    types.setCurrentIndex(type_index);
    auto& forms = combo(dialog, "buildingObjectForm");
    const auto form_index = forms.findData(QString::fromUtf8(form.data(),
                                                                static_cast<qsizetype>(form.size())));
    require(form_index >= 0, "requested building form is not listed");
    forms.setCurrentIndex(form_index);
}

void set_field(BuildingObjectDialog& dialog, const char* object_name,
               const char* value) {
    field(dialog, object_name).setText(QString::fromUtf8(value));
}

const nlohmann::json& quantity_entries(const Entity& entity) {
    require(entity.properties.contains("quantity_entries") &&
                entity.properties.at("quantity_entries").is_object(),
            "candidate should contain an object-valued quantity receipt map");
    return entity.properties.at("quantity_entries");
}

const nlohmann::json& quantity_receipt(const Entity& entity, const char* pointer) {
    const auto& entries = quantity_entries(entity);
    const auto found = entries.find(pointer);
    require(found != entries.end(), std::string("missing quantity receipt: ") + pointer);
    return *found;
}

void require_quantity_receipt(const Entity& entity, const char* pointer,
                              const char* expression, const char* unit,
                              std::int64_t numerator, std::int64_t denominator) {
    const auto& receipt = quantity_receipt(entity, pointer);
    require(receipt.is_object() && receipt.at("version") == 1 &&
                receipt.at("original_expression") == expression &&
                receipt.at("entered_unit") == unit,
            std::string("quantity receipt metadata mismatch: ") + pointer);
    const auto& exact = receipt.at("exact_metres");
    require(exact.is_object() && exact.at("numerator") == numerator &&
                exact.at("denominator") == denominator,
            std::string("quantity receipt rational mismatch: ") + pointer);
}

void capture(BuildingObjectDialog& dialog, QString name, bool scroll_to_end = false) {
    if (!capture_directory.has_value()) {
        return;
    }
    const auto directory = *capture_directory;
    require(QDir().mkpath(directory), "capture directory could not be created");
    dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
    dialog.resize(440, 560);
    dialog.show();
    QCoreApplication::processEvents();
    if (scroll_to_end) {
        auto* scroll = dialog.findChild<QScrollArea*>("buildingObjectScrollArea");
        require(scroll != nullptr, "dialog capture requires its scroll area");
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QCoreApplication::processEvents();
    }
    const auto path = QDir(directory).filePath(std::move(name) + QStringLiteral(".png"));
    require(dialog.grab().save(path), "dialog capture could not be written");
    dialog.hide();
    QCoreApplication::processEvents();
}

void test_all_forms_submit_to_entities() {
    using namespace sketch;

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "rectangular_column");
        set_field(dialog, "buildingObjectWidth", "400 mm");
        set_field(dialog, "buildingObjectDepth", "600 mm");
        set_field(dialog, "buildingObjectHeight", "3 m");
        capture(dialog, QStringLiteral("rectangular-column"));
        require(dialog.submit(), "rectangular column should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value(), "rectangular column candidate");
        require(candidate->type == "column" &&
                    candidate->properties.at("form") == "rectangular_column",
                "rectangular column canonical discriminator");
    }

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "circular_column");
        set_field(dialog, "buildingObjectRadius", "250 mm");
        set_field(dialog, "buildingObjectHeight", "2 m");
        capture(dialog, QStringLiteral("circular-column"));
        require(dialog.submit(), "circular column should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value() &&
                    candidate->properties.at("form") == "circular_column",
                "circular column canonical discriminator");
        auto rotated=*candidate;
        rotated.properties["rotation_rad"]=1.234;
        BuildingObjectDialog editor(rotated,true);
        set_field(editor,"buildingObjectRadius","300 mm");
        require(editor.submit() && editor.candidate().has_value() &&
                    editor.candidate()->properties.at("rotation_rad")==1.234,
                "circular column property edit reset its selection orientation");
    }

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "straight_beam");
        set_field(dialog, "buildingObjectStartX", "0 m");
        set_field(dialog, "buildingObjectStartY", "0 m");
        set_field(dialog, "buildingObjectStartZ", "2.5 m");
        set_field(dialog, "buildingObjectEndX", "3 m");
        set_field(dialog, "buildingObjectEndY", "1 m");
        set_field(dialog, "buildingObjectEndZ", "3 m");
        set_field(dialog, "buildingObjectWidth", "200 mm");
        set_field(dialog, "buildingObjectDepth", "300 mm");
        capture(dialog, QStringLiteral("straight-beam"));
        require(dialog.submit(), "beam should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value() && candidate->type == "beam" &&
                    candidate->properties.at("form") == "straight_beam",
                "beam canonical discriminator");
    }

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "straight_stair_flight");
        set_field(dialog, "buildingObjectRiserCount", "4");
        set_field(dialog, "buildingObjectTotalRise", "2 m");
        set_field(dialog, "buildingObjectGoing", "250 mm");
        set_field(dialog, "buildingObjectWidth", "1.5 m");
        check(dialog, "buildingObjectLandingEnabled").setChecked(true);
        set_field(dialog, "buildingObjectLandingDepth", "500 mm");
        set_field(dialog, "buildingObjectLandingThickness", "200 mm");
        capture(dialog, QStringLiteral("straight-stair-flight"));
        require(dialog.submit(), "stairs should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value() && candidate->type == "stair" &&
                    candidate->properties.at("top_landing").is_object(),
                "stair landing canonical entity");
    }

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "straight_stair_flight");
        check(dialog, "buildingObjectLevelConnectionEnabled").setChecked(true);
        set_field(dialog, "buildingObjectLevelGraph", "levels-1");
        set_field(dialog, "buildingObjectLevelLink", "ground-first");
        set_field(dialog, "buildingObjectLowerLevel", "ground");
        set_field(dialog, "buildingObjectUpperLevel", "first");
        require(dialog.submit(), "connected stairs should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value() &&
                    candidate->properties.at("level_connection").at("graph_id") ==
                        "levels-1" &&
                    candidate->properties.at("level_connection").at("link_id") ==
                        "ground-first",
                "stair level connection controls should emit canonical IDs");
    }

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "sloped_roof_panel");
        set_field(dialog, "buildingObjectRun", "4 m");
        set_field(dialog, "buildingObjectSpan", "3 m");
        set_field(dialog, "buildingObjectRise", "1 m");
        set_field(dialog, "buildingObjectOverhang", "200 mm");
        set_field(dialog, "buildingObjectThickness", "100 mm");
        capture(dialog, QStringLiteral("sloped-roof-panel"));
        require(dialog.submit(), "sloped roof panel should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value(), "sloped roof panel candidate");
        const auto& properties = candidate->properties;
        require(properties.at("form") == "sloped_roof_panel" &&
                    !dialog.findChild<QLineEdit*>("buildingObjectPitch"),
                "sloped roof pitch must be derived");
        require_near(properties.at("pitch_rad").get<double>(), std::atan(0.25), 1e-12,
                     "sloped roof derived pitch");
    }

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "sloped_roof_panel");
        set_field(dialog, "buildingObjectRise", "0 m");
        require(dialog.submit(), "flat roof panel should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value(), "flat roof panel candidate");
        require(candidate->properties.at("form") == "sloped_roof_panel" &&
                    candidate->properties.at("rise_m") == 0.0 &&
                    candidate->properties.at("pitch_rad") == 0.0,
                "flat roof panel should preserve zero rise and pitch");
    }

    {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, "gable_roof");
        set_field(dialog, "buildingObjectLength", "5 m");
        set_field(dialog, "buildingObjectSpan", "4 m");
        set_field(dialog, "buildingObjectRise", "1 m");
        set_field(dialog, "buildingObjectOverhang", "200 mm");
        set_field(dialog, "buildingObjectThickness", "100 mm");
        capture(dialog, QStringLiteral("gable-roof"));
        require(dialog.submit(), "gable roof should submit");
        const auto candidate = dialog.candidate();
        require(candidate.has_value(), "gable roof candidate");
        const auto& properties = candidate->properties;
        require(properties.at("form") == "gable_roof" &&
                    !dialog.findChild<QLineEdit*>("buildingObjectPitch"),
                "gable roof pitch must be derived");
        require_near(properties.at("pitch_rad").get<double>(), std::atan(0.5), 1e-12,
                     "gable roof derived pitch");
    }
}

void test_invalid_input_stays_open_with_inline_error() {
    BuildingObjectDialog dialog(std::nullopt, true);
    select_form(dialog, "rectangular_column");
    set_field(dialog, "buildingObjectWidth", "not a length");
    require(field(dialog, "buildingObjectDepth").text() == QStringLiteral("0.4 m"),
            "ordinary metric dimensions should not show binary floating-point artifacts");
    dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
    dialog.show();
    QCoreApplication::processEvents();
    require(!dialog.submit(), "invalid quantity must reject submit");
    require(!dialog.candidate().has_value(), "invalid submit must not create candidate");
    require(!dialog.lastError().isEmpty(), "invalid quantity should expose an error");
    const auto* error = dialog.findChild<QLabel*>("buildingObjectError");
    require(error != nullptr && error->isVisible() && !error->text().isEmpty(),
            "invalid quantity error should be visible inline");
    require(dialog.result() != QDialog::Accepted,
            "invalid submit must leave dialog open");
    capture(dialog, QStringLiteral("invalid-input"));
}

void test_malformed_original_disables_submit_without_throwing() {
    const Entity malformed{
        .id = "column-invalid",
        .type = "column",
        .properties = nlohmann::json::object(),
        .required = true,
        .extensions = nlohmann::json::object(),
    };
    BuildingObjectDialog dialog(malformed, true);
    require(!dialog.lastError().isEmpty(), "malformed original should show an error");
    const auto* buttons = dialog.findChild<QDialogButtonBox*>("buildingObjectButtons");
    require(buttons != nullptr, "dialog button box should be named");
    require(!buttons->button(QDialogButtonBox::Ok)->isEnabled(),
            "malformed original should disable submit");
    require(!dialog.submit(), "malformed original cannot submit");
    require(!dialog.candidate().has_value(), "malformed original has no candidate");
}

void test_edit_merges_canonical_fields_and_preserves_metadata() {
    using namespace sketch;
    auto original = encode_building_entity(
        RectangularColumn{"stable-column", {0.0, 0.0, 0.0}, 0.4, 0.6, 3.0, 0.0},
        nlohmann::json{{"ui", {{"color", "blue"}}},
                       {"future", {{"token", 42}}}});
    original.required = true;
    original.properties["future_property"] = "retain me";
    BuildingObjectDialog dialog(original, false);
    set_field(dialog, "buildingObjectWidth", "2 ft");
    require(dialog.submit(), "valid object edit should submit");
    const auto candidate = dialog.candidate();
    require(candidate.has_value(), "edited candidate should be present");
    require(candidate->id == original.id && candidate->required == original.required,
            "edit must preserve stable identity and required state");
    require(candidate->extensions == original.extensions,
            "edit must preserve opaque metadata");
    require(candidate->properties.at("future_property") == "retain me",
            "edit must preserve unknown properties");
    require_near(candidate->properties.at("width_m").get<double>(), 0.6096, 1e-12,
                 "imperial edit should parse to metres");
}

void test_edit_preserves_untouched_high_precision_fields() {
    using namespace sketch;
    const GableRoof object{
        .id = "precise-gable",
        .base_position = {1.23456789012345, -2.34567890123456, 4.5678901234567},
        .orientation_radians = 0.371234567890123,
        .length = 5.12345678901234,
        .span = 4.45678901234567,
        .rise = 1.11123456789012,
        .pitch_radians = std::atan(1.11123456789012 / (4.45678901234567 * 0.5)),
        .overhang = 0.222345678901234,
        .thickness = 0.123456789012345,
    };
    const HipRoof hip{object.id, object.base_position, object.orientation_radians,
        object.length, object.span, object.rise, object.pitch_radians, object.overhang, object.thickness};
    for (const BuildingObject& candidate_object : {BuildingObject{object}, BuildingObject{hip}}) {
    const auto original = encode_building_entity(candidate_object);
    BuildingObjectDialog dialog(original, false);
    set_field(dialog, "buildingObjectThickness", "6 in");
    require(dialog.submit(), "high precision object edit should submit");
    const auto candidate = dialog.candidate();
    require(candidate.has_value(), "high precision edited candidate should be present");
    for (const auto* key : {"base_position_m", "orientation_rad", "length_m", "span_m",
                            "rise_m", "pitch_rad", "overhang_m"}) {
        require(candidate->properties.at(key) == original.properties.at(key),
                std::string("untouched field changed: ") + key);
    }
    require(candidate->properties.at("thickness_m") != original.properties.at("thickness_m"),
            "edited field should change");
    }
}

void test_imperial_defaults_use_shared_quantity_input() {
    BuildingObjectDialog dialog(std::nullopt, false);
    select_form(dialog, "rectangular_column");
    const auto width = field(dialog, "buildingObjectWidth").text();
    require((width.endsWith(QStringLiteral(" ft")) || width.endsWith(QStringLiteral(" m"))) &&
                sketch::parse_quantity(width.toStdString(), sketch::Unit::foot).metres == 0.4,
            "imperial dialog defaults should show explicit units without changing their geometry");
    set_field(dialog, "buildingObjectWidth", "2 ft");
    require(dialog.submit(), "imperial quantity should submit");
    require_near(dialog.candidate()->properties.at("width_m").get<double>(), 0.6096, 1e-12,
                 "imperial quantity conversion");
}

void test_quantity_receipts_capture_exact_and_nested_inputs() {
    using namespace sketch;

    BuildingObjectDialog column(std::nullopt, false);
    select_form(column, "rectangular_column");
    set_field(column, "buildingObjectBaseX", "2 1/2 in");
    set_field(column, "buildingObjectWidth", "1/3 ft");
    require(column.submit(), "column with exact quantity input should submit");
    const auto column_candidate = column.candidate();
    require(column_candidate.has_value(), "column receipt candidate should be present");
    require_quantity_receipt(*column_candidate, "/width_m", "1/3 ft", "ft", 127, 1250);
    require_quantity_receipt(*column_candidate, "/base_center_m/0", "2 1/2 in", "in", 127, 2000);

    BuildingObjectDialog stairs(std::nullopt, false);
    select_form(stairs, "straight_stair_flight");
    set_field(stairs, "buildingObjectBaseX", "1/3 ft");
    check(stairs, "buildingObjectLandingEnabled").setChecked(true);
    set_field(stairs, "buildingObjectLandingDepth", "2 1/2 in");
    set_field(stairs, "buildingObjectLandingThickness", "1/3 ft");
    const bool stair_submitted = stairs.submit();
    require(stair_submitted,
            "stair with nested quantity input should submit: " +
                stairs.lastError().toStdString());
    const auto stair_candidate = stairs.candidate();
    require(stair_candidate.has_value(), "stair receipt candidate should be present");
    require_quantity_receipt(*stair_candidate, "/base_position_m/0", "1/3 ft", "ft", 127, 1250);
    require_quantity_receipt(*stair_candidate, "/top_landing/depth_m", "2 1/2 in", "in", 127, 2000);
    require_quantity_receipt(*stair_candidate, "/top_landing/thickness_m", "1/3 ft", "ft", 127, 1250);
}

void test_suffixless_receipt_survives_metric_edit_display() {
    BuildingObjectDialog create(std::nullopt, false);
    select_form(create, "rectangular_column");
    set_field(create, "buildingObjectWidth", "1/3");
    require(create.submit(), "suffixless imperial quantity should submit");
    const auto original = create.candidate();
    require(original.has_value(), "suffixless original candidate should be present");

    BuildingObjectDialog edit(*original, true);
    require(field(edit, "buildingObjectWidth").text() == QStringLiteral("1/3 ft"),
            "a valid suffixless receipt should show its recorded unit in metric mode");
    capture(edit, QStringLiteral("quantity-unit-switch"));
    require(edit.submit(), "unchanged metric edit should submit");
    const auto edited = edit.candidate();
    require(edited.has_value(), "metric edit candidate should be present");
    require_quantity_receipt(*edited, "/width_m", "1/3", "ft", 127, 1250);
    require(edited->properties.at("width_m") == original->properties.at("width_m"),
            "display-unit change must not alter authoritative width");
}

void test_changed_quantity_replaces_only_its_receipt() {
    BuildingObjectDialog create(std::nullopt, false);
    select_form(create, "rectangular_column");
    set_field(create, "buildingObjectBaseX", "1.23456789012345 m");
    set_field(create, "buildingObjectWidth", "1/3 ft");
    set_field(create, "buildingObjectDepth", "2 1/2 in");
    require(create.submit(), "receipt precision fixture should submit");
    const auto original = create.candidate();
    require(original.has_value(), "receipt precision fixture should produce a candidate");
    const auto original_depth_receipt = quantity_receipt(*original, "/depth_m");
    const auto original_base_receipt = quantity_receipt(*original, "/base_center_m/0");
    const auto original_base_value = original->properties.at("base_center_m").at(0);

    BuildingObjectDialog edit(*original, false);
    set_field(edit, "buildingObjectWidth", "2/3 ft");
    require(edit.submit(), "changed quantity edit should submit");
    const auto edited = edit.candidate();
    require(edited.has_value(), "changed quantity edit should produce a candidate");
    require_quantity_receipt(*edited, "/width_m", "2/3 ft", "ft", 127, 625);
    require(quantity_receipt(*edited, "/depth_m") == original_depth_receipt,
            "untouched depth receipt must remain byte-for-byte stable");
    require(quantity_receipt(*edited, "/base_center_m/0") == original_base_receipt,
            "untouched coordinate receipt must remain byte-for-byte stable");
    require(edited->properties.at("base_center_m").at(0) == original_base_value,
            "untouched high-precision coordinate must remain authoritative");
}

void test_untouched_defaults_submit_identically_in_both_units() {
    const std::array<std::string_view, 8> forms{
        "rectangular_column", "circular_column", "straight_beam",
        "straight_stair_flight", "straight_railing", "sloped_roof_panel",
        "gable_roof", "hip_roof"};

    for (const auto form : forms) {
        std::optional<nlohmann::json> canonical;
        for (const bool metric : {true, false}) {
            BuildingObjectDialog dialog(std::nullopt, metric);
            select_form(dialog, form);
            if (!metric) {
                for (const auto* input : dialog.findChildren<QLineEdit*>()) {
                    if (input->text().endsWith(QStringLiteral(" ft")))
                        require(input->text().size() <= 14,
                                "generated imperial defaults should remain concise");
                }
            }
            if (!metric && form == "straight_stair_flight") {
                capture(dialog, QStringLiteral("imperial-default-stair"));
            }
            const bool submitted = dialog.submit();
            require(submitted,
                    std::string("untouched ") + std::string(form) +
                        (metric ? " metric" : " imperial") + " form should submit: " +
                        dialog.lastError().toStdString());
            const auto candidate = dialog.candidate();
            require(candidate.has_value(), "untouched default should produce a candidate");
            auto properties = candidate->properties;
            properties.erase("quantity_entries");
            if (!canonical.has_value()) {
                canonical = std::move(properties);
            } else {
                require(properties == *canonical,
                        std::string("default geometry differs between units for ") +
                            std::string(form));
            }
        }
    }
}

void test_untouched_unknown_receipts_survive_unrelated_edit() {
    BuildingObjectDialog create(std::nullopt, false);
    select_form(create, "rectangular_column");
    set_field(create, "buildingObjectWidth", "1/3 ft");
    set_field(create, "buildingObjectDepth", "2 1/2 in");
    require(create.submit(), "unknown receipt fixture should submit");
    auto original = create.candidate();
    require(original.has_value(), "unknown receipt fixture should produce a candidate");

    const auto original_width_receipt = quantity_receipt(*original, "/width_m");
    const nlohmann::json future_record{
        {"version", 99},
        {"original_expression", "future-dimension-token"},
        {"entered_unit", "survey-ft"},
        {"exact_metres", {{"numerator", 17}, {"denominator", 5}}},
        {"future_payload", {{"opaque", true}}},
    };
    original->properties["quantity_entries"]["/future_dimension_m"] = future_record;

    BuildingObjectDialog edit(*original, false);
    set_field(edit, "buildingObjectDepth", "3/4 ft");
    require(edit.submit(), "unrelated dimension edit should submit");
    const auto edited = edit.candidate();
    require(edited.has_value(), "unrelated dimension edit should produce a candidate");
    const auto& entries = quantity_entries(*edited);
    require(entries.at("/future_dimension_m") == future_record,
            "future receipt records must survive unrelated edits byte-for-byte");
    require(entries.at("/width_m") == original_width_receipt,
            "untouched known receipt must survive unrelated edits byte-for-byte");
    require_quantity_receipt(*edited, "/depth_m", "3/4 ft", "ft", 1143, 5000);
}

void test_stale_invalid_and_oversized_receipts_do_not_override_display() {
    using namespace sketch;

    const auto receipt_for = [](std::string expression, Unit default_unit) {
        const auto quantity = parse_quantity(expression, default_unit);
        return nlohmann::json{
            {"version", 1},
            {"original_expression", std::move(expression)},
            {"entered_unit", quantity.entered_unit == Unit::foot ? "ft" : "m"},
            {"exact_metres", {{"numerator", quantity.exact_metres.numerator},
                               {"denominator", quantity.exact_metres.denominator}}},
        };
    };

    const auto check_not_displayed = [](const nlohmann::json& receipt,
                                        std::string_view message) {
        auto original = encode_building_entity(
            RectangularColumn{"forged-receipt", {0.0, 0.0, 0.0}, 0.4, 0.6, 3.0, 0.0});
        original.properties["quantity_entries"] = nlohmann::json::object();
        original.properties["quantity_entries"]["/width_m"] = receipt;

        BuildingObjectDialog edit(original, true);
        require(field(edit, "buildingObjectWidth").text() !=
                    QStringLiteral("999 ft"),
                message);
        require(field(edit, "buildingObjectWidth").text() == QStringLiteral("0.4 m"),
                "invalid receipt must leave the authoritative metric value visible");
        require(edit.submit(), "invalid receipt edit should still submit");
        const auto candidate = edit.candidate();
        require(candidate.has_value(), "invalid receipt edit should produce a candidate");
        require(quantity_entries(*candidate).at("/width_m") == receipt,
                "invalid receipt should remain opaque metadata until its field is edited");
    };

    check_not_displayed(receipt_for("999 ft", Unit::foot),
                        "stale receipt must not override the current width");

    const auto oversized_numerator = nlohmann::json{
        {"version", 1},
        {"original_expression", "999 ft"},
        {"entered_unit", "ft"},
        {"exact_metres", {{"numerator", std::numeric_limits<std::uint64_t>::max()},
                           {"denominator", std::uint64_t{1}}}},
    };
    check_not_displayed(oversized_numerator,
                        "unsigned numerator above int64 must not override the current width");

    const auto oversized_denominator = nlohmann::json{
        {"version", 1},
        {"original_expression", "999 ft"},
        {"entered_unit", "ft"},
        {"exact_metres", {{"numerator", std::uint64_t{1}},
                           {"denominator", std::numeric_limits<std::uint64_t>::max()}}},
    };
    check_not_displayed(oversized_denominator,
                        "unsigned denominator above int64 must not override the current width");
}

void test_failed_submit_does_not_leak_receipts_across_candidates() {
    BuildingObjectDialog dialog(std::nullopt, false);
    select_form(dialog, "rectangular_column");
    set_field(dialog, "buildingObjectWidth", "1/3 ft");
    require(dialog.submit(), "initial receipt candidate should submit");
    require(dialog.candidate().has_value(), "initial candidate should exist");

    set_field(dialog, "buildingObjectWidth", "not a quantity");
    require(!dialog.submit(), "invalid quantity must reject a second submit");
    require(!dialog.candidate().has_value(),
            "failed submit must clear the previous candidate and its receipts");

    select_form(dialog, "circular_column");
    set_field(dialog, "buildingObjectRadius", "1/2 ft");
    require(dialog.submit(), "new form after invalid submit should submit");
    const auto candidate = dialog.candidate();
    require(candidate.has_value() && candidate->properties.at("form") == "circular_column",
            "new form candidate should be circular");
    const auto& entries = quantity_entries(*candidate);
    require(!entries.contains("/width_m") && !entries.contains("/depth_m"),
            "receipts from the failed rectangular candidate must not leak into circular form");
}

QTableWidget& roof_openings(BuildingObjectDialog& dialog) {
    auto* table = dialog.findChild<QTableWidget*>("buildingObjectRoofOpenings");
    require(table != nullptr, "roof openings table should be available in the object dialog");
    return *table;
}

void add_roof_opening(BuildingObjectDialog& dialog) {
    auto* button = dialog.findChild<QPushButton*>("buildingObjectAddRoofOpening");
    require(button != nullptr, "roof opening add control should be available");
    button->click();
}

void test_roof_openings_create_edit_remove_and_preserve_units() {
    for (const auto form : {"sloped_roof_panel", "gable_roof", "hip_roof"}) {
        BuildingObjectDialog create(std::nullopt, false);
        select_form(create, form);
        add_roof_opening(create);
        auto& table = roof_openings(create);
        table.item(0, 0)->setText("1/3");
        table.item(0, 1)->setText("1/3 ft");
        table.item(0, 2)->setText("2 ft");
        table.item(0, 3)->setText("1 ft");
        if (std::string_view(form) == "hip_roof") capture(create, "hip-roof-openings", true);
        require(create.submit(), std::string(form) + " should create an opening: " + create.lastError().toStdString());
        const auto original = *create.candidate();
        const auto original_entries = original.properties.at("roof_openings");
        const auto opening_id = original_entries.at(0).at("id").get<std::string>();
        require(!opening_id.empty() && original.properties.at("version") == 2,
                "opening should have a stable ID and version 2 schema");
        require_near(original_entries.at(0).at("x_m").get<double>(), 0.1016, 1e-12,
                     "suffixless opening coordinate uses imperial default");
        const auto original_receipts = original.extensions.at("roof_opening_input");

        BuildingObjectDialog unchanged(original, true);
        require(roof_openings(unchanged).item(0, 0)->text() == "1/3 ft",
                "opening receipt should disambiguate suffixless units when display units change");
        set_field(unchanged, "buildingObjectThickness", "90 mm");
        require(unchanged.submit(), "unrelated roof dimension edit should preserve opening");
        require(unchanged.candidate()->properties.at("roof_openings") == original_entries &&
                    unchanged.candidate()->extensions.at("roof_opening_input") == original_receipts,
                "untouched opening values and receipts should remain exact through unit changes");

        BuildingObjectDialog edit(original, true);
        roof_openings(edit).item(0, 2)->setText("700 mm");
        require(edit.submit(), "opening width edit should submit");
        const auto edited = *edit.candidate();
        require(edited.id == original.id && edited.properties.at("roof_openings").at(0).at("id") == opening_id,
                "opening edit should preserve roof and opening identity");
        require_near(edited.properties.at("roof_openings").at(0).at("width_m").get<double>(), 0.7, 1e-12,
                     "opening width edit should use shared unit parser");
        require(edited.extensions.at("roof_opening_input").at("entries").at(opening_id).at("x_m") ==
                    original_receipts.at("entries").at(opening_id).at("x_m"),
                "opening edit should preserve untouched field receipt");

        BuildingObjectDialog remove(edited, true);
        roof_openings(remove).selectRow(0);
        remove.findChild<QPushButton*>("buildingObjectRemoveRoofOpening")->click();
        require(remove.submit(), "opening removal should submit");
        require(!remove.candidate()->properties.contains("roof_openings") &&
                    remove.candidate()->properties.at("version") == 1 &&
                    remove.candidate()->extensions.at("roof_opening_input").at("entries").empty(),
                "removing last opening should clear opening schema and receipt");
    }
}

void test_roof_opening_validation_is_atomic() {
    for (const auto form : {"sloped_roof_panel", "gable_roof", "hip_roof"}) {
        BuildingObjectDialog dialog(std::nullopt, true);
        select_form(dialog, form);
        add_roof_opening(dialog);
        auto& table = roof_openings(dialog);
        for (const auto invalid_width : {"not a length", "0 m", "100 m"}) {
            table.item(0, 2)->setText(invalid_width);
            require(!dialog.submit() && !dialog.candidate().has_value() && !dialog.lastError().isEmpty(),
                    "invalid opening must reject candidate with an inline error");
        }
        table.item(0, 2)->setText("0.5 m");
        add_roof_opening(dialog);
        require(!dialog.submit() && !dialog.candidate().has_value() &&
                    dialog.lastError().contains("overlap"),
                "overlapping openings should reject the complete candidate");
        table.selectRow(1);
        dialog.findChild<QPushButton*>("buildingObjectRemoveRoofOpening")->click();
        require(dialog.submit(), "valid opening should submit after correcting failures");
        const auto original = *dialog.candidate();
        BuildingObjectDialog resize(original, true);
        set_field(resize, "buildingObjectSpan", "0.7 m");
        require(!resize.submit() && !resize.candidate().has_value(),
                "roof resizing should reject invalid retained openings instead of dropping them");
        require(original.properties.at("roof_openings").size() == 1,
                "failed edit should not mutate original roof");
    }
}

void test_roof_form_changes_preserve_opening_draft() {
    BuildingObjectDialog dialog(std::nullopt, true);
    select_form(dialog, "sloped_roof_panel");
    add_roof_opening(dialog);
    roof_openings(dialog).item(0, 2)->setText("600 mm");
    select_form(dialog, "gable_roof");
    select_form(dialog, "hip_roof");
    require(roof_openings(dialog).rowCount() == 1 && roof_openings(dialog).item(0, 2)->text() == "600 mm",
            "switching roof form should preserve opening draft");
    require(dialog.submit() && dialog.candidate()->properties.at("roof_openings").size() == 1,
            "retained opening should validate in the selected roof form");
    select_form(dialog, "rectangular_column");
    require(!dialog.submit() && !dialog.candidate().has_value(),
            "changing away from roof should require explicit opening removal");
}

void test_multiflight_widgets_preserve_identity_and_metadata() {
    BuildingObjectDialog dialog(std::nullopt, true);
    select_form(dialog, "multi_flight_stair");
    auto* flights = dialog.findChild<QTableWidget*>("buildingObjectStairFlights");
    auto* landings = dialog.findChild<QTableWidget*>("buildingObjectStairLandings");
    require(flights && landings && flights->rowCount() == 1, "multi-flight authoring table missing");
    flights->item(0, 0)->setText("5");
    dialog.findChild<QPushButton*>("buildingObjectAddStairFlight")->click();
    flights->item(1, 0)->setText("7");
    landings->item(0, 0)->setText("1500 mm");
    auto* turn = qobject_cast<QComboBox*>(landings->cellWidget(0, 2));
    require(turn != nullptr, "connecting landing turn selector missing");
    turn->setCurrentIndex(turn->findData(static_cast<int>(sketch::StairTurn::left_quarter)));
    require(dialog.submit(), "valid multi-flight stair should submit");
    auto original = *dialog.candidate();
    auto stair = sketch::decode_stair_properties(original.id, original.properties);
    require(stair.riser_count == 12 && stair.flights.size() == 2 &&
                stair.landings.front().turn == sketch::StairTurn::left_quarter,
            "table edits must author exact counts and turn");
    original.properties["future_stair"] = {{"opaque", true}};
    original.properties["flights"][0]["future_flight"] = 42;
    original.properties["level_connection"] = {{"version", 1}, {"graph_id", "graph"},
        {"link_id", "link"}, {"lower_level_id", "lower"}, {"upper_level_id", "upper"}, {"future_connection", 17}};
    original.extensions["future_metadata"] = "preserved";
    BuildingObjectDialog edit(original, true);
    flights = edit.findChild<QTableWidget*>("buildingObjectStairFlights");
    flights->selectRow(1);
    edit.findChild<QPushButton*>("buildingObjectMoveStairFlightUp")->click();
    require(edit.submit(), "reordered stair should submit");
    const auto edited = *edit.candidate();
    const auto reordered = sketch::decode_stair_properties(edited.id, edited.properties);
    require(reordered.flights[0].id == stair.flights[1].id && reordered.flights[1].id == stair.flights[0].id &&
                reordered.landings[0].id == stair.landings[0].id && reordered.riser_count == 12,
            "reorder must preserve stable IDs and counts");
    require(edited.properties.at("flights")[1].at("future_flight") == 42 &&
                edited.properties.at("future_stair") == original.properties.at("future_stair") &&
                edited.extensions == original.extensions,
            "multi-flight edits must retain opaque child and entity metadata");
    require_quantity_receipt(edited, "/landings/0/depth_m", "1500 mm", "mm", 3, 2);
    require(edited.properties.at("level_connection") == original.properties.at("level_connection"),
            "untouched connection must preserve opaque nested metadata");
    BuildingObjectDialog disconnect(edited, true);
    check(disconnect, "buildingObjectLevelConnectionEnabled").setChecked(false);
    require(disconnect.submit() && !disconnect.candidate()->properties.contains("level_connection") &&
                !sketch::decode_stair_properties(disconnect.candidate()->id, disconnect.candidate()->properties).level_connection,
            "explicit disconnect must remove canonical connection while retaining stair topology");
    BuildingObjectDialog invalid(edited, true);
    invalid.findChild<QTableWidget*>("buildingObjectStairFlights")->item(0, 0)->setText("0");
    require(!invalid.submit() && !invalid.candidate() && !invalid.lastError().isEmpty(),
            "invalid flight count must reject complete candidate");
}

void test_hosted_railing_candidates_are_atomic_and_cancel_is_immutable() {
    auto document = sketch::Document::create();
    const sketch::VerticalLevelGraph levels({{"lower", 0}, {"upper", 2.5}}, {{"link", "lower", "upper"}});
    const auto level_entity = Entity::create("vertical_levels", {{"model", nlohmann::json::parse(levels.serialize())}});
    auto host = sketch::encode_building_entity(sketch::StairFlight{"host", {}, 0, 10, 2.5, 0.25, 1.2,
        sketch::StairLanding{1.2, 0.15}, sketch::StairLevelConnection{level_entity.id, "link", "lower", "upper"}});
    host.properties["future_host"] = 23;
    host.properties["top_landing"]["future_payload"] = {{"version", 8}, {"data", {"opaque", 17}}};
    host.properties["quantity_entries"] = {
        {"/top_landing/depth_m", {{"version", 1}, {"original_expression", "1200 mm"}, {"entered_unit", "mm"},
            {"exact_metres", {{"numerator", 6}, {"denominator", 5}}}}},
        {"/going_m", {{"version", 1}, {"original_expression", "250 mm"}, {"entered_unit", "mm"},
            {"exact_metres", {{"numerator", 1}, {"denominator", 4}}}}},
        {"/future_dimension_m", {{"version", 9}, {"future_payload", "retain verbatim"}}}};
    host.extensions["name"] = "Hall stairs";
    document.apply(sketch::ApplyEntityChanges{document.snapshot().revision(),
        {sketch::EntityChange::upsert(level_entity), sketch::EntityChange::upsert(host)}, {}, "host"});
    const auto source = document.snapshot();
    BuildingObjectDialog dialog(source, std::nullopt, true);
    select_form(dialog, "stair_flight_railing");
    auto& hosts = combo(dialog, "buildingObjectStairHost");
    hosts.setCurrentIndex(hosts.findData(QStringLiteral("host")));
    require(dialog.submit(), "hosted railing should submit with explicit v1 host choice");
    require(dialog.relatedCandidates().size() == 1 && dialog.coordinatedCandidates().size() == 2,
            "v1 host upgrade and railing must be coordinated candidates");
    const auto upgrade = dialog.relatedCandidates().front();
    const auto railing = sketch::decode_railing_properties(dialog.candidate()->id, dialog.candidate()->properties);
    const auto upgraded_stair = sketch::decode_stair_properties(upgrade.id, upgrade.properties);
    require(railing.host && railing.host->flight_id == upgraded_stair.flights.front().id &&
                upgrade.properties.at("future_host") == 23 && upgrade.extensions == host.extensions,
            "hosted railing must reference generated stable flight while preserving host metadata");
    auto expected_upgrade = host.properties;
    for (const auto* key : {"version", "form", "flights", "landings"}) expected_upgrade[key] = upgrade.properties.at(key);
    require(upgrade.properties == expected_upgrade && upgraded_stair.flights.front().id != host.id &&
                !upgraded_stair.flights.front().id.empty(),
            "host upgrade must change only topology schema and retain all nested metadata and receipts");
    require_quantity_receipt(upgrade, "/top_landing/depth_m", "1200 mm", "mm", 6, 5);
    const auto old_layout = sketch::derive_stair_layout(sketch::decode_stair_properties(host.id, host.properties));
    const auto upgraded_layout = sketch::derive_stair_layout(upgraded_stair);
    require(old_layout.flights.front().run == upgraded_layout.flights.front().run &&
                old_layout.flights.front().rise == upgraded_layout.flights.front().rise &&
                old_layout.flights.front().orientation_radians == upgraded_layout.flights.front().orientation_radians &&
                old_layout.landings.front().elevation == upgraded_layout.landings.front().elevation,
            "implicit host upgrade must preserve stair and top landing geometry");
    for (std::size_t index = 0; index < old_layout.flights.front().treads.size(); ++index) {
        for (std::size_t corner = 0; corner < 4; ++corner) {
            const auto before = old_layout.flights.front().treads[index].footprint[corner];
            const auto after = upgraded_layout.flights.front().treads[index].footprint[corner];
            require(before.x == after.x && before.y == after.y && before.z == after.z,
                    "upgrade must preserve every original tread corner");
        }
    }
    require(!dialog.candidate()->properties.contains("base_position_m") &&
                !dialog.candidate()->properties.contains("length_m"), "hosted rail must not persist independent placement");
    require(document.snapshot().entities() == source.entities(), "submit must not mutate source document");
    BuildingObjectDialog cancel(source, std::nullopt, true);
    select_form(cancel, "stair_flight_railing");
    combo(cancel, "buildingObjectStairHost").setCurrentIndex(1);
    cancel.reject();
    require(!cancel.candidate() && cancel.relatedCandidates().empty() &&
                document.snapshot().entities() == source.entities(), "Cancel must discard candidates and source mutation");
    BuildingObjectDialog invalid(source, std::nullopt, true);
    select_form(invalid, "stair_flight_railing");
    require(!invalid.submit() && !invalid.candidate(), "host selection must be explicit");
    combo(invalid, "buildingObjectStairHost").setCurrentIndex(1);
    const auto selected_flight = combo(invalid, "buildingObjectStairHostFlight").currentData();
    set_field(invalid, "buildingObjectHostStart", "0.8");
    set_field(invalid, "buildingObjectHostEnd", "0.2");
    require(!invalid.submit() && invalid.relatedCandidates().empty(), "invalid hosted station range must discard all candidates");
    set_field(invalid, "buildingObjectHostStart", "0");
    set_field(invalid, "buildingObjectHostEnd", "1");
    require(invalid.submit() && combo(invalid, "buildingObjectStairHostFlight").currentData() == selected_flight &&
                sketch::decode_railing_properties(invalid.candidate()->id, invalid.candidate()->properties).host->flight_id == selected_flight.toString().toStdString(),
            "retry must retain the generated stable flight identity");

    auto existing_rail = *dialog.candidate();
    existing_rail.properties["host"]["future_anchor"] = "preserved";
    existing_rail.properties["future_rail"] = 52;
    existing_rail.extensions["future_metadata"] = "rail";
    const auto upgraded_source = sketch::Document::create({level_entity, upgrade}).snapshot();
    BuildingObjectDialog edit(upgraded_source, existing_rail, true);
    require(combo(edit, "buildingObjectStairHostFlight").currentData().toString().toStdString() == railing.host->flight_id,
            "hosted edit must select the original stable flight");
    require(edit.findChild<QLineEdit*>("buildingObjectBaseX") == nullptr &&
                edit.findChild<QLineEdit*>("buildingObjectLength") == nullptr,
            "hosted editor must not expose independently authored placement");
    combo(edit, "buildingObjectStairHostSide").setCurrentIndex(1);
    set_field(edit, "buildingObjectHeight", "3 ft");
    require(edit.submit() && edit.relatedCandidates().empty(), "v2 hosted edit must not upgrade any host");
    const auto changed_rail = *edit.candidate();
    const auto changed = sketch::decode_railing_properties(changed_rail.id, changed_rail.properties);
    require(changed.host->flight_id == railing.host->flight_id && changed.host->side == sketch::StairRailingSide::right &&
                changed_rail.properties.at("host").at("future_anchor") == "preserved" &&
                changed_rail.properties.at("future_rail") == 52 && changed_rail.extensions == existing_rail.extensions,
            "hosted edit must preserve identity and opaque metadata while changing authored side");
    require_quantity_receipt(changed_rail, "/height_m", "3 ft", "ft", 1143, 1250);
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        for (int index = 1; index < argc; ++index) {
            const auto argument = QString::fromLocal8Bit(argv[index]);
            if (argument == QStringLiteral("--capture-directory")) {
                if (index + 1 >= argc) {
                    throw std::runtime_error(
                        "--capture-directory requires a destination path");
                }
                capture_directory = QString::fromLocal8Bit(argv[++index]);
                if (capture_directory->trimmed().isEmpty()) {
                    throw std::runtime_error(
                        "--capture-directory requires a destination path");
                }
            } else {
                throw std::runtime_error("unknown test argument: " +
                                         argument.toStdString());
            }
        }
        test_all_forms_submit_to_entities();
        test_invalid_input_stays_open_with_inline_error();
        test_malformed_original_disables_submit_without_throwing();
        test_edit_merges_canonical_fields_and_preserves_metadata();
        test_edit_preserves_untouched_high_precision_fields();
        test_imperial_defaults_use_shared_quantity_input();
        test_untouched_defaults_submit_identically_in_both_units();
        test_quantity_receipts_capture_exact_and_nested_inputs();
        test_suffixless_receipt_survives_metric_edit_display();
        test_changed_quantity_replaces_only_its_receipt();
        test_untouched_unknown_receipts_survive_unrelated_edit();
        test_stale_invalid_and_oversized_receipts_do_not_override_display();
        test_failed_submit_does_not_leak_receipts_across_candidates();
        test_roof_openings_create_edit_remove_and_preserve_units();
        test_roof_opening_validation_is_atomic();
        test_roof_form_changes_preserve_opening_draft();
        test_multiflight_widgets_preserve_identity_and_metadata();
        test_hosted_railing_candidates_are_atomic_and_cancel_is_immutable();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

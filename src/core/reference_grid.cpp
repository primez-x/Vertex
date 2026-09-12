#include "sketch/reference_grid.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

void exact_fields(const Json& value, std::initializer_list<const char*> fields) {
    if (!value.is_object() || value.size() != fields.size())
        invalid("Reference grid JSON fields are invalid");
    for (const auto* field : fields) {
        if (!value.contains(field)) invalid("Reference grid JSON field is missing");
    }
}

double finite_number(const Json& value, const char* name) {
    if (!value.is_number()) invalid(std::string("Reference grid ") + name + " must be finite");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid(std::string("Reference grid ") + name + " must be finite");
    return result;
}

std::int32_t bounded_integer(const Json& value, const char* name,
                             std::int32_t minimum, std::int32_t maximum) {
    if (!value.is_number_integer() && !value.is_number_unsigned())
        invalid(std::string("Reference grid ") + name + " must be an integer");
    std::int64_t result = 0;
    try {
        if (value.is_number_unsigned()) {
            const auto unsigned_value = value.get<std::uint64_t>();
            if (unsigned_value > static_cast<std::uint64_t>(maximum))
                invalid(std::string("Reference grid ") + name + " is outside its limit");
            result = static_cast<std::int64_t>(unsigned_value);
        } else {
            result = value.get<std::int64_t>();
        }
    } catch (const Json::exception&) {
        invalid(std::string("Reference grid ") + name + " is outside its limit");
    }
    if (result < minimum || result > maximum)
        invalid(std::string("Reference grid ") + name + " is outside its limit");
    return static_cast<std::int32_t>(result);
}

void validate_label(const std::string& label, const char* name) {
    if (label.empty() || label.size() > 32 ||
        std::any_of(label.begin(), label.end(), [](unsigned char character) {
            return character < 0x20U || character == 0x7fU;
        })) {
        invalid(std::string("Reference grid ") + name + " is invalid");
    }
    try {
        (void)Json(label).dump();
    } catch (const Json::exception&) {
        invalid(std::string("Reference grid ") + name + " is not valid UTF-8");
    }
}

void validate_model(const ReferenceGridModel& model) {
    if (!std::isfinite(model.origin_m.x) || !std::isfinite(model.origin_m.y))
        invalid("Reference grid origin must be finite");
    if (!std::isfinite(model.rotation_radians))
        invalid("Reference grid rotation must be finite");
    for (const auto [value, name] : {std::pair{model.spacing_x_m, "spacing_x_m"},
                                     std::pair{model.spacing_y_m, "spacing_y_m"}}) {
        if (!std::isfinite(value) || value <= 0.0 || value > 1.0e6)
            invalid(std::string("Reference grid ") + name + " is outside its limit");
    }
    if (model.count_x < 1 || model.count_x > ReferenceGridModel::maximum_lines_from_origin ||
        model.count_y < 1 || model.count_y > ReferenceGridModel::maximum_lines_from_origin)
        invalid("Reference grid line count is outside its limit");
    if (model.major_every < 1 || model.major_every > ReferenceGridModel::maximum_major_interval)
        invalid("Reference grid major interval is outside its limit");
    validate_label(model.x_label, "x_label");
    validate_label(model.y_label, "y_label");
}

Vec2 rotate_and_translate(Vec2 point, const ReferenceGridModel& model) {
    const auto cosine = std::cos(model.rotation_radians);
    const auto sine = std::sin(model.rotation_radians);
    return {model.origin_m.x + point.x * cosine - point.y * sine,
            model.origin_m.y + point.x * sine + point.y * cosine};
}

}  // namespace

nlohmann::json ReferenceGridModel::to_json() const {
    validate_model(*this);
    return Json{{"version", 1},
                {"origin_m", {origin_m.x, origin_m.y}},
                {"rotation_radians", rotation_radians},
                {"spacing_x_m", spacing_x_m},
                {"spacing_y_m", spacing_y_m},
                {"count_x", count_x},
                {"count_y", count_y},
                {"major_every", major_every},
                {"x_label", x_label},
                {"y_label", y_label},
                {"visible", visible}};
}

ReferenceGridModel ReferenceGridModel::from_json(const nlohmann::json& value) {
    try {
        exact_fields(value, {"version", "origin_m", "rotation_radians", "spacing_x_m",
                             "spacing_y_m", "count_x", "count_y", "major_every", "x_label",
                             "y_label", "visible"});
        if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.at("origin_m").is_array() || value.at("origin_m").size() != 2 ||
            !value.at("x_label").is_string() || !value.at("y_label").is_string() ||
            !value.at("visible").is_boolean()) {
            invalid("Reference grid JSON value is invalid");
        }
        ReferenceGridModel result;
        result.origin_m = {finite_number(value.at("origin_m")[0], "origin_m.x"),
                           finite_number(value.at("origin_m")[1], "origin_m.y")};
        result.rotation_radians = finite_number(value.at("rotation_radians"), "rotation_radians");
        result.spacing_x_m = finite_number(value.at("spacing_x_m"), "spacing_x_m");
        result.spacing_y_m = finite_number(value.at("spacing_y_m"), "spacing_y_m");
        result.count_x = bounded_integer(value.at("count_x"), "count_x", 1,
                                         maximum_lines_from_origin);
        result.count_y = bounded_integer(value.at("count_y"), "count_y", 1,
                                         maximum_lines_from_origin);
        result.major_every = bounded_integer(value.at("major_every"), "major_every", 1,
                                             maximum_major_interval);
        result.x_label = value.at("x_label").get<std::string>();
        result.y_label = value.at("y_label").get<std::string>();
        result.visible = value.at("visible").get<bool>();
        validate_model(result);
        return result;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("Invalid reference grid JSON: ") + error.what());
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid reference grid JSON: ") + error.what());
    }
}

std::vector<ReferenceGridLine> ReferenceGridModel::lines() const {
    validate_model(*this);
    std::vector<ReferenceGridLine> result;
    result.reserve(static_cast<std::size_t>(2 * count_x + 1 + 2 * count_y + 1));
    const auto is_major = [&](std::int32_t index) {
        return index % major_every == 0;
    };
    for (std::int32_t index = -count_x; index <= count_x; ++index) {
        const auto x = static_cast<double>(index) * spacing_x_m;
        result.push_back({rotate_and_translate({x, -static_cast<double>(count_y) * spacing_y_m}, *this),
                          rotate_and_translate({x, static_cast<double>(count_y) * spacing_y_m}, *this),
                          ReferenceGridAxis::x, index, is_major(index)});
    }
    for (std::int32_t index = -count_y; index <= count_y; ++index) {
        const auto y = static_cast<double>(index) * spacing_y_m;
        result.push_back({rotate_and_translate({-static_cast<double>(count_x) * spacing_x_m, y}, *this),
                          rotate_and_translate({static_cast<double>(count_x) * spacing_x_m, y}, *this),
                          ReferenceGridAxis::y, index, is_major(index)});
    }
    return result;
}

}  // namespace sketch

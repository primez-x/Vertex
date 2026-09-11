#include "sketch/document_schedule_adapter.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

sketch::Entity entity(std::string id, std::string type, nlohmann::json properties) {
    return {std::move(id), std::move(type), std::move(properties), false, nlohmann::json::object()};
}

void test_revision_and_opening_schedule() {
    using namespace sketch;
    auto document = Document::create({
        entity("door-1", "opening", {{"opening_kind", "door"}, {"mark", "D1"},
            {"width_m", 0.9}, {"height_m", 2.1}}),
        entity("window-1", "opening", {{"opening_kind", "window"}, {"width_m", 1.2},
            {"height_m", 1.0}, {"sill_m", 0.9}}),
    });
    const auto projection = build_document_schedules(document.snapshot());
    require(projection.snapshot.revision == document.revision(),
            "schedule projection must bind its source revision");
    require(projection.snapshot.rows.size() == 2 && projection.snapshot.rows.front().mark == "D1" &&
                projection.snapshot.rows.front().cells.contains("area"),
            "opening schedule must include deterministic rows and calculated area");
    const auto& area = projection.snapshot.rows.front().cells.at("area");
    require(!area.editable && area.sources.size() == 2 &&
                area.explanation == "Width multiplied by height",
            "calculated opening area must expose read-only source provenance");
    require(!projection.diagnostics.empty(), "missing window mark must remain visible as a diagnostic");
}

void test_room_area_and_invalid_rows_are_explicit() {
    using namespace sketch;
    const nlohmann::json square_boundary = nlohmann::json::array({
        {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}}, {"sweep_radians", 0.0}},
        {{"start", {4.0, 0.0}}, {"end", {4.0, 3.0}}, {"sweep_radians", 0.0}},
        {{"start", {4.0, 3.0}}, {"end", {0.0, 3.0}}, {"sweep_radians", 0.0}},
        {{"start", {0.0, 3.0}}, {"end", {0.0, 0.0}}, {"sweep_radians", 0.0}},
    });
    auto document = Document::create({
        entity("room-1", "room", {{"name", "Kitchen"}, {"boundary", square_boundary}}),
        entity("broken-window", "opening", {{"opening_kind", "window"}, {"width_m", 0.0},
            {"height_m", 1.0}}),
    });
    const auto projection = build_document_schedules(document.snapshot());
    require(projection.snapshot.rows.size() == 1 && projection.snapshot.rows.front().kind == ScheduleRowKind::room,
            "room boundary area must produce a room schedule row");
    require(std::get<ScheduleQuantity>(projection.snapshot.rows.front().cells.at("gross_area").value).value == 12.0,
            "room schedule area must come from the closed boundary");
    require(std::any_of(projection.diagnostics.begin(), projection.diagnostics.end(),
                        [](const auto& message) { return message.find("broken-window") != std::string::npos; }),
            "invalid opening must be reported without a partial schedule row");
}

}  // namespace

int main() {
    try {
        test_revision_and_opening_schedule();
        test_room_area_and_invalid_rows_are_explicit();
        std::cout << "document_schedule_adapter_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

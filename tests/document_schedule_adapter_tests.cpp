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
    const auto& gross_area = projection.snapshot.rows.front().cells.at("gross_area");
    require(std::get<ScheduleQuantity>(gross_area.value).value == 12.0 && !gross_area.editable &&
                gross_area.sources.size() == 1 && gross_area.sources.front().property == "boundary",
            "room schedule area must come from the closed boundary and remain calculated");
    require(std::any_of(projection.diagnostics.begin(), projection.diagnostics.end(),
                        [](const auto& message) { return message.find("broken-window") != std::string::npos; }),
            "invalid opening must be reported without a partial schedule row");
}

void test_edit_is_a_document_command_with_revision_and_undo_semantics() {
    using namespace sketch;
    auto document = Document::create({
        entity("door-1", "opening", { {"opening_kind", "door"}, {"mark", "D1"},
            {"width_m", 0.9}, {"height_m", 2.1} }),
    });
    const auto projection = build_document_schedules(document.snapshot());
    const auto edit = make_schedule_edit(projection.snapshot, "door-1", "width",
                                         ScheduleQuantity{1.1, ScheduleUnit::metre});
    const auto command = make_document_schedule_edit(document.snapshot(), edit);
    require(command.expected_revision == document.revision() && command.entity_changes.size() == 1,
            "schedule edit must produce a revision-checked document command");
    const auto revision = document.apply(command);
    require(revision == 1 && document.snapshot().entities().at("door-1").properties.at("width_m") == 1.1,
            "schedule command must update the canonical opening source");
    require(document.undo(document.revision()) == 2 &&
                document.snapshot().entities().at("door-1").properties.at("width_m") == 0.9,
            "schedule edit must participate in document undo");
    require(document.redo(document.revision()) == 3 &&
                document.snapshot().entities().at("door-1").properties.at("width_m") == 1.1,
            "schedule edit must participate in document redo");
    const auto stale = edit;
    try {
        (void)make_document_schedule_edit(document.snapshot(), stale);
        throw std::runtime_error("stale schedule edit was accepted");
    } catch (const DocumentError& error) {
        require(error.code() == DocumentErrorCode::stale_revision,
                "stale schedule edit must fail with the document revision error");
    }
    const auto current = build_document_schedules(document.snapshot());
    const auto area = current.snapshot.rows.front().cells.at("area");
    try {
        (void)make_schedule_edit(current.snapshot, "door-1", "area",
                                 ScheduleQuantity{2.0, ScheduleUnit::square_metre});
        throw std::runtime_error("calculated schedule cell was accepted");
    } catch (const std::invalid_argument&) {
        // The read-only provenance guard is the expected result.
    }
    require(!area.editable, "opening area must remain a calculated cell");
}

}  // namespace

int main() {
    try {
        test_revision_and_opening_schedule();
        test_room_area_and_invalid_rows_are_explicit();
        test_edit_is_a_document_command_with_revision_and_undo_semantics();
        std::cout << "document_schedule_adapter_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

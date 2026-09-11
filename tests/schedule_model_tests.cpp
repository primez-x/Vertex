#include "sketch/schedule_model.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
template<class F> void rejects(F action, const std::string& expected) {
    try { action(); } catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
                "Rejection must explain the failure");
        return;
    }
    require(false, "Expected rejection");
}
}

int main() {
    using namespace sketch;
    std::vector<ScheduleRecord> records{
        {"door-2", "D2", ScheduleRowKind::door,
            {{"width", ScheduleQuantity{0.9, ScheduleUnit::metre}}, {"count", std::int64_t{1}},
             {"fire_rated", true}}, {}},
        {"door-1", "D1", ScheduleRowKind::door, {{"description", std::string("Entry")}}, {}},
        {"window-1", "W1", ScheduleRowKind::window, {{"height", ScheduleQuantity{1.2}}}, {}},
        {"room-1", "101", ScheduleRowKind::room,
            {{"gross_area", ScheduleQuantity{20, ScheduleUnit::square_metre}},
             {"deductions", ScheduleQuantity{2, ScheduleUnit::square_metre}}},
            {{"net_area", {ScheduleQuantity{18, ScheduleUnit::square_metre},
                {{"room-1", "gross_area"}, {"room-1", "deductions"}}, "Gross area minus deductions"}}}},
        {"material-1", "M1", ScheduleRowKind::material,
            {{"name", std::string("Brick")}, {"volume", ScheduleQuantity{3, ScheduleUnit::cubic_metre}}}, {}}
    };
    const auto snapshot = build_schedule(records, 7);
    require(snapshot.rows.size() == 5 && snapshot.rows.front().object_id == "door-1",
        "Rows must preserve object identities and have deterministic ordering");
    const auto edit = make_schedule_edit(snapshot, "door-2", "width", ScheduleQuantity{1.0});
    require(edit == make_schedule_edit(snapshot, "door-2", "width", ScheduleQuantity{1.0}),
        "Repeated edit requests must produce identical command descriptions");
    require(edit.expected_revision == 7 && edit.target == ScheduleSourceRef{"door-2", "width"} &&
        edit.before == ScheduleValue{ScheduleQuantity{0.9}}, "Edit must carry source and optimistic preconditions");
    require(build_schedule(records, 7) == snapshot, "Command creation must not mutate source records");
    rejects([&] { (void)make_schedule_edit(snapshot, "room-1", "net_area", ScheduleQuantity{1, ScheduleUnit::square_metre}); }, "room-1.gross_area");
    rejects([&] { (void)make_schedule_edit(snapshot, "door-2", "width", 1.0); }, "type and unit");
    rejects([&] { (void)make_schedule_edit(snapshot, "door-2", "width", ScheduleQuantity{1, ScheduleUnit::square_metre}); }, "type and unit");
    rejects([&] { (void)make_schedule_edit(snapshot, "door-2", "mark", std::string("D1")); }, "Duplicate");
    rejects([&] { (void)make_schedule_edit(snapshot, "door-2", "mark", std::string{}); }, "empty");
    rejects([&] { (void)make_schedule_edit(snapshot, "absent", "mark", std::string("D3")); }, "row not found");
    rejects([&] { (void)make_schedule_edit(snapshot, "door-2", "absent", true); }, "column not found");
    rejects([&] { (void)make_schedule_edit(snapshot, "door-2", "width", ScheduleQuantity{std::numeric_limits<double>::infinity()}); }, "finite");
    auto invalid = records;
    invalid.push_back(records.front());
    rejects([&] { (void)build_schedule(invalid, 7); }, "duplicate schedule object ID");
    invalid = records;
    invalid[1].mark = "D2";
    rejects([&] { (void)build_schedule(invalid, 7); }, "duplicate schedule mark");
    invalid = records;
    invalid[3].calculated.at("net_area").sources.front().property = "absent";
    rejects([&] { (void)build_schedule(invalid, 7); }, "Missing schedule source");
    invalid = records;
    invalid[3].calculated.at("net_area").sources.clear();
    rejects([&] { (void)build_schedule(invalid, 7); }, "explanation and sources");
    invalid = records;
    invalid[3].properties.emplace("net_area", 1.0);
    rejects([&] { (void)build_schedule(invalid, 7); }, "ambiguous");
    records[0].properties["width"] = edit.after;
    require(build_schedule(records, 8).rows[1].cells.at("width").value == edit.after,
        "Regeneration must reflect accepted semantic changes");
    std::cout << "schedule_model_tests passed\n";
}

#include "sketch/windows_appraisal_boundary.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

using namespace sketch;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
AppraisalMappingDescriptor mapping() {
  return {{1,0}, "fixture.appraisal", "Synthetic Appraisal", "fixture-1", "fixture-json",
          "Synthetic preparation only", {{"area", "gla", true}}, 3000};
}
template<class F> void until(F condition) {
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!condition()) {
    check(std::chrono::steady_clock::now() < deadline, "fixture supervision timed out");
    std::this_thread::sleep_for(1ms);
  }
}
void dispatch(const std::filesystem::path &exe, const char *id, AppraisalOutcome expected) {
  auto boundary = std::make_unique<WindowsAppraisalBoundary>(WindowsAppraisalOptions{exe});
  AppraisalDispatcher d(mapping(), {{"area", "123.5"}}, id, std::move(boundary));
  until([&] { return d.poll().has_value(); });
  check(d.poll()->outcome == expected, id);
  check(d.poll()->prepared_payload.has_value() == (expected == AppraisalOutcome::success), "unexpected prepared data");
}
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "fixture executable required");
    std::filesystem::path exe = argv[1];
#ifdef _WIN32
    SetEnvironmentVariableW(L"VERTEX_APPRAISAL_PARENT_SECRET",L"fixture-only-sentinel");
#endif
    bool rejected=false;
    try { WindowsAppraisalBoundary invalid({"relative-worker.exe"}); }
    catch(const std::invalid_argument &) { rejected=true; }
    check(rejected,"relative executable accepted");
    rejected=false;
    try { WindowsAppraisalBoundary invalid({exe,0}); }
    catch(const std::invalid_argument &) { rejected=true; }
    check(rejected,"zero frame limit accepted");
    dispatch(exe, "success", AppraisalOutcome::success);
    dispatch(exe, "exit-success", AppraisalOutcome::success);
    dispatch(exe, "malformed", AppraisalOutcome::uncertain_response);
    dispatch(exe, "oversize", AppraisalOutcome::uncertain_response);
    dispatch(exe, "duplicate", AppraisalOutcome::uncertain_response);
    dispatch(exe, "truncated", AppraisalOutcome::uncertain_response);
    dispatch(exe, "crash", AppraisalOutcome::worker_failure);
    dispatch(exe, "wrong-id", AppraisalOutcome::uncertain_response);
    dispatch(exe.parent_path() / "missing-appraisal-fixture.exe", "missing", AppraisalOutcome::worker_failure);
    auto b = std::make_unique<WindowsAppraisalBoundary>(WindowsAppraisalOptions{exe});
    auto *observed = b.get();
    auto now = AppraisalDispatcher::Clock::now();
    AppraisalDispatcher d(mapping(), {{"area", "123.5"}}, "late", std::move(b), {}, [&] { return now; });
    until([&] { return observed->process_id() != 0; });
    now += 3s;
    check(d.poll()->outcome == AppraisalOutcome::timeout, "absolute deadline not respected");
    until([&] { return observed->supervisor_finished(); });
    check(!observed->poll(), "abandon accepted late output");
    check(d.poll()->outcome == AppraisalOutcome::timeout, "late output changed terminal result");
    WindowsAppraisalBoundary pending({exe});
    pending.negotiate("hang");
    until([&] { return pending.process_id() != 0; });
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pending.process_id());
    check(process != nullptr, "cannot observe launched fixture");
#endif
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) check(!pending.poll(), "hung worker unexpectedly replied");
    pending.abandon(); pending.abandon();
    check(std::chrono::steady_clock::now() - start < 250ms, "poll or abandon waited for worker");
    until([&] { return pending.supervisor_finished(); });
#ifdef _WIN32
    check(WaitForSingleObject(process, 2000) == WAIT_OBJECT_0, "abandon did not terminate worker");
    CloseHandle(process);
#endif
    check(!pending.poll(), "abandoned session returned a reply");
    WindowsAppraisalBoundary blocked_write({exe});
    blocked_write.negotiate("no-read");
    until([&] { return blocked_write.poll().has_value(); });
    start=std::chrono::steady_clock::now();
    blocked_write.prepare("no-read",std::string(500000,'x'));
    check(std::chrono::steady_clock::now()-start<250ms,"prepare waited for pipe reader");
    std::this_thread::sleep_for(50ms);
    blocked_write.abandon();
    until([&] { return blocked_write.supervisor_finished(); });
    WindowsAppraisalBoundary tree({exe});
    tree.negotiate("tree");
    std::optional<AppraisalWorkerReply> tree_reply;
    until([&] { tree_reply=tree.poll(); return tree_reply.has_value(); });
    check(tree_reply->kind==AppraisalReplyKind::capabilities && tree_reply->capabilities.size()==1,"tree fixture did not acknowledge child");
#ifdef _WIN32
    HANDLE descendant=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(std::stoul(tree_reply->capabilities.front().provenance)));
    check(descendant!=nullptr,"cannot observe descendant");
    tree.abandon();
    check(WaitForSingleObject(descendant,2000)==WAIT_OBJECT_0,"abandon did not terminate descendant");
    CloseHandle(descendant);
#endif
    WindowsAppraisalBoundary early({exe});
    early.abandon();
    until([&] { return early.supervisor_finished(); });
    check(!early.poll(),"launch race published a reply after abandon");
    std::cout << "Windows appraisal transport fixture cases passed\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

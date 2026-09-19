#include "sketch/appraisal_dispatcher.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace sketch;
using namespace std::chrono_literals;
void check(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
AppraisalMappingDescriptor mapping() {
  return {{1, 0}, "fixture.appraisal", "Synthetic Appraisal", "fixture-1", "fixture-json",
          "Synthetic preparation only", {{"area", "gla", true}, {"note", "comment", false}}, 10};
}
struct FixtureState {
  AppraisalDispatcher::Clock::time_point now{};
  std::optional<AppraisalWorkerReply> reply;
  std::string payload;
  int negotiations{}, preparations{}, polls{}, abandons{};
  bool fail{}, expire_during_call{}, fail_negotiate{}, fail_prepare{};
  bool expire_negotiate{}, expire_prepare{};
  std::stop_source stop;
  bool cancel_during_call{};
};
class FixtureBoundary final : public AppraisalCallBoundary {
public:
  explicit FixtureBoundary(std::shared_ptr<FixtureState> s) : s_(std::move(s)) {}
  void negotiate(const std::string &id) override {
    check(id == "request-1", "request correlation not sent"); ++s_->negotiations;
    if (s_->expire_negotiate) s_->now += 10ms;
    if (s_->fail_negotiate) throw std::runtime_error("synthetic launch failure");
  }
  void prepare(const std::string &id, const std::string &payload) override {
    check(id == "request-1", "prepare correlation not sent");
    ++s_->preparations; s_->payload = payload;
    if (s_->expire_prepare) s_->now += 10ms;
    if (s_->fail_prepare) throw std::runtime_error("synthetic prepare failure");
  }
  std::optional<AppraisalWorkerReply> poll() override {
    ++s_->polls;
    if (s_->expire_during_call) s_->now += 10ms;
    if (s_->cancel_during_call) s_->stop.request_stop();
    if (s_->fail) throw std::runtime_error("synthetic worker crash");
    auto reply = std::move(s_->reply); s_->reply.reset(); return reply;
  }
  void abandon() noexcept override { ++s_->abandons; }
private:
  std::shared_ptr<FixtureState> s_;
};
auto dispatcher(const std::shared_ptr<FixtureState> &s) {
  return std::make_unique<AppraisalDispatcher>(mapping(),
      std::map<std::string, std::string>{{"area", "123.5"}}, "request-1",
      std::make_unique<FixtureBoundary>(s), s->stop.get_token(), [s] { return s->now; });
}
void capabilities(const std::shared_ptr<FixtureState> &s,
                  std::vector<AppraisalMappingDescriptor> descriptors = {mapping()}) {
  s->reply = AppraisalWorkerReply{AppraisalReplyKind::capabilities, "request-1", std::move(descriptors), {}};
}
void prepared(const std::shared_ptr<FixtureState> &s) {
  s->reply = AppraisalWorkerReply{AppraisalReplyKind::prepared, "request-1", {}, s->payload};
}
void expect(AppraisalDispatcher &d, AppraisalOutcome outcome) {
  const auto &r = d.poll();
  check(r && r->outcome == outcome, "wrong typed dispatcher outcome");
  if (outcome != AppraisalOutcome::success) check(!r->prepared_payload, "failure leaked prepared data");
}
void run() {
  // Removing the exact negotiation or preparation handshake must fail this case.
  auto s = std::make_shared<FixtureState>(); auto d = dispatcher(s);
  check(!d->poll(), "pending negotiation must not become terminal");
  capabilities(s); check(!d->poll(), "negotiation alone is not preparation");
  check(s->payload.find("\"gla\":\"123.5\"") != std::string::npos, "mapped payload not sent");
  prepared(s); expect(*d, AppraisalOutcome::success);
  check(d->poll()->prepared_payload == s->payload, "successful data lost");
  check(d->source_values().at("area") == "123.5", "original diagnostic values lost");
  const auto calls = s->polls; s->now += 1h; expect(*d, AppraisalOutcome::success);
  check(s->polls == calls && s->abandons == 1, "terminal session was reused");

  // Every identity and mapping component is exact; no downgrade or loose match.
  for (int fault = 0; fault < 8; ++fault) {
    s = std::make_shared<FixtureState>(); d = dispatcher(s); auto m = mapping();
    switch (fault) {
      case 0: m.protocol_version.minor = 1; break;
      case 1: m.adapter_id = "other"; break;
      case 2: m.application = "Other"; break;
      case 3: m.application_version = "fixture-2"; break;
      case 4: m.exchange_format = "other-json"; break;
      case 5: m.fields[0].target_field = "other"; break;
      case 6: m.fields[0].source_field = "other"; break;
      case 7: m.fields[0].required = false; break;
    }
    capabilities(s, {m}); expect(*d, AppraisalOutcome::unsupported);
    check(s->preparations == 0, "mismatched capability executed");
  }
  s = std::make_shared<FixtureState>(); d = dispatcher(s);
  capabilities(s, {mapping(), mapping()}); expect(*d, AppraisalOutcome::ambiguous);
  check(s->preparations == 0, "ambiguous selection executed");

  for (const bool cancel : {false, true}) {
    for (const bool during : {false, true}) {
      s = std::make_shared<FixtureState>(); d = dispatcher(s);
      capabilities(s); check(!d->poll(), "prepare pending expected"); prepared(s);
      if (during) { s->expire_during_call = !cancel; s->cancel_during_call = cancel; }
      else if (cancel) s->stop.request_stop(); else s->now += 10ms;
      expect(*d, cancel ? AppraisalOutcome::cancelled : AppraisalOutcome::timeout);
      const auto polls = s->polls; prepared(s);
      expect(*d, cancel ? AppraisalOutcome::cancelled : AppraisalOutcome::timeout);
      check(s->polls == polls && s->abandons == 1, "late success escaped terminal state");
    }
  }
  for (const auto [kind, outcome] : {
      std::pair{AppraisalReplyKind::offline, AppraisalOutcome::offline},
      {AppraisalReplyKind::unavailable, AppraisalOutcome::unavailable},
      {AppraisalReplyKind::unsupported, AppraisalOutcome::unsupported},
      {AppraisalReplyKind::ambiguous, AppraisalOutcome::ambiguous},
      {AppraisalReplyKind::worker_failure, AppraisalOutcome::worker_failure},
      {AppraisalReplyKind::uncertain_response, AppraisalOutcome::uncertain_response}}) {
    s = std::make_shared<FixtureState>(); d = dispatcher(s);
    s->reply = AppraisalWorkerReply{kind, "request-1", {}, {}}; expect(*d, outcome);
  }
  s = std::make_shared<FixtureState>(); d = dispatcher(s); s->fail = true;
  expect(*d, AppraisalOutcome::worker_failure);
  for (int fault = 0; fault < 4; ++fault) {
    s = std::make_shared<FixtureState>(); d = dispatcher(s);
    if (fault != 0) { capabilities(s); check(!d->poll(), "preparation must be pending"); }
    prepared(s);
    if (fault == 1) s->reply->request_id = "stale-request";
    if (fault == 2) s->reply->prepared_payload = "changed payload";
    if (fault == 3) s->reply->capabilities = {mapping()};
    expect(*d, AppraisalOutcome::uncertain_response);
  }
  s = std::make_shared<FixtureState>(); s->stop.request_stop(); d = dispatcher(s);
  expect(*d, AppraisalOutcome::cancelled); check(s->negotiations == 0, "pre-cancelled request started");
  s = std::make_shared<FixtureState>(); { auto pending = dispatcher(s); }
  check(s->abandons == 1, "abandoned caller left session active");

  // The budget includes negotiation and preparation, never resets between them.
  s = std::make_shared<FixtureState>(); d = dispatcher(s); s->now += 9ms;
  capabilities(s); check(!d->poll(), "negotiation before deadline failed");
  prepared(s); s->now += 1ms; expect(*d, AppraisalOutcome::timeout);
  s = std::make_shared<FixtureState>(); d = dispatcher(s);
  s->now += 10ms; capabilities(s); expect(*d, AppraisalOutcome::timeout);
  check(s->preparations == 0, "expired negotiation started preparation");
  for (const bool prepare_stage : {false, true}) {
    for (const bool expire : {false, true}) {
      s = std::make_shared<FixtureState>();
      if (prepare_stage) { s->fail_prepare = !expire; s->expire_prepare = expire; }
      else { s->fail_negotiate = !expire; s->expire_negotiate = expire; }
      d = dispatcher(s);
      if (prepare_stage) capabilities(s);
      expect(*d, expire ? AppraisalOutcome::timeout : AppraisalOutcome::worker_failure);
      check(s->abandons == 1, "failed boundary session not abandoned");
    }
  }
  // Capability mapping order and local policy metadata do not change capability.
  s = std::make_shared<FixtureState>(); d = dispatcher(s); auto reordered = mapping();
  std::swap(reordered.fields[0], reordered.fields[1]);
  reordered.provenance = "Other synthetic source"; reordered.timeout_ms = 99;
  capabilities(s, {reordered}); check(!d->poll(), "equivalent capability rejected");
  prepared(s); s->now += 9ms; expect(*d, AppraisalOutcome::success);
  for (int fault = 0; fault < 5; ++fault) {
    s = std::make_shared<FixtureState>(); d = dispatcher(s); capabilities(s);
    if (fault == 0) s->reply->capabilities[0].fields[0].target_field.clear();
    if (fault == 1) s->reply->capabilities.resize(65, mapping());
    if (fault == 2) s->reply->prepared_payload = "unexpected data";
    if (fault == 3) s->reply->request_id.clear();
    if (fault == 4) s->reply->kind = static_cast<AppraisalReplyKind>(99);
    expect(*d, AppraisalOutcome::uncertain_response);
    check(s->preparations == 0, "malformed capability started preparation");
  }
  s = std::make_shared<FixtureState>(); d = dispatcher(s);
  capabilities(s, {}); expect(*d, AppraisalOutcome::unsupported);
  AppraisalDispatcher missing(mapping(), {{"area", "123.5"}}, "request-1", nullptr);
  expect(missing, AppraisalOutcome::unavailable);
  for (int fault = 0; fault < 4; ++fault) {
    s = std::make_shared<FixtureState>(); auto bad = mapping();
    auto values = std::map<std::string, std::string>{{"area", "123.5"}};
    if (fault == 0) bad.timeout_ms = 0;
    if (fault == 1) bad.timeout_ms = 120001;
    if (fault == 2) values.clear();
    bool rejected{};
    try {
      AppraisalDispatcher invalid(bad, values, fault == 3 ? "" : "request-1",
          std::make_unique<FixtureBoundary>(s));
    } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected && s->negotiations == 0, "invalid local input started worker");
  }
}
}
int main() {
  try { run(); std::cout << "appraisal dispatcher synthetic checks passed\n"; return 0; }
  catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

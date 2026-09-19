#pragma once

#include "sketch/field_adapter_contract.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>

namespace sketch {

enum class AppraisalOutcome {
  success, offline, unavailable, unsupported, ambiguous, timeout, cancelled,
  worker_failure, uncertain_response
};

enum class AppraisalReplyKind {
  capabilities, prepared, offline, unavailable, unsupported, ambiguous,
  worker_failure, uncertain_response
};

struct AppraisalWorkerReply {
  AppraisalReplyKind kind{AppraisalReplyKind::uncertain_response};
  std::string request_id;
  std::vector<AppraisalMappingDescriptor> capabilities;
  std::string prepared_payload;
};

// Injectable transport seam, NOT a native process implementation. All methods,
// including destruction, must be nonblocking. Workers may prepare data only:
// they must have no destination-write authority. abandon() revokes this session
// and discards its replies; no automatic retry or remote commit is permitted.
// Implementations must copy request arguments before returning if used later.
class AppraisalCallBoundary {
public:
  virtual ~AppraisalCallBoundary() = default;
  virtual void negotiate(const std::string &request_id) = 0;
  virtual void prepare(const std::string &request_id, const std::string &payload) = 0;
  virtual std::optional<AppraisalWorkerReply> poll() = 0;
  virtual void abandon() noexcept = 0;
};

struct AppraisalDispatchResult {
  AppraisalOutcome outcome{AppraisalOutcome::uncertain_response};
  // Present only for a timely, correlated, exact preparation acknowledgement.
  // This is data for review, never a destination commit or commit authorization.
  std::optional<std::string> prepared_payload;
};

// Single-owner polling state machine, independent of drawing/document engines.
// The caller must service poll(); this class does not schedule threads or prove
// process isolation. It rejects all replies at/after the absolute deadline.
// Now must be a nonthrowing monotonic clock. Cancellation is sampled at boundary
// checkpoints; terminal results never change. Use a unique request_id per session.
class AppraisalDispatcher final {
public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  AppraisalDispatcher(AppraisalMappingDescriptor descriptor,
      std::map<std::string, std::string> source_values, std::string request_id,
      std::unique_ptr<AppraisalCallBoundary> boundary,
      std::stop_token cancellation = {}, Now now = [] { return Clock::now(); });
  ~AppraisalDispatcher();
  AppraisalDispatcher(const AppraisalDispatcher &) = delete;
  AppraisalDispatcher &operator=(const AppraisalDispatcher &) = delete;
  [[nodiscard]] const std::optional<AppraisalDispatchResult> &poll();
  [[nodiscard]] const AppraisalMappingDescriptor &descriptor() const noexcept { return descriptor_; }
  [[nodiscard]] const std::map<std::string, std::string> &source_values() const noexcept { return source_values_; }

private:
  bool stop_if_needed();
  void finish(AppraisalOutcome outcome);
  AppraisalMappingDescriptor descriptor_;
  std::map<std::string, std::string> source_values_;
  std::string request_id_;
  std::unique_ptr<AppraisalCallBoundary> boundary_;
  std::stop_token cancellation_;
  Now now_;
  Clock::time_point deadline_;
  std::string payload_;
  bool preparing_{};
  std::optional<AppraisalDispatchResult> result_;
};

} // namespace sketch

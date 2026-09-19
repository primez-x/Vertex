#include "sketch/appraisal_dispatcher.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
bool valid_request_id(const std::string &id) {
  return !id.empty() && id.size() <= 128 &&
      std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
      });
}
bool matches(const AppraisalMappingDescriptor &requested, AppraisalMappingDescriptor advertised) {
  // Validate all peer metadata, including unsupported protocol advertisements,
  // without treating a newer version as compatible with our local protocol.
  const auto version = advertised.protocol_version;
  advertised.protocol_version = {1, 0};
  validate_appraisal_mapping(advertised);
  if (!appraisal_contract_compatible(requested, version, advertised.application,
      advertised.application_version, advertised.exchange_format) ||
      requested.adapter_id != advertised.adapter_id) return false;
  // Provenance and timeout describe the caller's local policy, not capabilities.
  advertised.provenance = requested.provenance;
  advertised.timeout_ms = requested.timeout_ms;
  return appraisal_mapping_json(requested) == appraisal_mapping_json(advertised);
}
}
AppraisalDispatcher::AppraisalDispatcher(AppraisalMappingDescriptor descriptor,
    std::map<std::string, std::string> values, std::string request_id,
    std::unique_ptr<AppraisalCallBoundary> boundary, std::stop_token cancellation, Now now)
    : descriptor_(std::move(descriptor)), source_values_(std::move(values)),
      request_id_(std::move(request_id)), boundary_(std::move(boundary)),
      cancellation_(cancellation), now_(std::move(now)) {
  if (!now_ || !valid_request_id(request_id_)) throw std::invalid_argument("invalid appraisal dispatch request");
  const auto started = now_();
  payload_ = appraisal_payload_json(descriptor_, source_values_);
  const auto duration = std::chrono::milliseconds(descriptor_.timeout_ms);
  if (started > Clock::time_point::max() - duration)
    throw std::invalid_argument("appraisal deadline overflow");
  deadline_ = started + duration;
  if (stop_if_needed()) return;
  if (!boundary_) { finish(AppraisalOutcome::unavailable); return; }
  try { boundary_->negotiate(request_id_); }
  catch (...) {
    if (!stop_if_needed()) finish(AppraisalOutcome::worker_failure);
    return;
  }
  (void)stop_if_needed();
}
AppraisalDispatcher::~AppraisalDispatcher() {
  if (!result_ && boundary_) boundary_->abandon();
}
void AppraisalDispatcher::finish(AppraisalOutcome outcome) {
  result_ = AppraisalDispatchResult{outcome, outcome == AppraisalOutcome::success
      ? std::optional<std::string>{payload_} : std::nullopt};
  if (boundary_) boundary_->abandon();
}
bool AppraisalDispatcher::stop_if_needed() {
  if (result_) return true;
  // Cancellation wins if both signals are observed at the same checkpoint.
  if (cancellation_.stop_requested()) { finish(AppraisalOutcome::cancelled); return true; }
  if (now_() >= deadline_) { finish(AppraisalOutcome::timeout); return true; }
  return false;
}
const std::optional<AppraisalDispatchResult> &AppraisalDispatcher::poll() {
  if (stop_if_needed()) return result_;
  std::optional<AppraisalWorkerReply> reply;
  try { reply = boundary_->poll(); }
  catch (...) {
    if (!stop_if_needed()) finish(AppraisalOutcome::worker_failure);
    return result_;
  }
  if (stop_if_needed() || !reply) return result_;
  if (reply->request_id != request_id_) {
    finish(AppraisalOutcome::uncertain_response); return result_;
  }
  if (reply->kind == AppraisalReplyKind::capabilities) {
    if (preparing_ || !reply->prepared_payload.empty() || reply->capabilities.size() > 64) {
      finish(AppraisalOutcome::uncertain_response); return result_;
    }
    std::size_t count{};
    try {
      for (const auto &capability : reply->capabilities)
        if (matches(descriptor_, capability)) ++count;
    } catch (const std::invalid_argument &) {
      finish(AppraisalOutcome::uncertain_response); return result_;
    }
    if (stop_if_needed()) return result_;
    if (count != 1) {
      finish(count == 0 ? AppraisalOutcome::unsupported : AppraisalOutcome::ambiguous);
      return result_;
    }
    preparing_ = true;
    try { boundary_->prepare(request_id_, payload_); }
    catch (...) {
      if (!stop_if_needed()) finish(AppraisalOutcome::worker_failure);
      return result_;
    }
    (void)stop_if_needed();
    return result_;
  }
  if (!reply->capabilities.empty()) {
    finish(AppraisalOutcome::uncertain_response); return result_;
  }
  if (reply->kind == AppraisalReplyKind::prepared) {
    const bool certain = preparing_ && reply->prepared_payload == payload_;
    if (!stop_if_needed())
      finish(certain ? AppraisalOutcome::success : AppraisalOutcome::uncertain_response);
    return result_;
  }
  if (!reply->prepared_payload.empty()) {
    finish(AppraisalOutcome::uncertain_response); return result_;
  }
  switch (reply->kind) {
    case AppraisalReplyKind::offline: finish(AppraisalOutcome::offline); break;
    case AppraisalReplyKind::unavailable: finish(AppraisalOutcome::unavailable); break;
    case AppraisalReplyKind::unsupported: finish(AppraisalOutcome::unsupported); break;
    case AppraisalReplyKind::ambiguous: finish(AppraisalOutcome::ambiguous); break;
    case AppraisalReplyKind::worker_failure: finish(AppraisalOutcome::worker_failure); break;
    default: finish(AppraisalOutcome::uncertain_response); break;
  }
  return result_;
}
} // namespace sketch

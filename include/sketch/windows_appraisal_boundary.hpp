#pragma once

#include "sketch/appraisal_dispatcher.hpp"
#include <cstdint>
#include <filesystem>

namespace sketch {

struct WindowsAppraisalOptions {
  // Trusted, locally installed executable. No shell or document-supplied arguments.
  std::filesystem::path executable;
  std::uint32_t max_frame_bytes{1024 * 1024};
};

// Local preparation-only protocol over inherited stdin/stdout, supervised on a
// private thread. Calls never wait for worker I/O or process exit. abandon()
// immediately revokes replies; the supervisor kills the Job Object process tree.
// No destination, credential, file handle, commit verb, or retry is exposed.
// This is NOT an OS write/network sandbox: only trusted workers may be launched.
class WindowsAppraisalBoundary final : public AppraisalCallBoundary {
public:
  explicit WindowsAppraisalBoundary(WindowsAppraisalOptions options);
  ~WindowsAppraisalBoundary() override;
  void negotiate(const std::string &request_id) override;
  void prepare(const std::string &request_id, const std::string &payload) override;
  std::optional<AppraisalWorkerReply> poll() override;
  void abandon() noexcept override;
  // Diagnostic lifecycle observations, not worker authority or isolation claims.
  [[nodiscard]] std::uint32_t process_id() const noexcept;
  [[nodiscard]] bool supervisor_finished() const noexcept;
private:
  struct State;
  std::shared_ptr<State> state_;
  std::string request_id_;
  unsigned phase_{};
  void send(std::string message);
  static void supervise(std::shared_ptr<State> state, WindowsAppraisalOptions options) noexcept;
};
} // namespace sketch

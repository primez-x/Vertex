#pragma once

#include "sketch/document.hpp"
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace sketch {

enum class ExtractFaultStage {
  none,
  after_assets,
  before_publish,
  after_publish,
};
struct ExtractOptions {
  ExtractFaultStage fault_stage{ExtractFaultStage::none};
  std::function<void(ExtractFaultStage, const std::filesystem::path &)>
      stage_observer;
};

class ExtractionCleanupError final : public std::runtime_error {
public:
  ExtractionCleanupError(std::string message,
                         std::filesystem::path residual_path)
      : std::runtime_error(std::move(message)),
        residual_path_(std::move(residual_path)) {}
  [[nodiscard]] const std::filesystem::path &residual_path() const noexcept {
    return residual_path_;
  }

private:
  std::filesystem::path residual_path_;
};

// The destination must be new. Failed extraction checks cleanup of its private
// staging directory and reports its exact path if cleanup is prevented. If
// validation after publication cannot roll the exact root back, the reported
// residual is the contaminated destination. It never replaces an existing
// destination. Asset paths in the
// interchange JSON always use forward slashes, independently of Windows paths.
// The root directory is retained by handle through publication. Descendant
// guards close immediately before the Windows directory rename, then expected
// descendants are reopened relative to the retained root and revalidated.
// Callers must not concurrently modify staging descendants in that interval.
// The after_publish observer is a narrow fault-test seam before that
// revalidation. See docs/project-extraction.md for the identity and concurrency
// boundary.
void extract_project(const DocumentSnapshot &snapshot,
                     const std::filesystem::path &destination,
                     const ExtractOptions &options = {});

} // namespace sketch

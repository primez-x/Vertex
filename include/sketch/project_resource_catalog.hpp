#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Resources carried by a portable project package are application inputs, not
// files discovered from PATH or a hosted catalog.  The catalog keeps their
// package-relative identity and verifies their bytes before exposing them.
enum class ProjectResourceKind { template_resource, profile, documentation };

[[nodiscard]] const char* project_resource_kind_name(ProjectResourceKind kind) noexcept;
[[nodiscard]] ProjectResourceKind project_resource_kind_from_name(std::string_view name);

struct ProjectResource {
    ProjectResourceKind kind{ProjectResourceKind::documentation};
    std::string name;
    std::filesystem::path relative_path;
    std::string sha256;
    std::uint64_t size{};
    bool operator==(const ProjectResource&) const = default;
};

// A verified, local resource catalog for one project package. Registration is
// copy-free: package bytes stay in place, while every lookup reopens the
// already-validated package-relative file. The catalog never writes to the
// package and never attempts a network lookup.
class ProjectResourceCatalog final {
public:
    [[nodiscard]] static ProjectResourceCatalog register_package(
        const std::filesystem::path& package_root);

    [[nodiscard]] const std::filesystem::path& package_root() const noexcept {
        return package_root_;
    }
    [[nodiscard]] const std::vector<ProjectResource>& resources() const noexcept {
        return resources_;
    }
    [[nodiscard]] std::vector<ProjectResource> resources(ProjectResourceKind kind) const;
    [[nodiscard]] std::optional<ProjectResource> find(ProjectResourceKind kind,
                                                        std::string_view name) const;
    [[nodiscard]] std::vector<std::uint8_t> read(ProjectResourceKind kind,
                                                  std::string_view name) const;

private:
    std::filesystem::path package_root_;
    std::vector<ProjectResource> resources_;
};

}  // namespace sketch

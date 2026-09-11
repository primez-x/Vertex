#include "sketch/workspace_accessibility.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename F>
void rejects(F&& function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid accessibility profile accepted");
}
}

int main() {
    try {
        using namespace sketch;
        const auto profile = WorkspaceAccessibilityProfile::production_baseline();
        profile.validate();
        const auto encoded = profile.to_json();
        require(encoded.at("themes").size() == 3, "all themes must be declared");
        require(encoded.at("layouts").size() == 4, "all DPI qualification layouts must be declared");
        require(WorkspaceAccessibilityProfile::from_json(encoded).to_json() == encoded,
                "accessibility profile roundtrip mismatch");
        auto no_contrast = encoded;
        no_contrast["high_contrast"] = false;
        rejects([&] { (void)WorkspaceAccessibilityProfile::from_json(no_contrast); });
        auto bad_scale = encoded;
        bad_scale["layouts"][0]["scale"] = 1.25;
        rejects([&] { (void)WorkspaceAccessibilityProfile::from_json(bad_scale); });
        auto unknown = encoded;
        unknown["future"] = true;
        rejects([&] { (void)WorkspaceAccessibilityProfile::from_json(unknown); });
        std::cout << "workspace accessibility tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "sketch/product_scope.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename F>
void rejects(F&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid product scope accepted");
}
}  // namespace

int main() {
    try {
        using namespace sketch;
        const auto scope = ProductScopeProfile::production_scope();
        scope.validate();
        const auto encoded = scope.to_json();
        require(encoded.at("platform") == "Windows 11 x64", "platform scope mismatch");
        require(encoded.at("units").size() == 2 && encoded.at("markets").size() == 2 &&
                    encoded.at("workspaces").size() == 2,
                "production scope must enumerate both choices");
        require(ProductScopeProfile::from_json(encoded).to_json() == encoded, "scope roundtrip mismatch");

        auto missing_unit = encoded;
        missing_unit["units"] = {"imperial"};
        rejects([&] { (void)ProductScopeProfile::from_json(missing_unit); });
        auto network = encoded;
        network["subscription_required"] = true;
        rejects([&] { (void)ProductScopeProfile::from_json(network); });
        auto unknown = encoded;
        unknown["future"] = true;
        rejects([&] { (void)ProductScopeProfile::from_json(unknown); });
        std::cout << "product scope tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

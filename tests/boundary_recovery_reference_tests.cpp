#include "sketch/boundary_authoring_recovery.hpp"
#include "support/boundary_recovery_oracle.hpp"
#include "support/noninteractive_errors.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        if (argc != 2) throw std::runtime_error("reference fixture path is required");
        std::ifstream stream(std::filesystem::path(argv[1]), std::ios::binary);
        if (!stream) throw std::runtime_error("cannot read reference fixture");
        const auto fixtures = nlohmann::json::parse(stream);
        if (!fixtures.is_array() || fixtures.size() < 10) {
            throw std::runtime_error("reference fixture corpus is incomplete");
        }
        std::size_t positions = 0;
        for (const auto& expected : fixtures) {
            const auto decoded = sketch::decode_boundary_authoring_recovery(expected.at("checkpoint"));
            if (!decoded.supported()) throw std::runtime_error("old checkpoint became unsupported");
            const auto restored = sketch::BoundaryAuthoringSession::from_recovery_checkpoint(*decoded.checkpoint);
            const auto actual = sketch::testing::recovery_oracle::fixture(
                restored, expected.at("label").get<std::string>());
            if (actual != expected) {
                std::cerr << "Reference mismatch: " << expected.at("label") << '\n'
                          << nlohmann::json::diff(expected, actual).dump(2) << '\n';
                return 1;
            }
            positions += expected.at("positions").size();
        }
        std::cout << fixtures.size() << " frozen fixtures, " << positions
                  << " history positions and their branches remain exact\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_recovery_reference_tests: " << error.what() << '\n';
        return 1;
    }
}

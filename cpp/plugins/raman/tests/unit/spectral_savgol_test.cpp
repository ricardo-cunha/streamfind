#include "spectral_savgol.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::tests {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, const std::string &message) {
    require(std::abs(actual - expected) <= 1e-10, message);
}

}  // namespace streamfind::raman::tests

int main() {
    using namespace streamfind::raman::tests;
    try {
        std::vector<double> coordinates;
        std::vector<double> quadratic;
        for (int index = 0; index < 9; ++index) {
            const auto coordinate = static_cast<double>(index - 4);
            coordinates.push_back(coordinate * 2.0 + 100.0);
            quadratic.push_back(coordinate * coordinate + 2.0 * coordinate + 3.0);
        }
        const auto smoothed = streamfind::raman::operations::savitzky_golay_smooth(
            quadratic, coordinates, 5, 2);
        require(smoothed.size() == quadratic.size(), "Savitzky-Golay changed spectrum length");
        for (std::size_t index = 0; index < smoothed.size(); ++index)
            require_near(smoothed[index], quadratic[index],
                         "Savitzky-Golay did not preserve a quadratic at an edge or center");

        const std::vector<double> impulse_coordinates = {0, 1, 2, 3, 4};
        const std::vector<double> impulse = {0, 0, 1, 0, 0};
        const auto filtered = streamfind::raman::operations::savitzky_golay_smooth(
            impulse, impulse_coordinates, 5, 2);
        require_near(filtered[2], 17.0 / 35.0, "Savitzky-Golay central smoothing coefficient");

        bool rejected_irregular_axis = false;
        try {
            streamfind::raman::operations::savitzky_golay_smooth(
                impulse, {0, 1, 2.2, 3, 4}, 5, 2);
        } catch (const std::invalid_argument &) {
            rejected_irregular_axis = true;
        }
        require(rejected_irregular_axis, "Savitzky-Golay accepted an irregular axis");
        std::cout << "Raman Savitzky-Golay unit tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

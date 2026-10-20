#pragma once

#include <cstddef>
#include <vector>

namespace streamfind::raman::operations {

std::vector<double> savitzky_golay_smooth(const std::vector<double> &intensities,
                                          const std::vector<double> &coordinates,
                                          std::size_t window_length,
                                          std::size_t polynomial_order);

}  // namespace streamfind::raman::operations

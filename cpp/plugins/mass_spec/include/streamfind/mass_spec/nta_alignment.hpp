// nta_alignment.h
// Feature alignment and grouping functions for PROJECT_NON_TARGET_ANALYSIS

#ifndef NTA_ALIGNMENT_H
#define NTA_ALIGNMENT_H

#include <vector>
#include <string>
#include <map>
#include <cstddef>

namespace nta { namespace api { struct NTA_FEATURE_ROW; struct NTA_FEATURES; class PROJECT_NON_TARGET_ANALYSIS; } using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS; }

namespace nta {
namespace alignment {

// Struct to hold internal standard information
struct InternalStandard {
  std::string analysis;
  std::string name;
  float exp_rt;
  float avg_rt;
  float rt_shift;
};

// Struct to hold feature information for alignment
struct AlignmentFeature {
  std::string analysis;
  std::string feature;
  float rt;
  float mass;
  float intensity;
  int polarity;
  float rt_corrected;
  int group_id;
  std::string feature_group;
};

// Struct to hold anchor points for OBI-Warp alignment
struct AnchorPoint {
  float ref_rt;
  float target_rt;
  float rt_shift;
  float weight;
};

// Calculate RT shifts using internal standards with linear interpolation
void calculate_rt_shifts_istd(
  std::vector<AlignmentFeature> &features,
  const std::vector<InternalStandard> &internal_standards,
  float bin_size
);

// Calculate RT shifts using OBI-Warp alignment
void calculate_rt_shifts_obiwarp(
  std::vector<AlignmentFeature> &features,
  float ppm_threshold,
  float rt_deviation,
  float bin_size
);

// Group features based on mass and corrected RT using tolerance-based clustering
void group_features(
  std::vector<AlignmentFeature> &features,
  float ppm_threshold,
  float rt_deviation,
  int min_samples
);

// Helper function to interpolate RT shift at a given RT
float interpolate_rt_shift(
  float feature_rt,
  const std::vector<float> &anchor_rts,
  const std::vector<float> &anchor_shifts
);

// Forward declaration for PROJECT_NON_TARGET_ANALYSIS (already declared above)

// Implementation function for PROJECT_NON_TARGET_ANALYSIS::group_features
void group_features_impl(
  nta::PROJECT_NON_TARGET_ANALYSIS &nta_data,
  const std::string &method,
  float rt_deviation,
  float ppm_threshold,
  int min_samples,
  float bin_size = 5.0f,
  bool debug = false,
  float debug_rt = 0.0f
);

} // namespace alignment
} // namespace nta

#endif // NTA_ALIGNMENT_H

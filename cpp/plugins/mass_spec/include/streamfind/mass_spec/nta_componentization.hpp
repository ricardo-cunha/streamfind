#ifndef NTA_COMPONENTIZATION_H
#define NTA_COMPONENTIZATION_H

#include <vector>
#include <string>

namespace nta {
  struct FEATURE;    // Forward declaration
  struct FEATURES;   // Forward declaration
  namespace api { class PROJECT_NON_TARGET_ANALYSIS; }
  using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS;
}

namespace nta
{
  namespace componentization
  {
    // Helper function to decode base64-encoded EIC data
    std::vector<float> decode_eic_base64(const std::string &base64_str);

    // Helper function to calculate Pearson correlation between two aligned EIC vectors
    float calculate_pearson_correlation(
      const std::vector<float> &x,
      const std::vector<float> &y
    );

    // Helper function to align two EICs by their RT values (not shifted by apex)
    // This preserves temporal information so time-shifted peaks show poor correlation
    std::pair<std::vector<float>, std::vector<float>> align_eics_by_rt(
      const std::vector<float> &rt1, const std::vector<float> &int1,
      const std::vector<float> &rt2, const std::vector<float> &int2
    );

    // Main implementation function
    void create_components_impl(
      nta::PROJECT_NON_TARGET_ANALYSIS &nta_data,
        const std::vector<float> &rtWindow,
        float minCorrelation = 0.8f,
        float debugRT = 0.0f,
        const std::string &debugAnalysis = "");

  } // namespace componentization
} // namespace nta

#endif

#ifndef NTA_COMPONENTIZATION_H
#include "utils/nta.hpp"
#include "streamfind/sdk/debug_session.hpp"

#define NTA_COMPONENTIZATION_H

#include <vector>
#include <string>

namespace streamfind::mass_spec::nta {
  struct FEATURE;
  struct FEATURES;
  class NtaProjectData;
}

namespace streamfind::mass_spec::nta
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
      ::streamfind::mass_spec::nta::NtaProjectData &nta_data,
        const std::vector<float> &rtWindow,
        float minCorrelation = 0.8f,
        float debugRT = 0.0f,
        const std::string &debugAnalysis = "",
        sdk::DebugSession *debug = nullptr);

  } // namespace componentization
} // namespace streamfind::mass_spec::nta

#endif

namespace streamfind::mass_spec::nta::create_components { STREAMFIND_DOMAIN_API nlohmann::json run(sdk::PluginProjectAccess &, const nlohmann::json &); }

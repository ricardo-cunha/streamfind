// nta_suspect_screening
// Suspect screening for NtaProjectData

#ifndef NTA_SUSPECT_SCREENING_H
#include "utils/nta.hpp"

#define NTA_SUSPECT_SCREENING_H

#include <string>
#include <vector>

namespace streamfind::mass_spec::nta
{
  class NtaProjectData;

  namespace suspect_screening
  {
    using SuspectQuery = ::streamfind::mass_spec::nta::SuspectQuery;

    struct IsotopeMatch
    {
      int theoretical_peaks = 0;
      int matched_peaks = 0;
      double similarity = 0.0;
      bool evaluated = false;
      bool matched = true;
    };

    IsotopeMatch matches_isotope_pattern(
      const SuspectQuery &suspect,
      const ::streamfind::mass_spec::nta::api::NTA_FEATURES &features,
      size_t feature_index,
      double ppm);

    void suspect_screening_impl(
      NtaProjectData &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        double isotopePpm,
        bool filtered);

    void find_internal_standards_impl(
      NtaProjectData &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        bool filtered);
  } // namespace suspect_screening
} // namespace streamfind::mass_spec::nta

#endif // NTA_SUSPECT_SCREENING_H

namespace streamfind::mass_spec::nta::suspect_screening { STREAMFIND_DOMAIN_API nlohmann::json run(sdk::PluginProjectAccess &, const nlohmann::json &); }

namespace streamfind::mass_spec::nta::find_internal_standards { STREAMFIND_DOMAIN_API nlohmann::json run(sdk::PluginProjectAccess &, const nlohmann::json &); }

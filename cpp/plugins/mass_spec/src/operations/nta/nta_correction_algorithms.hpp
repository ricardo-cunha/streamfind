#ifndef NTA_CORRECTION_ALGORITHMS_H
#include "utils/nta.hpp"

#define NTA_CORRECTION_ALGORITHMS_H

#include <string>
#include <vector>

namespace streamfind::mass_spec::nta
{
  class NtaProjectData;

  namespace correction_algorithms
  {
    struct TIC_MATRIX_SUPPRESSION_ROW
    {
      std::string analysis;
      std::string replicate;
      int polarity = 0;
      int level = 1;
      double rt = 0.0;
      double intensity = 0.0;
      double mp = 0.0;
    };

    struct ISTD_MATRIX_SUPPRESSION_ROW
    {
      std::string analysis;
      std::string replicate;
      std::string name;
      double rt = 0.0;
      double intensity = 0.0;
      double matrix_effect = 0.0;
      double mp = 0.0;
      double tichri = 0.0;
    };

    std::vector<TIC_MATRIX_SUPPRESSION_ROW> get_matrix_suppression_impl(
      const NtaProjectData &nta_data,
      const std::vector<std::string> &analyses,
      float rtWindow,
      const std::string &refBlankReplicate = "");

    void correct_matrix_suppression_impl(
      NtaProjectData &nta_data,
      float mpRtWindow,
      const std::string &refBlankReplicate = "");
  } // namespace correction_algorithms
} // namespace streamfind::mass_spec::nta

#endif // NTA_CORRECTION_ALGORITHMS_H

namespace streamfind::mass_spec::nta::correct_matrix_suppression { STREAMFIND_DOMAIN_API nlohmann::json run(sdk::PluginProjectAccess &, const nlohmann::json &); }

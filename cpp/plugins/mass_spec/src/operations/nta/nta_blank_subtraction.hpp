// nta_blank_subtraction.h
// Feature blank subtraction for NtaProjectData

#ifndef NTA_BLANK_SUBTRACTION_H
#include "utils/nta.hpp"

#define NTA_BLANK_SUBTRACTION_H

#include <vector>
#include <string>

namespace streamfind::mass_spec::nta
{
  class NtaProjectData;

  namespace blank_subtraction
  {
    void subtract_blank_impl(
      NtaProjectData &nta_data,
        float blankThreshold,
        float rtExpand,
        float mzExpand,
        float minTracesIntensity = 0.0f);
  } // namespace blank_subtraction
} // namespace streamfind::mass_spec::nta

#endif // NTA_BLANK_SUBTRACTION_H

namespace streamfind::mass_spec::nta::subtract_blank { STREAMFIND_DOMAIN_API nlohmann::json run(sdk::PluginProjectAccess &, const nlohmann::json &); }

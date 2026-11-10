// nta_blank_subtraction.h
// Feature blank subtraction for NtaProjectData

#ifndef NTA_BLANK_SUBTRACTION_H
#include "utils/nta.hpp"

#define NTA_BLANK_SUBTRACTION_H

#include <cstddef>
#include <vector>
#include <string>
#include <unordered_map>

namespace streamfind::mass_spec::nta
{
  class NtaProjectData;

  namespace blank_subtraction
  {
    inline std::unordered_map<std::string, std::vector<size_t>> index_analyses_by_replicate(
        const std::vector<std::string> &replicate_names)
    {
      std::unordered_map<std::string, std::vector<std::size_t>> indices;
      for (std::size_t index = 0; index < replicate_names.size(); ++index)
        indices[replicate_names[index]].push_back(index);
      return indices;
    }

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

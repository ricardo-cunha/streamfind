// nta_blank_subtraction.h
// Feature blank subtraction for PROJECT_NON_TARGET_ANALYSIS

#ifndef NTA_BLANK_SUBTRACTION_H
#define NTA_BLANK_SUBTRACTION_H

#include <vector>
#include <string>

namespace nta
{
  namespace api { class PROJECT_NON_TARGET_ANALYSIS; }
  using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS;

  namespace blank_subtraction
  {
    void subtract_blank_impl(
      PROJECT_NON_TARGET_ANALYSIS &nta_data,
        float blankThreshold,
        float rtExpand,
        float mzExpand,
        float minTracesIntensity = 0.0f);
  } // namespace blank_subtraction
} // namespace nta

#endif // NTA_BLANK_SUBTRACTION_H

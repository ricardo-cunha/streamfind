#ifndef NTA_CORRECTION_ALGORITHMS_H
#define NTA_CORRECTION_ALGORITHMS_H

#include <string>
#include <vector>

namespace nta
{
  namespace api
  {
    class PROJECT_NON_TARGET_ANALYSIS;
  }
  using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS;

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
      const PROJECT_NON_TARGET_ANALYSIS &nta_data,
      const std::vector<std::string> &analyses,
      float rtWindow,
      const std::string &refBlankReplicate = "");

    void correct_matrix_suppression_impl(
      PROJECT_NON_TARGET_ANALYSIS &nta_data,
      float mpRtWindow,
      const std::string &refBlankReplicate = "");
  } // namespace correction_algorithms
} // namespace nta

#endif // NTA_CORRECTION_ALGORITHMS_H

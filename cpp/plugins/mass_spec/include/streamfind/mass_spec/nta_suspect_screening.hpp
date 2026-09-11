// nta_suspect_screening
// Suspect screening for PROJECT_NON_TARGET_ANALYSIS

#ifndef NTA_SUSPECT_SCREENING_H
#define NTA_SUSPECT_SCREENING_H

#include <string>
#include <vector>

namespace nta
{
  namespace api { struct NTA_SUSPECTS; class PROJECT_NON_TARGET_ANALYSIS; }
  using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS;

  namespace suspect_screening
  {
    struct SuspectQuery
    {
      std::string name;
      bool has_mass = false;
      double mass = 0.0;
      double rt = 0.0;
      std::string formula;
      std::string SMILES;
      std::string InChI;
      std::string InChIKey;
      double score = 0.0;
      bool has_xLogP = false;
      double xLogP = 0.0;
      std::string database_id;
      std::vector<double> fragments_mz_pos;
      std::vector<double> fragments_intensity_pos;
      std::vector<double> fragments_mz_neg;
      std::vector<double> fragments_intensity_neg;
    };

    void suspect_screening_impl(
      PROJECT_NON_TARGET_ANALYSIS &nta_data,
        const std::vector<std::string> &analyses,
        const std::vector<SuspectQuery> &suspects,
        double ppm,
        double sec,
        double ppmMS2,
        double mzrMS2,
        double minCosineSimilarity,
        int minSharedFragments,
        bool filtered);

    void find_internal_standards_impl(
      PROJECT_NON_TARGET_ANALYSIS &nta_data,
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
} // namespace nta

#endif // NTA_SUSPECT_SCREENING_H

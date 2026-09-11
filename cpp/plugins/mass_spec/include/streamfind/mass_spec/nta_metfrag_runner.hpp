// nta_metfrag_runner.hpp — MetFragCL subprocess runner for NTS data
//
// Invokes MetFragCL (JAR or native executable) per feature, parses
// the CSV output, computes cosine similarity for explained peaks,
// and populates nta::PROJECT_NON_TARGET_ANALYSIS::suspects.
//
// Ported operation-faithfully from bindings/r/src/core/nta/nta_metfrag_runner.cpp.

#ifndef STREAMFIND_NTA_METFRAG_RUNNER_HPP
#define STREAMFIND_NTA_METFRAG_RUNNER_HPP

#include <string>
#include <utility>
#include <vector>

namespace nta
{
  namespace api { class PROJECT_NON_TARGET_ANALYSIS; }
  using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS;

  namespace metfrag_runner
  {
    struct MetFragParams
    {
      std::string metfrag_path;          ///< Full path to MetFragCL JAR or native executable.
      std::string database_type;         ///< MetFrag database type: KEGG, PubChem, ExtendedPubChem, ChemSpiderRest, LocalSDF, LocalPSV, or LocalCSV.
      std::string database_path;         ///< Path to local database file (LocalCSV / LocalPSV / LocalSDF).
      double      ppm            = 5.0;  ///< MS1 precursor mass tolerance (ppm).
      double      sec            = 10.0; ///< RT tolerance for post-filtering (seconds).
      double      ppmMS2         = 10.0; ///< MS2 fragment tolerance (ppm).
      double      mzrMS2         = 0.008;///< MS2 minimum absolute m/z tolerance.
      int         top_n          = 1;    ///< Max candidates kept per feature (top-ranked by score).
      std::vector<std::string> score_types = {"FragmenterScore"}; ///< MetFrag score types.
      std::vector<double> score_weights = {1.0}; ///< MetFrag score weights aligned with score_types.
      std::vector<std::string> pre_processing_candidate_filter = {"UnconnectedCompoundFilter", "IsotopeFilter"}; ///< Candidate pre-filters.
      std::vector<std::string> post_processing_candidate_filter = {"InChIKeyFilter"}; ///< Candidate post-filters.
      std::vector<std::string> candidate_writer = {"CSV", "FragmentSmilesPSV"}; ///< Output writers (CSV for standard data, FragmentSmilesPSV for fragment SMILES).
      int         maximum_tree_depth = 2; ///< Maximum fragmentation tree depth.
      int         number_threads = 1; ///< Number of threads requested from MetFrag.
      bool        use_smiles = true; ///< Use SMILES rather than InChI for fragmentation.
      bool        filtered       = false;///< Include filtered features when true.
      std::string java_path      = "java";///< Path to Java executable (used in JAR mode only).
      std::string run_dir;               ///< Directory for temp files and logs; created if absent.
      std::vector<std::pair<std::string, std::string>> extra_params; ///< Additional MetFrag parameters (override-last).
    };

    std::vector<std::string> supported_database_types();

    std::string canonicalize_database_type(const std::string &database_type);

    MetFragParams canonicalize_and_validate_params(const MetFragParams &params);

    std::string resolve_run_dir(const MetFragParams &params);

    /**
     * Run MetFragCL screening for all (or selected) analyses in nta_data.
     * Results are written to nta_data.suspects[i] for each analysis index i.
     *
     * @param nta_data     NTS data with loaded features.
     * @param analyses     If non-empty, only these analysis names are processed.
     * @param params       MetFrag runner configuration.
     */
    void metfrag_screening_impl(
      PROJECT_NON_TARGET_ANALYSIS &nta_data,
        const std::vector<std::string> &analyses,
        const MetFragParams &params);

  } // namespace metfrag_runner
} // namespace nta

#endif // STREAMFIND_NTA_METFRAG_RUNNER_HPP
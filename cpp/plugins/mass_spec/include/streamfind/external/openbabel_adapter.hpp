#ifndef streamfind_OPENBABEL_ADAPTER_H
#define streamfind_OPENBABEL_ADAPTER_H

#include <string>
#include <vector>

namespace sf::obabel
{
  struct NormalizedStructure
  {
    bool ok = false;
    std::string error;
    // These fields are derived from the parsed structure and should be treated
    // as the normalized canonical values for downstream screening.
    std::string canonical_smiles;
    std::string formula;
    std::string inchi;
    std::string inchikey;
    double exact_mass = 0.0;
    double xlogp = 0.0;
    bool has_xlogp = false;
  };

  struct StructureSvg
  {
    bool ok = false;
    std::string error;
    std::string svg;
  };

  bool openbabel_available();

  NormalizedStructure normalize_structure(
      const std::string &smiles,
      const std::string &inchi);

  NormalizedStructure normalize_structure_from_mol_file(
      const std::string &file_path);

  struct FormulaMatch
  {
    std::string formula;
    double exact_mass = 0.0;
    double error_ppm = 0.0;
  };

  std::vector<FormulaMatch> formula_from_mass(
      double monoisotopic_mass,
      double tolerance_ppm = 5.0,
      const std::string &elements = "");

  StructureSvg render_structure_svg(
      const std::string &smiles,
      const std::string &inchi,
      int width_px,
      int height_px,
      const std::string &bond_color = "");

  std::string debug_runtime();

} // namespace sf::obabel

#endif // streamfind_OPENBABEL_ADAPTER_H

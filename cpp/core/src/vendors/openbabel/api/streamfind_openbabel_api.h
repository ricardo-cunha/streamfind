#ifndef streamfind_OPENBABEL_API_H
#define streamfind_OPENBABEL_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(streamfind_OPENBABEL_BUILD_DLL)
#define streamfind_OPENBABEL_API __declspec(dllexport)
#else
#define streamfind_OPENBABEL_API
#endif

enum
{
  streamfind_OB_SMILES_CAPACITY = 4096,
  streamfind_OB_FORMULA_CAPACITY = 256,
  streamfind_OB_INCHI_CAPACITY = 8192,
  streamfind_OB_INCHIKEY_CAPACITY = 128,
  streamfind_OB_ERROR_CAPACITY = 2048,
  streamfind_OB_COLOR_CAPACITY = 64,
  streamfind_OB_SVG_CAPACITY = 262144,
  streamfind_OB_DEBUG_CAPACITY = 16384,
  streamfind_OB_FORMULA_MAX_RESULTS = 256,
  streamfind_OB_FORMULA_STR_SIZE = 128
};

typedef struct streamfind_ob_normalized_result
{
  int ok;
  int has_xlogp;
  double exact_mass;
  double xlogp;
  char canonical_smiles[streamfind_OB_SMILES_CAPACITY];
  char formula[streamfind_OB_FORMULA_CAPACITY];
  char inchi[streamfind_OB_INCHI_CAPACITY];
  char inchikey[streamfind_OB_INCHIKEY_CAPACITY];
  char error[streamfind_OB_ERROR_CAPACITY];
} streamfind_ob_normalized_result;

typedef struct streamfind_ob_svg_result
{
  int ok;
  char svg[streamfind_OB_SVG_CAPACITY];
  char error[streamfind_OB_ERROR_CAPACITY];
} streamfind_ob_svg_result;

typedef struct streamfind_ob_formula_result
{
  int count;
  char formulas[streamfind_OB_FORMULA_MAX_RESULTS][streamfind_OB_FORMULA_STR_SIZE];
  double masses[streamfind_OB_FORMULA_MAX_RESULTS];
  double errors[streamfind_OB_FORMULA_MAX_RESULTS];
  char error[streamfind_OB_ERROR_CAPACITY];
} streamfind_ob_formula_result;

streamfind_OPENBABEL_API int sf_ob_openbabel_available(void);

streamfind_OPENBABEL_API int sf_ob_normalize_structure(
  const char *smiles,
  const char *inchi,
  streamfind_ob_normalized_result *out);

streamfind_OPENBABEL_API int sf_ob_render_structure_svg(
  const char *smiles,
  const char *inchi,
  int width_px,
  int height_px,
  const char *bond_color,
  streamfind_ob_svg_result *out);

streamfind_OPENBABEL_API int sf_ob_normalize_structure_from_mol_file(
  const char *file_path,
  streamfind_ob_normalized_result *out);

streamfind_OPENBABEL_API int sf_ob_formula_from_mass(
  double monoisotopic_mass,
  double tolerance_ppm,
  const char *elements,
  streamfind_ob_formula_result *out);

streamfind_OPENBABEL_API int sf_ob_debug_runtime(
  char *out,
  size_t capacity);

#ifdef __cplusplus
}
#endif

#endif // streamfind_OPENBABEL_API_H

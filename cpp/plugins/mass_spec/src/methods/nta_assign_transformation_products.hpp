// nta_assign_transformation_products.hpp
// C++ implementation of the AssignTransformationProducts algorithm.
// Accepts a flat suspects list (with feature_group pre-joined) and a
// transformation-products input table; returns an expanded output table
// with per-combination cosine-similarity and RT-plausibility metrics.
//
// Ported operation-faithfully from bindings/r/src/core/nta/nta_assign_transformation_products.cpp.

#ifndef STREAMFIND_NTA_ASSIGN_TRANSFORMATION_PRODUCTS_HPP
#define STREAMFIND_NTA_ASSIGN_TRANSFORMATION_PRODUCTS_HPP

#include <string>
#include <vector>

namespace nta::api
{
  struct NTA_SUSPECT_ROW;
  struct NTA_TRANSFORMATION_PRODUCT_ROW;
  struct NTA_TRANSFORMATION_PRODUCTS;
}

namespace nta::assign_transformation_products
{
  nta::api::NTA_TRANSFORMATION_PRODUCTS assign_transformation_products_impl(
      const std::vector<nta::api::NTA_SUSPECT_ROW> &suspects,
      const std::vector<nta::api::NTA_TRANSFORMATION_PRODUCT_ROW> &transformation_products,
      const std::string &chromatographic_phase,
      double mzrMS2);

} // namespace nta::assign_transformation_products

#endif // STREAMFIND_NTA_ASSIGN_TRANSFORMATION_PRODUCTS_HPP
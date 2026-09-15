#include "operations/operations.hpp"
#include <string>

namespace streamfind::mass_spec::operations
{

  Json get_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_CHROMATOGRAMS");
    std::string query = "SELECT c.analysis, a.replicate, c.index, c.chromatogram_id, c.polarity, c.precursor_mz, c.activation_ce, c.product_mz, c.wavelength_nm, c.rt, c.raw_intensity, c.baseline, c.intensity FROM MASS_SPEC_CHROMATOGRAMS c LEFT JOIN MASS_SPEC_ANALYSES a ON c.analysis = a.analysis WHERE 1=1";
    const auto wanted = parameters.value("analysis_names", Json::array());
    if (!wanted.empty())
    {
      query += " AND c.analysis IN (";
      for (std::size_t i = 0; i < wanted.size(); ++i)
        query += (i ? "," : "") + streamfind::mass_spec::detail::sql(wanted[i].get<std::string>());
      query += ")";
    }
    query += " ORDER BY c.analysis, c.chromatogram_id, c.rt";
    auto rows = access.query(query);
    for (auto &row : rows)
    {
      if (row.at("replicate").is_null())
        row["replicate"] = "";
      if (row.at("index").is_null())
        row["index"] = 0;
      else
        row["index"] = std::stoi(row.at("index").get<std::string>());
      if (row.at("polarity").is_null())
        row["polarity"] = Json(nullptr);
      else
        row["polarity"] = std::stoi(row.at("polarity").get<std::string>());
      for (const auto *name : {"precursor_mz", "activation_ce", "product_mz"})
      {
        if (row.at(name).is_null())
          row[name] = Json(nullptr);
        else
          row[name] = std::stod(row.at(name).get<std::string>());
      }
      if (row.at("wavelength_nm").is_null())
        row["wavelength_nm"] = 0.0;
      else
        row["wavelength_nm"] = std::stod(row.at("wavelength_nm").get<std::string>());
      row["rt"] = std::stod(row.at("rt").get<std::string>());
      row["raw_intensity"] = std::stod(row.at("raw_intensity").get<std::string>());
      row["baseline"] = std::stod(row.at("baseline").get<std::string>());
      row["intensity"] = std::stod(row.at("intensity").get<std::string>());
    }
    return rows;
  }

} // namespace streamfind::mass_spec::operations

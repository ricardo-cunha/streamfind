#include "operations/operations.hpp"
#include <map>
#include <string>

namespace streamfind::mass_spec::operations
{

  Json get_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_CHROMATOGRAMS");
    access.require_table("MASS_SPEC_CHROMATOGRAMS_HEADERS");
    access.require_table("MASS_SPEC_ANALYSES");

    const auto wanted = parameters.value("analysis_names", Json::array());

    // Read headers.
    std::string hdr_query = "SELECT analysis, index, chromatogram_id, "
        "polarity, precursor_mz, activation_ce, product_mz, "
        "signal_type, chromatogram_type, detector, channel, "
        "wavelength_nm, units FROM MASS_SPEC_CHROMATOGRAMS_HEADERS ORDER BY analysis, index";
    auto headers = access.query(hdr_query);

    // Read points.
    std::string pt_query = "SELECT analysis, index, rt, raw_intensity, baseline, intensity "
        "FROM MASS_SPEC_CHROMATOGRAMS ORDER BY analysis, index, rt";
    auto points = access.query(pt_query);

    // Read analysis replicate info.
    std::map<std::string, std::string> replicates;
    for (const auto &row : access.query(
             "SELECT analysis, replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
    {
        const auto name = row.at("analysis").get<std::string>();
        if (!wanted.empty()) {
            bool found = false;
            for (const auto &v : wanted)
                if (v.get<std::string>() == name) { found = true; break; }
            if (!found) continue;
        }
        replicates[name] = row.at("replicate").is_null() ? "" : row.at("replicate").get<std::string>();
    }

    // Build header lookup: (analysis, index) -> header json.
    struct HeaderInfo {
        std::string chromatogram_id;
        int polarity = 0;
        double precursor_mz = 0.0;
        double activation_ce = 0.0;
        double product_mz = 0.0;
        std::string signal_type;
        std::string chromatogram_type;
        std::string detector;
        std::string channel;
        double wavelength_nm = 0.0;
        std::string units;
    };
    std::map<std::pair<std::string, int>, HeaderInfo> hdr_map;
    for (const auto &h : headers) {
        auto key = std::make_pair(h.at("analysis").get<std::string>(), std::stoi(h.at("index").get<std::string>()));
        auto &info = hdr_map[key];
        info.chromatogram_id = h.at("chromatogram_id").get<std::string>();
        info.polarity = h.at("polarity").is_null() ? 0 : std::stoi(h.at("polarity").get<std::string>());
        info.precursor_mz = h.at("precursor_mz").is_null() ? 0.0 : std::stod(h.at("precursor_mz").get<std::string>());
        info.activation_ce = h.at("activation_ce").is_null() ? 0.0 : std::stod(h.at("activation_ce").get<std::string>());
        info.product_mz = h.at("product_mz").is_null() ? 0.0 : std::stod(h.at("product_mz").get<std::string>());
        info.signal_type = h.at("signal_type").get<std::string>();
        info.chromatogram_type = h.at("chromatogram_type").get<std::string>();
        info.detector = h.at("detector").get<std::string>();
        info.channel = h.at("channel").get<std::string>();
        info.wavelength_nm = h.at("wavelength_nm").is_null() ? 0.0 : std::stod(h.at("wavelength_nm").get<std::string>());
        info.units = h.at("units").get<std::string>();
    }

    // Join and build output.
    Json output = Json::array();
    for (const auto &pt : points) {
        const auto analysis = pt.at("analysis").get<std::string>();
        const auto idx = std::stoi(pt.at("index").get<std::string>());
        if (!wanted.empty()) {
            bool found = false;
            for (const auto &v : wanted)
                if (v.get<std::string>() == analysis) { found = true; break; }
            if (!found) continue;
        }
        auto key = std::make_pair(analysis, idx);
        auto it = hdr_map.find(key);
        const auto &rep = replicates.count(analysis) ? replicates.at(analysis) : std::string("");
        Json row;
        row["analysis"] = analysis;
        row["replicate"] = rep;
        row["index"] = idx;
        if (it != hdr_map.end()) {
            const auto &h = it->second;
            row["chromatogram_id"] = h.chromatogram_id;
            row["polarity"] = h.polarity;
            row["precursor_mz"] = h.precursor_mz;
            row["activation_ce"] = h.activation_ce;
            row["product_mz"] = h.product_mz;
            row["signal_type"] = h.signal_type;
            row["chromatogram_type"] = h.chromatogram_type;
            row["detector"] = h.detector;
            row["channel"] = h.channel;
            row["wavelength_nm"] = h.wavelength_nm;
            row["units"] = h.units;
        }
        row["rt"] = std::stod(pt.at("rt").get<std::string>());
        row["raw_intensity"] = std::stod(pt.at("raw_intensity").get<std::string>());
        row["baseline"] = std::stod(pt.at("baseline").get<std::string>());
        row["intensity"] = std::stod(pt.at("intensity").get<std::string>());
        output.push_back(std::move(row));
    }
    return output;
  }

  Json get_chromatogram_peaks(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_CHROMATOGRAM_PEAKS");
    access.require_table("MASS_SPEC_CHROMATOGRAMS_HEADERS");

    // Read headers.
    std::string hdr_query = "SELECT analysis, index, chromatogram_id, "
        "polarity, precursor_mz, activation_ce, product_mz, "
        "signal_type, chromatogram_type, detector, channel, "
        "wavelength_nm, units FROM MASS_SPEC_CHROMATOGRAMS_HEADERS ORDER BY analysis, index";
    auto headers = access.query(hdr_query);

    // Read peaks.
    std::string pk_query = "SELECT analysis, chromatogram_index, peak_id, "
        "rt, rt_start, rt_end, height, raw_height, area, raw_area, baseline_area, "
        "width, fwhm, snr, asymmetry, sharpness, plates, "
        "number_points, integration_algorithm, integration_status, manual_override "
        "FROM MASS_SPEC_CHROMATOGRAM_PEAKS ORDER BY analysis, chromatogram_index, peak_id";
    auto peaks = access.query(pk_query);

    // Build header lookup.
    struct HeaderInfo {
        std::string chromatogram_id;
        int polarity = 0;
        double precursor_mz = 0.0;
        double activation_ce = 0.0;
        double product_mz = 0.0;
        std::string signal_type;
        std::string chromatogram_type;
        std::string detector;
        std::string channel;
        double wavelength_nm = 0.0;
        std::string units;
    };
    std::map<std::pair<std::string, int>, HeaderInfo> hdr_map;
    for (const auto &h : headers) {
        auto key = std::make_pair(h.at("analysis").get<std::string>(), std::stoi(h.at("index").get<std::string>()));
        auto &info = hdr_map[key];
        info.chromatogram_id = h.at("chromatogram_id").get<std::string>();
        info.polarity = h.at("polarity").is_null() ? 0 : std::stoi(h.at("polarity").get<std::string>());
        info.precursor_mz = h.at("precursor_mz").is_null() ? 0.0 : std::stod(h.at("precursor_mz").get<std::string>());
        info.activation_ce = h.at("activation_ce").is_null() ? 0.0 : std::stod(h.at("activation_ce").get<std::string>());
        info.product_mz = h.at("product_mz").is_null() ? 0.0 : std::stod(h.at("product_mz").get<std::string>());
        info.signal_type = h.at("signal_type").get<std::string>();
        info.chromatogram_type = h.at("chromatogram_type").get<std::string>();
        info.detector = h.at("detector").get<std::string>();
        info.channel = h.at("channel").get<std::string>();
        info.wavelength_nm = h.at("wavelength_nm").is_null() ? 0.0 : std::stod(h.at("wavelength_nm").get<std::string>());
        info.units = h.at("units").get<std::string>();
    }

    // Join and build output.
    const auto wanted = parameters.value("analysis_names", Json::array());
    const auto wanted_indices = parameters.value("indices", Json::array());
    Json output = Json::array();
    for (const auto &pk : peaks) {
        const auto analysis = pk.at("analysis").get<std::string>();
        const auto chrom_idx = std::stoi(pk.at("chromatogram_index").get<std::string>());
        if (!wanted.empty()) {
            bool found = false;
            for (const auto &v : wanted)
                if (v.get<std::string>() == analysis) { found = true; break; }
            if (!found) continue;
        }
        if (!wanted_indices.empty()) {
            bool found = false;
            for (const auto &v : wanted_indices)
                if (v.get<int>() == chrom_idx) { found = true; break; }
            if (!found) continue;
        }
        auto key = std::make_pair(analysis, chrom_idx);
        Json row;
        row["analysis"] = analysis;
        row["chromatogram_index"] = chrom_idx;
        row["peak_id"] = std::stoi(pk.at("peak_id").get<std::string>());
        auto it = hdr_map.find(key);
        if (it != hdr_map.end()) {
            const auto &h = it->second;
            row["chromatogram_id"] = h.chromatogram_id;
            row["polarity"] = h.polarity;
            row["precursor_mz"] = h.precursor_mz;
            row["activation_ce"] = h.activation_ce;
            row["product_mz"] = h.product_mz;
            row["signal_type"] = h.signal_type;
            row["chromatogram_type"] = h.chromatogram_type;
            row["detector"] = h.detector;
            row["channel"] = h.channel;
            row["wavelength_nm"] = h.wavelength_nm;
            row["units"] = h.units;
        }
        for (const auto *name : {"rt", "rt_start", "rt_end", "height", "raw_height",
                                 "area", "raw_area", "baseline_area",
                                 "width", "fwhm", "snr", "asymmetry", "sharpness", "plates"})
        {
            if (pk.at(name).is_null())
                row[name] = 0.0;
            else
                row[name] = std::stod(pk.at(name).get<std::string>());
        }
        if (pk.at("number_points").is_null())
            row["number_points"] = 0;
        else
            row["number_points"] = std::stoi(pk.at("number_points").get<std::string>());
        row["integration_algorithm"] = pk.value("integration_algorithm", "");
        row["integration_status"] = pk.value("integration_status", "");
        row["manual_override"] = pk.at("manual_override").is_null() ? false :
            pk.at("manual_override").get<std::string>() == "true";
        output.push_back(std::move(row));
    }
    return output;
  }

} // namespace streamfind::mass_spec::operations

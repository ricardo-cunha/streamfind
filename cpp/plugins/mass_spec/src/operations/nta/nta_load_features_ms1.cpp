#include "operations/nta/nta_load_features_ms1.hpp"
#include "utils/nta.hpp"
#include "readers/reader.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <unordered_map>
namespace streamfind::mass_spec::nta::load_features_ms1
{
using Json = nlohmann::json;
    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const bool filtered = parameters.value("filtered", false);
        const auto rt_window = parameters.value("rt_window", Json::array({-2.0, 2.0}));
        const auto mz_window = parameters.value("mz_window", Json::array({-1.0, 6.0}));
        const float min_traces = parameters.value("min_traces_intensity", 250.0);
        const float mz_clust = parameters.value("mz_clust", 0.005);
        const float presence = parameters.value("presence", 0.8);
        if (min_traces < 0 || mz_clust < 0 || presence < 0 || presence > 1)
            throw Error(ErrorCode::InvalidArgument, "invalid MS1 spectrum loading parameters");
        const float rt_lo = rt_window.size() >= 1 ? rt_window[0].get<float>() : 0.0f;
        const float rt_hi = rt_window.size() >= 2 ? rt_window[1].get<float>() : 0.0f;
        const float mz_lo = mz_window.size() >= 1 ? mz_window[0].get<float>() : 0.0f;
        const float mz_hi = mz_window.size() >= 2 ? mz_window[1].get<float>() : 0.0f;
        auto all_analysis_parameters = parameters;
        all_analysis_parameters.erase("analysis_names");
        auto data = utils::detail::load_analysis_features(access, all_analysis_parameters);
        auto &buffers = data.feature_buffers();
        access.report_progress(0.0, "Preparing MS1 feature targets.");
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            ::mass_spec::spectra::MASS_SPEC_TARGETS targets;
            int counter = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                const auto ft = buffers[i].get_feature(j);
                if (utils::detail::excluded_feature(ft, filtered))
                    continue;
                if (utils::detail::already_had(ft, 1))
                    continue;
                targets.index.push_back(counter++);
                targets.id.push_back(ft.feature);
                targets.level.push_back(1);
                targets.polarity.push_back(ft.polarity);
                targets.precursor.push_back(false);
                targets.mzmin.push_back(static_cast<float>(ft.mzmin) + mz_lo);
                targets.mzmax.push_back(static_cast<float>(ft.mzmax) + mz_hi);
                targets.rtmin.push_back(static_cast<float>(ft.rtmin) + rt_lo);
                targets.rtmax.push_back(static_cast<float>(ft.rtmax) + rt_hi);
                targets.mz.push_back(static_cast<float>(ft.mz));
                targets.mass.push_back(static_cast<float>(ft.mass));
                targets.rt.push_back(static_cast<float>(ft.rt));
                targets.mobility.push_back(0.0f);
                targets.mobilitymin.push_back(0.0f);
                targets.mobilitymax.push_back(0.0f);
            }
            if (targets.id.empty())
                continue;
            if (!std::filesystem::exists(data.file_paths()[i]))
                continue;
            std::cerr << "[load_features_ms1] " << i + 1 << "/" << buffers.size()
                      << " targets=" << targets.id.size() << std::endl;
            ::mass_spec::reader::MASS_SPEC_FILE file(data.file_paths()[i]);
            file.select_analysis(data.analysis_index_at(i));
            auto spectra = file.get_spectra_targets(targets, data.spectra_headers_at(i), min_traces, 0.0f);
            std::cerr << "[load_features_ms1] extracted " << spectra.id.size()
                      << " spectrum points" << std::endl;
            std::unordered_map<std::string, std::vector<size_t>> spectra_by_feature;
            for (size_t k = 0; k < spectra.id.size(); ++k)
                spectra_by_feature[spectra.id[k]].push_back(k);
            std::vector<std::vector<std::optional<std::string>>> updates;
            size_t updated_for_analysis = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                auto ft = buffers[i].get_feature(j);
                if (utils::detail::excluded_feature(ft, filtered))
                    continue;
                if (utils::detail::already_had(ft, 1))
                    continue;
                ::mass_spec::spectra::MASS_SPEC_TARGETS_SPECTRA sub;
                const auto matching = spectra_by_feature.find(ft.feature);
                if (matching == spectra_by_feature.end())
                    continue;
                for (const size_t k : matching->second)
                {
                    sub.mz.push_back(spectra.mz[k]);
                    sub.intensity.push_back(spectra.intensity[k]);
                    if (k < spectra.rt.size())
                        sub.rt.push_back(spectra.rt[k]);
                    if (k < spectra.pre_ce.size())
                        sub.pre_ce.push_back(spectra.pre_ce[k]);
                }
                auto merged = utils::detail::merge_nta_feature_spectra(sub, mz_clust, presence);
                if (merged.mz.empty())
                    continue;
                ft.ms1_size = static_cast<int>(merged.mz.size());
                ft.ms1_mz = utils::detail::encode_float_array(merged.mz);
                ft.ms1_intensity = utils::detail::encode_float_array(merged.intensity);
                buffers[i].set_feature(j, ft);
                updates.push_back({ft.analysis, ft.feature, std::to_string(ft.ms1_size),
                                   ft.ms1_mz, ft.ms1_intensity});
                ++updated_for_analysis;
            }
            (void)updates;
            std::cerr << "[load_features_ms1] updated " << updated_for_analysis
                      << " features" << std::endl;
            access.report_progress(
                static_cast<double>(i + 1) / static_cast<double>(buffers.size()),
                "Loaded MS1 spectra for analysis " + std::to_string(i + 1) + "/" + std::to_string(buffers.size()) + ".");
        }
        access.report_progress(1.0, "MS1 spectra loading complete.");
        utils::detail::emit_features(access, data);
        return Json{{"status", "finished"}, {"info", "MS1 spectra loaded."}};
    }
}

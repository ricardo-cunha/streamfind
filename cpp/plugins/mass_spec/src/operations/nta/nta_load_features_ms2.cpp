#include "operations/nta/nta_load_features_ms2.hpp"
#include "utils/nta.hpp"
#include "readers/reader.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <unordered_map>
namespace streamfind::mass_spec::nta::load_features_ms2
{
using Json = nlohmann::json;
    Json run(::streamfind::sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const bool filtered = parameters.value("filtered", false);
        const float min_traces = parameters.value("min_traces_intensity", 10.0);
        const float isolation_window = parameters.value("isolation_window", 1.3);
        const float mz_clust = parameters.value("mz_clust", 0.005);
        const float presence = parameters.value("presence", 0.8);
        if (min_traces < 0 || isolation_window < 0 || mz_clust < 0 || presence < 0 || presence > 1)
            throw Error(ErrorCode::InvalidArgument, "invalid MS2 spectrum loading parameters");
        auto all_analysis_parameters = parameters;
        all_analysis_parameters.erase("analysis_names");
        auto data = utils::detail::load_analysis_features(access, all_analysis_parameters);
        auto &buffers = data.feature_buffers();
        access.report_progress(0.0, "Preparing MS2 feature targets.");
        const auto operation_started_at = std::chrono::steady_clock::now();
        std::size_t total_targets = 0;
        std::size_t completed_analyses = 0;
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            const auto analysis_started_at = std::chrono::steady_clock::now();
            ::mass_spec::spectra::MASS_SPEC_TARGETS targets;
            int counter = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                const auto ft = buffers[i].get_feature(j);
                if (utils::detail::excluded_feature(ft, filtered))
                    continue;
                if (utils::detail::already_had(ft, 2))
                    continue;
                targets.index.push_back(counter++);
                targets.id.push_back(ft.feature);
                targets.level.push_back(2);
                targets.polarity.push_back(ft.polarity);
                targets.precursor.push_back(true);
                targets.mzmin.push_back(static_cast<float>(ft.mz) - isolation_window / 2.0f);
                targets.mzmax.push_back(static_cast<float>(ft.mz) + isolation_window / 2.0f);
                targets.rtmin.push_back(static_cast<float>(ft.rtmin));
                targets.rtmax.push_back(static_cast<float>(ft.rtmax));
                targets.mz.push_back(static_cast<float>(ft.mz));
                targets.mass.push_back(static_cast<float>(ft.mass));
                targets.rt.push_back(static_cast<float>(ft.rt));
                targets.mobility.push_back(0.0f);
                targets.mobilitymin.push_back(0.0f);
                targets.mobilitymax.push_back(0.0f);
            }
            total_targets += targets.id.size();
            const auto analysis_name = data.analysis_names().at(i);
            access.report_progress(
                buffers.empty() ? 1.0 : static_cast<double>(i) / static_cast<double>(buffers.size()),
                "MS2 analysis " + std::to_string(i + 1) + "/" + std::to_string(buffers.size()) +
                    ": " + analysis_name + ", " + std::to_string(targets.id.size()) +
                    " eligible features; " + std::to_string(total_targets) +
                    " eligible targets across analyses so far.");
            if (targets.id.empty())
            {
                ++completed_analyses;
                access.report_progress(
                    buffers.empty() ? 1.0 : static_cast<double>(completed_analyses) / static_cast<double>(buffers.size()),
                    "MS2 analysis " + std::to_string(i + 1) + "/" + std::to_string(buffers.size()) +
                        ": " + analysis_name + " has no eligible targets; elapsed " +
                        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - operation_started_at).count()) + " s.");
                continue;
            }
            if (!std::filesystem::exists(data.file_paths()[i]))
            {
                ++completed_analyses;
                access.report_progress(
                    buffers.empty() ? 1.0 : static_cast<double>(completed_analyses) / static_cast<double>(buffers.size()),
                    "MS2 analysis " + std::to_string(i + 1) + "/" + std::to_string(buffers.size()) +
                        ": source file is missing for " + analysis_name + ".");
                continue;
            }
            access.report_progress(
                buffers.empty() ? 0.0 : static_cast<double>(i) / static_cast<double>(buffers.size()),
                "Extracting MS2 spectra for " + analysis_name + " (" +
                    std::to_string(targets.id.size()) + " feature targets).");
            ::mass_spec::reader::MASS_SPEC_FILE file(data.file_paths()[i]);
            file.select_analysis(data.analysis_index_at(i));
            const auto analysis_fraction = [&](double within_analysis) {
                return buffers.empty()
                    ? 1.0
                    : (static_cast<double>(i) + within_analysis) / static_cast<double>(buffers.size());
            };
            auto spectra = file.get_spectra_targets(
                targets,
                data.spectra_headers_at(i),
                0.0f,
                min_traces,
                [&](size_t scans_completed, size_t total_scans) {
                    const double within_analysis = total_scans == 0
                        ? 1.0
                        : static_cast<double>(scans_completed) / static_cast<double>(total_scans);
                    access.report_progress(
                        analysis_fraction(within_analysis),
                        "MS2 extraction " + std::to_string(i + 1) + "/" + std::to_string(buffers.size()) +
                            ": " + analysis_name + ", decoded " + std::to_string(scans_completed) + "/" +
                            std::to_string(total_scans) + " candidate scans.");
                });
            std::cerr << "[load_features_ms2] " << i + 1 << "/" << buffers.size()
                      << " targets=" << targets.id.size() << std::endl;
            std::cerr << "[load_features_ms2] extracted " << spectra.id.size()
                      << " spectrum points" << std::endl;
            std::unordered_map<std::string, std::vector<size_t>> spectra_by_feature;
            for (size_t k = 0; k < spectra.id.size(); ++k)
                spectra_by_feature[spectra.id[k]].push_back(k);
            size_t updated_for_analysis = 0;
            for (int j = 0; j < buffers[i].size(); ++j)
            {
                auto ft = buffers[i].get_feature(j);
                if (utils::detail::excluded_feature(ft, filtered))
                    continue;
                if (utils::detail::already_had(ft, 2))
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
                ft.ms2_size = static_cast<int>(merged.mz.size());
                ft.ms2_mz = merged.mz;
                ft.ms2_intensity = merged.intensity;
                buffers[i].set_feature(j, ft);
                ++updated_for_analysis;
            }
            std::cerr << "[load_features_ms2] updated " << updated_for_analysis
                      << " features" << std::endl;
            ++completed_analyses;
            access.report_progress(
                buffers.empty() ? 1.0 : static_cast<double>(completed_analyses) / static_cast<double>(buffers.size()),
                "Loaded MS2 spectra for analysis " + std::to_string(i + 1) + "/" + std::to_string(buffers.size()) +
                    ": " + analysis_name + ", " + std::to_string(targets.id.size()) +
                    " targets, " + std::to_string(spectra.mz.size()) + " spectrum points, " +
                    std::to_string(updated_for_analysis) + " features updated; analysis elapsed " +
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - analysis_started_at).count()) +
                    " s, total elapsed " +
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - operation_started_at).count()) + " s.");
        }
        access.report_progress(1.0, "MS2 spectra loading complete.");
        utils::detail::emit_features(access, data);
        return Json{{"status", "finished"}, {"info", "MS2 spectra loaded."}};
    }
}

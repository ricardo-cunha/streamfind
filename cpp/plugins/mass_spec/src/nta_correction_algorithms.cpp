#include "streamfind/mass_spec/nta_correction_algorithms.hpp"

#include "streamfind/mass_spec/nta.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace nta::correction_algorithms
{
  namespace streamfind::nta_correction_detail
  {
    using Profile = std::vector<TIC_MATRIX_SUPPRESSION_ROW>;

    struct SupportPoint
    {
      std::string replicate;
      std::string name;
      double rt = 0.0;
      double mp = std::numeric_limits<double>::quiet_NaN();
      double tichri = std::numeric_limits<double>::quiet_NaN();
    };

    using BlankLookup = std::vector<std::vector<std::size_t>>;

    bool is_missing_blank(const std::string &value)
    {
      return value.empty() || value == "NA";
    }

    std::string join_key(const std::string &lhs, const std::string &rhs)
    {
      return lhs + "\x1f" + rhs;
    }

    bool contains_analysis(const std::unordered_set<std::string> &selected, const std::string &analysis)
    {
      return selected.empty() || selected.find(analysis) != selected.end();
    }

    BlankLookup resolve_blank_indices(
      const PROJECT_NON_TARGET_ANALYSIS &nta_data,
      const std::string &refBlankReplicate)
    {
      const auto &analysis_names = nta_data.analysis_names();
      const auto &replicate_names = nta_data.replicate_names();
      const auto &blank_names = nta_data.blank_names();

      std::unordered_map<std::string, std::vector<std::size_t>> replicate_to_indices;
      for (std::size_t i = 0; i < analysis_names.size(); ++i)
      {
        replicate_to_indices[replicate_names[i]].push_back(i);
      }

      std::vector<std::string> override_blank_replicates;
      if (!refBlankReplicate.empty())
      {
        for (std::size_t i = 0; i < analysis_names.size(); ++i)
        {
          if (replicate_names[i] == refBlankReplicate && !is_missing_blank(blank_names[i]))
          {
            override_blank_replicates.push_back(blank_names[i]);
          }
        }
        std::sort(override_blank_replicates.begin(), override_blank_replicates.end());
        override_blank_replicates.erase(
          std::unique(override_blank_replicates.begin(), override_blank_replicates.end()),
          override_blank_replicates.end());
      }

      BlankLookup out(analysis_names.size());
      for (std::size_t i = 0; i < analysis_names.size(); ++i)
      {
        std::vector<std::string> blank_replicates;
        if (!override_blank_replicates.empty())
        {
          blank_replicates = override_blank_replicates;
        }
        else if (!is_missing_blank(blank_names[i]))
        {
          blank_replicates.push_back(blank_names[i]);
        }

        std::vector<std::size_t> blank_indices;
        for (const auto &blank_replicate : blank_replicates)
        {
          auto it = replicate_to_indices.find(blank_replicate);
          if (it == replicate_to_indices.end())
          {
            continue;
          }
          blank_indices.insert(blank_indices.end(), it->second.begin(), it->second.end());
        }

        std::sort(blank_indices.begin(), blank_indices.end());
        blank_indices.erase(std::unique(blank_indices.begin(), blank_indices.end()), blank_indices.end());
        out[i] = std::move(blank_indices);
      }

      return out;
    }

    Profile build_tic_profile(const PROJECT_NON_TARGET_ANALYSIS &nta_data, std::size_t analysis_index)
    {
      Profile profile;
      const auto headers = nta_data.spectra_headers_at(analysis_index);
      const auto &analysis_names = nta_data.analysis_names();
      const auto &replicate_names = nta_data.replicate_names();

      for (std::size_t row = 0; row < headers.rt.size(); ++row)
      {
        if (row >= headers.level.size() || headers.level[row] != 1)
        {
          continue;
        }
        if (row >= headers.tic.size())
        {
          continue;
        }
        const float rt = headers.rt[row];
        const float intensity = headers.tic[row];
        if (!std::isfinite(rt) || !std::isfinite(intensity))
        {
          continue;
        }

        TIC_MATRIX_SUPPRESSION_ROW out_row;
        out_row.analysis = analysis_names[analysis_index];
        out_row.replicate = replicate_names[analysis_index];
        out_row.polarity = row < headers.polarity.size() ? headers.polarity[row] : 0;
        out_row.level = 1;
        out_row.rt = rt;
        out_row.intensity = intensity;
        out_row.mp = std::numeric_limits<double>::quiet_NaN();
        profile.push_back(out_row);
      }

      return profile;
    }

    double mean_intensity_in_window(const Profile &profile, double rtmin, double rtmax, bool use_mp = false)
    {
      double sum = 0.0;
      int count = 0;
      for (const auto &row : profile)
      {
        if (row.rt < rtmin || row.rt > rtmax)
        {
          continue;
        }
        const double value = use_mp ? row.mp : row.intensity;
        if (!std::isfinite(value))
        {
          continue;
        }
        sum += value;
        ++count;
      }
      if (count == 0)
      {
        return std::numeric_limits<double>::quiet_NaN();
      }
      return sum / static_cast<double>(count);
    }

    std::unordered_map<std::string, Profile> build_matrix_profiles(
      const PROJECT_NON_TARGET_ANALYSIS &nta_data,
      const std::vector<std::string> &analyses,
      float rtWindow,
      const std::string &refBlankReplicate)
    {
      const auto &analysis_names = nta_data.analysis_names();
      std::unordered_set<std::string> selected_analyses(analyses.begin(), analyses.end());
      const auto blank_lookup = resolve_blank_indices(nta_data, refBlankReplicate);

      std::vector<Profile> raw_profiles(analysis_names.size());
      for (std::size_t i = 0; i < analysis_names.size(); ++i)
      {
        if (!contains_analysis(selected_analyses, analysis_names[i]))
        {
          continue;
        }
        raw_profiles[i] = build_tic_profile(nta_data, i);
      }

      std::unordered_map<std::string, Profile> out;
      for (std::size_t i = 0; i < analysis_names.size(); ++i)
      {
        if (!contains_analysis(selected_analyses, analysis_names[i]))
        {
          continue;
        }

        Profile profile = raw_profiles[i];
        const auto &blank_indices = blank_lookup[i];
        for (auto &row : profile)
        {
          if (blank_indices.empty())
          {
            row.mp = -1.0;
            continue;
          }

          const double rtmin = row.rt - rtWindow;
          const double rtmax = row.rt + rtWindow;
          const double sample_mean = mean_intensity_in_window(profile, rtmin, rtmax, false);
          if (!std::isfinite(sample_mean))
          {
            row.mp = -1.0;
            continue;
          }

          double blank_sum = 0.0;
          int blank_count = 0;
          for (const auto blank_index : blank_indices)
          {
            if (blank_index >= raw_profiles.size())
            {
              continue;
            }
            if (raw_profiles[blank_index].empty())
            {
              raw_profiles[blank_index] = build_tic_profile(nta_data, blank_index);
            }
            const double blank_mean = mean_intensity_in_window(raw_profiles[blank_index], rtmin, rtmax, false);
            if (!std::isfinite(blank_mean) || blank_mean <= 0.0)
            {
              continue;
            }
            blank_sum += blank_mean;
            ++blank_count;
          }

          if (blank_count == 0)
          {
            row.mp = -1.0;
            continue;
          }

          const double blank_mean = blank_sum / static_cast<double>(blank_count);
          row.mp = blank_mean > 0.0 ? (sample_mean / blank_mean) * -1.0 : -1.0;
        }

        out.emplace(analysis_names[i], std::move(profile));
      }

      return out;
    }

    std::vector<SupportPoint> build_internal_standard_support(
      const PROJECT_NON_TARGET_ANALYSIS &nta_data,
      const std::unordered_map<std::string, Profile> &matrix_profiles,
      float rtWindow,
      const std::string &refBlankReplicate)
    {
      const auto &analysis_names = nta_data.analysis_names();
      const auto &replicate_names = nta_data.replicate_names();
      const auto &buffers = nta_data.internal_standard_buffers();
      const auto blank_lookup = resolve_blank_indices(nta_data, refBlankReplicate);

      std::unordered_map<std::string, std::vector<double>> blank_intensities_by_key;
      std::unordered_map<std::string, std::vector<std::tuple<std::size_t, int>>> sample_rows_by_key;

      for (std::size_t analysis_index = 0; analysis_index < buffers.size(); ++analysis_index)
      {
        const auto &buffer = buffers[analysis_index];
        for (int row = 0; row < buffer.size(); ++row)
        {
          const std::string key = join_key(replicate_names[analysis_index], buffer.name[row]);
          sample_rows_by_key[key].push_back(std::make_tuple(analysis_index, row));

          for (const auto blank_index : blank_lookup[analysis_index])
          {
            if (blank_index >= buffers.size())
            {
              continue;
            }
            const auto &blank_buffer = buffers[blank_index];
            for (int blank_row = 0; blank_row < blank_buffer.size(); ++blank_row)
            {
              if (blank_buffer.name[blank_row] != buffer.name[row])
              {
                continue;
              }
              const double blank_intensity = blank_buffer.intensity[blank_row];
              if (std::isfinite(blank_intensity) && blank_intensity > 0.0)
              {
                blank_intensities_by_key[key].push_back(blank_intensity);
              }
            }
          }
        }
      }

      std::vector<SupportPoint> support;
      for (const auto &entry : sample_rows_by_key)
      {
        const auto blank_it = blank_intensities_by_key.find(entry.first);
        if (blank_it == blank_intensities_by_key.end() || blank_it->second.empty())
        {
          continue;
        }

        double sample_intensity_sum = 0.0;
        double sample_rt_sum = 0.0;
        double sample_mp_sum = 0.0;
        int sample_count = 0;
        int sample_mp_count = 0;

        for (const auto &[analysis_index, row] : entry.second)
        {
          const auto &buffer = buffers[analysis_index];
          const double sample_intensity = buffer.intensity[row];
          if (std::isfinite(sample_intensity) && sample_intensity > 0.0)
          {
            sample_intensity_sum += sample_intensity;
            ++sample_count;
          }

          const double sample_rt = buffer.exp_rt[row];
          if (std::isfinite(sample_rt))
          {
            sample_rt_sum += sample_rt;
          }

          auto profile_it = matrix_profiles.find(analysis_names[analysis_index]);
          if (profile_it == matrix_profiles.end())
          {
            continue;
          }
          const double mp_mean = mean_intensity_in_window(
            profile_it->second,
            sample_rt - rtWindow,
            sample_rt + rtWindow,
            true);
          if (std::isfinite(mp_mean))
          {
            sample_mp_sum += mp_mean;
            ++sample_mp_count;
          }
        }

        if (sample_count == 0)
        {
          continue;
        }

        const double blank_sum = std::accumulate(blank_it->second.begin(), blank_it->second.end(), 0.0);
        const double blank_mean = blank_sum / static_cast<double>(blank_it->second.size());
        const double sample_mean = sample_intensity_sum / static_cast<double>(sample_count);
        if (!std::isfinite(blank_mean) || blank_mean <= 0.0 || !std::isfinite(sample_mean) || sample_mean <= 0.0)
        {
          continue;
        }

        SupportPoint point;
        const auto split_pos = entry.first.find("\x1f");
        point.replicate = entry.first.substr(0, split_pos);
        point.name = entry.first.substr(split_pos + 1);
        point.rt = sample_rt_sum / static_cast<double>(entry.second.size());
        point.mp = sample_mp_count > 0 ? (sample_mp_sum / static_cast<double>(sample_mp_count)) : std::numeric_limits<double>::quiet_NaN();
        point.tichri = ((blank_mean / sample_mean) - 1.0) * (-1.0);
        if (!std::isfinite(point.mp))
        {
          continue;
        }
        support.push_back(point);
      }

      return support;
    }

    std::vector<SupportPoint> select_support_points(
      const std::vector<SupportPoint> &support,
      const std::string &replicate,
      double feature_rt)
    {
      std::vector<SupportPoint> replicate_points;
      for (const auto &point : support)
      {
        if (point.replicate == replicate && std::isfinite(point.mp) && std::isfinite(point.tichri))
        {
          replicate_points.push_back(point);
        }
      }

      if (replicate_points.empty())
      {
        return {};
      }

      std::sort(replicate_points.begin(), replicate_points.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.rt != rhs.rt)
        {
          return lhs.rt < rhs.rt;
        }
        return lhs.name < rhs.name;
      });

      std::vector<SupportPoint> selected;
      bool has_before = false;
      bool has_after = false;
      SupportPoint best_before;
      SupportPoint best_after;
      double before_distance = std::numeric_limits<double>::infinity();
      double after_distance = std::numeric_limits<double>::infinity();

      for (const auto &point : replicate_points)
      {
        const double distance = std::abs(point.rt - feature_rt);
        if (point.rt <= feature_rt && distance < before_distance)
        {
          best_before = point;
          before_distance = distance;
          has_before = true;
        }
        if (point.rt >= feature_rt && distance < after_distance)
        {
          best_after = point;
          after_distance = distance;
          has_after = true;
        }
      }

      if (has_before)
      {
        selected.push_back(best_before);
      }
      if (has_after)
      {
        if (!has_before || best_after.name != best_before.name || best_after.rt != best_before.rt)
        {
          selected.push_back(best_after);
        }
      }

      if (selected.size() < 2 && replicate_points.size() > selected.size())
      {
        std::sort(replicate_points.begin(), replicate_points.end(), [feature_rt](const auto &lhs, const auto &rhs) {
          return std::abs(lhs.rt - feature_rt) < std::abs(rhs.rt - feature_rt);
        });
        for (const auto &point : replicate_points)
        {
          bool duplicate = false;
          for (const auto &existing : selected)
          {
            if (existing.name == point.name && existing.rt == point.rt)
            {
              duplicate = true;
              break;
            }
          }
          if (!duplicate)
          {
            selected.push_back(point);
          }
          if (selected.size() >= 2)
          {
            break;
          }
        }
      }

      return selected;
    }

    double predict_scaled_suppression(
      double mp_feature,
      const std::vector<SupportPoint> &selected_points)
    {
      if (!std::isfinite(mp_feature) || selected_points.empty())
      {
        return std::numeric_limits<double>::quiet_NaN();
      }

      if (selected_points.size() == 1)
      {
        const auto &point = selected_points.front();
        if (!std::isfinite(point.mp) || std::abs(point.mp) < 1e-12)
        {
          return std::numeric_limits<double>::quiet_NaN();
        }
        return mp_feature * (point.tichri / point.mp);
      }

      double x_mean = 0.0;
      double y_mean = 0.0;
      for (const auto &point : selected_points)
      {
        x_mean += point.mp;
        y_mean += point.tichri;
      }
      x_mean /= static_cast<double>(selected_points.size());
      y_mean /= static_cast<double>(selected_points.size());

      double numerator = 0.0;
      double denominator = 0.0;
      for (const auto &point : selected_points)
      {
        numerator += (point.mp - x_mean) * (point.tichri - y_mean);
        denominator += (point.mp - x_mean) * (point.mp - x_mean);
      }

      if (std::abs(denominator) < 1e-12)
      {
        if (std::abs(x_mean) < 1e-12)
        {
          return std::numeric_limits<double>::quiet_NaN();
        }
        return mp_feature * (y_mean / x_mean);
      }

      const double slope = numerator / denominator;
      const double intercept = y_mean - (slope * x_mean);
      return intercept + (slope * mp_feature);
    }
  } // namespace streamfind::nta_correction_detail

  using namespace streamfind::nta_correction_detail;

  std::vector<TIC_MATRIX_SUPPRESSION_ROW> get_matrix_suppression_impl(
    const PROJECT_NON_TARGET_ANALYSIS &nta_data,
    const std::vector<std::string> &analyses,
    float rtWindow,
    const std::string &refBlankReplicate)
  {
    const auto profiles = build_matrix_profiles(nta_data, analyses, rtWindow, refBlankReplicate);
    std::vector<TIC_MATRIX_SUPPRESSION_ROW> out;
    for (const auto &entry : profiles)
    {
      out.insert(out.end(), entry.second.begin(), entry.second.end());
    }
    std::sort(out.begin(), out.end(), [](const auto &lhs, const auto &rhs) {
      if (lhs.analysis != rhs.analysis)
      {
        return lhs.analysis < rhs.analysis;
      }
      return lhs.rt < rhs.rt;
    });
    return out;
  }

  void correct_matrix_suppression_impl(
    PROJECT_NON_TARGET_ANALYSIS &nta_data,
    float mpRtWindow,
    const std::string &refBlankReplicate)
  {
    const std::vector<std::string> analyses = nta_data.analysis_names();
    const auto profiles = build_matrix_profiles(nta_data, analyses, mpRtWindow, refBlankReplicate);
    const auto support_points = build_internal_standard_support(nta_data, profiles, mpRtWindow, refBlankReplicate);
    const auto &replicate_names = nta_data.replicate_names();
    auto &feature_buffers = nta_data.feature_buffers();

    for (std::size_t analysis_index = 0; analysis_index < feature_buffers.size(); ++analysis_index)
    {
      auto profile_it = profiles.find(nta_data.analysis_names()[analysis_index]);
      if (profile_it == profiles.end())
      {
        continue;
      }

      auto &features = feature_buffers[analysis_index];
      for (int row = 0; row < features.size(); ++row)
      {
        const double rtmin = static_cast<double>(features.rtmin[row]) - mpRtWindow;
        const double rtmax = static_cast<double>(features.rtmax[row]) + mpRtWindow;
        const double mp_feature = mean_intensity_in_window(profile_it->second, rtmin, rtmax, true);

        double correction = std::numeric_limits<double>::quiet_NaN();
        const auto selected_points = select_support_points(
          support_points,
          replicate_names[analysis_index],
          static_cast<double>(features.rt[row]));
        if (!selected_points.empty())
        {
          const double scaled_suppression = predict_scaled_suppression(mp_feature, selected_points);
          if (std::isfinite(scaled_suppression))
          {
            correction = 1.0 - scaled_suppression;
          }
        }

        if (!std::isfinite(correction))
        {
          correction = std::isfinite(mp_feature) ? (1.0 - mp_feature) : 1.0;
        }

        features.correction[row] = static_cast<float>(correction);
      }
    }
  }
} // namespace nta::correction_algorithms

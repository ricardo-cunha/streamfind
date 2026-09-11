#include "streamfind/mass_spec/nta_componentization.hpp"
#include "streamfind/mass_spec/mass_spec.hpp"
#include "streamfind/mass_spec/nta.hpp"
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <numeric>
#include <limits>

namespace nta
{
  namespace componentization
  {
    // Helper function to decode base64-encoded EIC data
    std::vector<float> decode_eic_base64(const std::string &base64_str) {
      if (base64_str.empty()) return std::vector<float>();
      try {
        std::string decoded = mass_spec::reader::utils::decode_base64(base64_str);
        return mass_spec::reader::utils::decode_little_endian_to_float(decoded, 4);
      } catch (...) {
        return std::vector<float>();
      }
    }

    // Helper function to calculate Pearson correlation between two aligned EIC vectors
    float calculate_pearson_correlation(
      const std::vector<float> &x,
      const std::vector<float> &y
    ) {
      const size_t n = std::min(x.size(), y.size());
      if (n < 3) return 0.0f; // Need at least 3 points

      // Calculate means
      float mean_x = 0.0f, mean_y = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        mean_x += x[i];
        mean_y += y[i];
      }
      mean_x /= static_cast<float>(n);
      mean_y /= static_cast<float>(n);

      // Calculate correlation
      float numerator = 0.0f, denom_x = 0.0f, denom_y = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        const float dx = x[i] - mean_x;
        const float dy = y[i] - mean_y;
        numerator += dx * dy;
        denom_x += dx * dx;
        denom_y += dy * dy;
      }

      if (denom_x < 1e-10f || denom_y < 1e-10f) return 0.0f;
      return numerator / std::sqrt(denom_x * denom_y);
    }

    // Helper function to align two EICs by their RT values (not shifted by apex)
    // This preserves temporal information so time-shifted peaks show poor correlation
    std::pair<std::vector<float>, std::vector<float>> align_eics_by_rt(
      const std::vector<float> &rt1, const std::vector<float> &int1,
      const std::vector<float> &rt2, const std::vector<float> &int2
    ) {
      std::vector<float> aligned1, aligned2;

      if (rt1.empty() || rt2.empty() || int1.empty() || int2.empty()) {
        return {aligned1, aligned2};
      }

      if (rt1.size() != int1.size() || rt2.size() != int2.size()) {
        return {aligned1, aligned2};
      }

      // Find overlapping RT range
      const float rt1_min = *std::min_element(rt1.begin(), rt1.end());
      const float rt1_max = *std::max_element(rt1.begin(), rt1.end());
      const float rt2_min = *std::min_element(rt2.begin(), rt2.end());
      const float rt2_max = *std::max_element(rt2.begin(), rt2.end());

      const float overlap_start = std::max(rt1_min, rt2_min);
      const float overlap_end = std::min(rt1_max, rt2_max);

      if (overlap_start >= overlap_end) {
        return {aligned1, aligned2}; // No overlap
      }

      // For each RT point in EIC1 within overlap, find closest match in EIC2
      aligned1.reserve(rt1.size());
      aligned2.reserve(rt1.size());

      for (size_t i = 0; i < rt1.size(); ++i) {
        const float rt_val = rt1[i];

        // Skip if outside overlap range
        if (rt_val < overlap_start || rt_val > overlap_end) continue;

        // Find closest RT in EIC2
        size_t best_idx = 0;
        float min_diff = std::abs(rt2[0] - rt_val);

        for (size_t j = 1; j < rt2.size(); ++j) {
          const float diff = std::abs(rt2[j] - rt_val);
          if (diff < min_diff) {
            min_diff = diff;
            best_idx = j;
          }
        }

        // Only include if RT values are reasonably close (within 0.5 seconds)
        if (min_diff <= 0.5f) {
          aligned1.push_back(int1[i]);
          aligned2.push_back(int2[best_idx]);
        }
      }

      return {aligned1, aligned2};
    }

    // MARK: create_components_impl
    void create_components_impl(
      nta::PROJECT_NON_TARGET_ANALYSIS &nta_data,
        const std::vector<float> &rtWindow,
        float minCorrelation,
        float debugRT,
        const std::string &debugAnalysis)
    {
      const float left_offset = rtWindow.size() >= 1 ? rtWindow[0] : 0.0f;
      const float right_offset = rtWindow.size() >= 2 ? rtWindow[1] : 0.0f;

      const bool debug_mode = debugRT > 0.0f;
      if (debug_mode) {
        std::ostringstream log_filename;
        log_filename << "log/debug_log_create_components_"
                     << std::fixed << std::setprecision(2) << debugRT << ".log";
        utils::init_debug_log(log_filename.str(), "=== Component Creation Debug Log ===\n");
      }

      bool debug_triggered = false;
      auto &feature_buffers = nta_data.feature_buffers();
      const auto &analysis_names = nta_data.analysis_names();

      for (size_t i = 0; i < feature_buffers.size(); ++i)
      {
        nta::api::NTA_FEATURES &fts = feature_buffers[i];
        const int n = fts.size();

        if (n == 0)
        {
          continue;
        }

        const bool debug_this_analysis = debugAnalysis.empty() ||
                                         (i < analysis_names.size() && analysis_names[i] == debugAnalysis);
        const bool should_debug = debug_mode && debug_this_analysis;

        if (should_debug && !debug_triggered) {
          debug_triggered = true;
          const std::string analysis_name = i < analysis_names.size() ? analysis_names[i] : std::to_string(i);
          std::cerr << "Debugging components: Analysis '" << analysis_name
                      << "' RT=" << debugRT << " (window " << left_offset << " to " << right_offset
                      << ") -> [" << (debugRT + left_offset) << ", " << (debugRT + right_offset) << "]" << std::endl;
          DEBUG_OUT("\nDebugging analysis: " << analysis_name << "\n");
        }

        std::map<int, std::vector<int>> polarity_groups;
        for (int j = 0; j < n; ++j)
        {
          const nta::api::NTA_FEATURE_ROW &ft = fts.get_feature(j);
          polarity_groups[ft.polarity].push_back(j);
        }

        for (const auto &[polarity, feature_indices] : polarity_groups)
        {
          if (feature_indices.empty()) continue;

          int component_counter = 1;

          // Determine polarity suffix
          std::string polarity_suffix;
          if (polarity > 0) {
            polarity_suffix = "_POS";
          } else if (polarity < 0) {
            polarity_suffix = "_NEG";
          } else {
            polarity_suffix = "";
          }

          struct FeatureIntensity {
            int idx;
            float rt;
            float intensity;
          };

          struct FeatureEIC {
            int idx;
            float rt;
            float intensity;
            std::vector<float> eic_rt;
            std::vector<float> eic_int;
          };

          std::vector<FeatureIntensity> sorted_features;
          sorted_features.reserve(feature_indices.size());

          for (int j : feature_indices) {
            nta::api::NTA_FEATURE_ROW ft = fts.get_feature(j);
            FeatureIntensity fi;
            fi.idx = j;
            fi.rt = static_cast<float>(ft.rt);
            fi.intensity = static_cast<float>(ft.intensity);
            sorted_features.push_back(fi);
          }

          // Sort by descending intensity
          std::sort(sorted_features.begin(), sorted_features.end(),
                    [](const FeatureIntensity &a, const FeatureIntensity &b) {
                      return a.intensity > b.intensity;
                    });

          std::vector<FeatureEIC> feature_eics;
          feature_eics.reserve(sorted_features.size());

          for (const auto &sf : sorted_features) {
            const nta::api::NTA_FEATURE_ROW &ft = fts.get_feature(sf.idx);
            FeatureEIC feic;
            feic.idx = sf.idx;
            feic.rt = static_cast<float>(ft.rt);
            feic.intensity = static_cast<float>(ft.intensity);
            feic.eic_rt = decode_eic_base64(ft.eic_rt);
            feic.eic_int = decode_eic_base64(ft.eic_intensity);
            feature_eics.push_back(std::move(feic));
          }

          std::sort(feature_eics.begin(), feature_eics.end(),
                    [](const FeatureEIC &a, const FeatureEIC &b) {
                      if (a.rt != b.rt) return a.rt < b.rt;
                      if (a.intensity != b.intensity) return a.intensity > b.intensity;
                      return a.idx < b.idx;
                    });

          const size_t feature_count = feature_eics.size();
          const float max_rt_gap = std::max(std::abs(left_offset), std::abs(right_offset));
          auto rt_compatible = [&](float rt_a, float rt_b) {
            return (rt_b >= rt_a + left_offset && rt_b <= rt_a + right_offset) ||
                   (rt_a >= rt_b + left_offset && rt_a <= rt_b + right_offset);
          };

          std::vector<std::vector<int>> adjacency(feature_count);
          std::vector<std::vector<float>> correlation_matrix(feature_count, std::vector<float>(feature_count, std::numeric_limits<float>::quiet_NaN()));
          for (size_t a = 0; a < feature_count; ++a) {
            correlation_matrix[a][a] = 1.0f;
          }
          for (size_t a = 0; a < feature_count; ++a) {
            for (size_t b = a + 1; b < feature_count; ++b) {
              const float rt_delta = feature_eics[b].rt - feature_eics[a].rt;
              if (rt_delta > max_rt_gap) break;
              if (!rt_compatible(feature_eics[a].rt, feature_eics[b].rt)) continue;
              if (minCorrelation <= 0.0f) {
                adjacency[a].push_back(static_cast<int>(b));
                adjacency[b].push_back(static_cast<int>(a));
                continue;
              }
              if (feature_eics[a].eic_rt.empty() || feature_eics[a].eic_int.empty() ||
                  feature_eics[b].eic_rt.empty() || feature_eics[b].eic_int.empty()) {
                continue;
              }

              auto [aligned1, aligned2] = align_eics_by_rt(
                feature_eics[a].eic_rt, feature_eics[a].eic_int,
                feature_eics[b].eic_rt, feature_eics[b].eic_int
              );

              if (aligned1.size() < 3) continue;

              const float corr = calculate_pearson_correlation(aligned1, aligned2);
              correlation_matrix[a][b] = corr;
              correlation_matrix[b][a] = corr;
              if (corr >= minCorrelation) {
                adjacency[a].push_back(static_cast<int>(b));
                adjacency[b].push_back(static_cast<int>(a));
              }
            }
          }

          std::vector<bool> visited(feature_count, false);
          struct ComponentMembers {
            std::vector<int> members;
            float mean_rt = 0.0f;
            float max_intensity = 0.0f;
          };
          std::vector<ComponentMembers> components;

          for (size_t start = 0; start < feature_count; ++start) {
            if (visited[start]) continue;

            std::vector<int> stack{static_cast<int>(start)};
            visited[start] = true;
            ComponentMembers component;
            float rt_sum = 0.0f;

            while (!stack.empty()) {
              const int node = stack.back();
              stack.pop_back();
              component.members.push_back(node);
              rt_sum += feature_eics[node].rt;
              component.max_intensity = std::max(component.max_intensity, feature_eics[node].intensity);

              for (const int neighbor : adjacency[node]) {
                if (!visited[neighbor]) {
                  visited[neighbor] = true;
                  stack.push_back(neighbor);
                }
              }
            }

            component.mean_rt = rt_sum / static_cast<float>(component.members.size());
            std::sort(component.members.begin(), component.members.end(),
                      [&](int lhs, int rhs) {
                        const auto &ft_lhs = fts.get_feature(feature_eics[lhs].idx);
                        const auto &ft_rhs = fts.get_feature(feature_eics[rhs].idx);
                        if (ft_lhs.mz != ft_rhs.mz) return ft_lhs.mz < ft_rhs.mz;
                        if (ft_lhs.rt != ft_rhs.rt) return ft_lhs.rt < ft_rhs.rt;
                        return feature_eics[lhs].idx < feature_eics[rhs].idx;
                      });
            components.push_back(std::move(component));
          }

          std::sort(components.begin(), components.end(),
                    [](const ComponentMembers &a, const ComponentMembers &b) {
                      if (a.max_intensity != b.max_intensity) return a.max_intensity > b.max_intensity;
                      if (a.mean_rt != b.mean_rt) return a.mean_rt < b.mean_rt;
                      return a.members.front() < b.members.front();
                    });

          const std::string analysis_name = i < analysis_names.size() ? analysis_names[i] : std::to_string(i);
          for (const auto &component : components) {
            bool debug_this_component = false;
            if (should_debug) {
              for (const int member : component.members) {
                const float rt = feature_eics[member].rt;
                if (rt >= (debugRT + left_offset) && rt <= (debugRT + right_offset)) {
                  debug_this_component = true;
                  break;
                }
              }
            }

            std::ostringstream oss;
            oss << "FC" << component_counter++ << "_RT" << std::fixed << std::setprecision(0) << component.mean_rt << polarity_suffix;
            const std::string component_id = oss.str();

            if (debug_this_component && should_debug) {
              DEBUG_LOG("\n--- Analysis " << analysis_name
                        << " [Polarity=" << polarity << "]: Graph Component " << component_id
                        << " ---\n");
              DEBUG_LOG("  Members: " << component.members.size()
                        << ", mean RT=" << component.mean_rt
                        << ", max intensity=" << component.max_intensity << "\n");

              for (const int member : component.members) {
                const auto &feic = feature_eics[member];
                const nta::api::NTA_FEATURE_ROW &ft = fts.get_feature(feic.idx);
                DEBUG_LOG("  " << ft.feature
                          << ": RT=" << ft.rt
                          << ", mz=" << ft.mz
                          << ", intensity=" << ft.intensity
                          << ", neighbors=" << adjacency[member].size() << "\n");
              }
            }

            const int member_count = static_cast<int>(component.members.size());
            const float component_rt_center = component.mean_rt;
            float rt_min = std::numeric_limits<float>::max();
            float rt_max = std::numeric_limits<float>::lowest();
            for (const int member : component.members) {
              rt_min = std::min(rt_min, feature_eics[member].rt);
              rt_max = std::max(rt_max, feature_eics[member].rt);
            }
            const float component_rt_spread = member_count > 0 ? (rt_max - rt_min) : 0.0f;

            int possible_pairs = member_count > 1 ? (member_count * (member_count - 1)) / 2 : 0;
            int edge_count = 0;
            int valid_corr_pairs = 0;
            float corr_sum = 0.0f;
            std::unordered_map<int, int> local_index;
            local_index.reserve(component.members.size());
            for (int mi = 0; mi < member_count; ++mi) {
              local_index[component.members[mi]] = mi;
            }

            for (int mi = 0; mi < member_count; ++mi) {
              const int member_a = component.members[mi];
              for (int mj = mi + 1; mj < member_count; ++mj) {
                const int member_b = component.members[mj];
                const float corr = correlation_matrix[member_a][member_b];
                if (!std::isnan(corr)) {
                  corr_sum += corr;
                  ++valid_corr_pairs;
                }
                if (std::find(adjacency[member_a].begin(), adjacency[member_a].end(), member_b) != adjacency[member_a].end()) {
                  ++edge_count;
                }
              }
            }

            const float component_density = possible_pairs > 0 ? static_cast<float>(edge_count) / static_cast<float>(possible_pairs) : 0.0f;
            const float component_mean_corr = valid_corr_pairs > 0 ? corr_sum / static_cast<float>(valid_corr_pairs) : 0.0f;

            std::vector<int> component_degrees(member_count, 0);
            std::vector<bool> articulation_flags(member_count, false);
            for (int mi = 0; mi < member_count; ++mi) {
              const int member = component.members[mi];
              for (const int neighbor : adjacency[member]) {
                if (local_index.find(neighbor) != local_index.end()) {
                  ++component_degrees[mi];
                }
              }
            }

            if (member_count > 2) {
              std::vector<int> disc(member_count, -1), low(member_count, -1), parent(member_count, -1);
              int time_counter = 0;
              std::function<void(int)> dfs = [&](int u) {
                disc[u] = low[u] = time_counter++;
                int children = 0;
                const int member = component.members[u];
                for (const int neighbor_member : adjacency[member]) {
                  const auto it = local_index.find(neighbor_member);
                  if (it == local_index.end()) continue;
                  const int v = it->second;
                  if (disc[v] == -1) {
                    parent[v] = u;
                    ++children;
                    dfs(v);
                    low[u] = std::min(low[u], low[v]);
                    if ((parent[u] == -1 && children > 1) ||
                        (parent[u] != -1 && low[v] >= disc[u])) {
                      articulation_flags[u] = true;
                    }
                  } else if (v != parent[u]) {
                    low[u] = std::min(low[u], disc[v]);
                  }
                }
              };
              for (int mi = 0; mi < member_count; ++mi) {
                if (disc[mi] == -1) dfs(mi);
              }
            }

            std::vector<int> degree_sorted = component_degrees;
            std::sort(degree_sorted.begin(), degree_sorted.end());
            const int median_degree = degree_sorted.empty() ? 0 : degree_sorted[degree_sorted.size() / 2];

            for (int mi = 0; mi < member_count; ++mi) {
              const int member = component.members[mi];
              const int feature_idx = feature_eics[member].idx;
              nta::api::NTA_FEATURE_ROW ft = fts.get_feature(feature_idx);
              float feature_corr_sum = 0.0f;
              int feature_corr_count = 0;
              float feature_max_corr = 0.0f;
              std::string best_partner;

              for (int mj = 0; mj < member_count; ++mj) {
                if (mi == mj) continue;
                const int other_member = component.members[mj];
                const float corr = correlation_matrix[member][other_member];
                if (std::isnan(corr)) continue;
                feature_corr_sum += corr;
                ++feature_corr_count;
                if (best_partner.empty() || corr > feature_max_corr) {
                  feature_max_corr = corr;
                  best_partner = fts.get_feature(feature_eics[other_member].idx).feature;
                }
              }

              const float feature_mean_corr = feature_corr_count > 0 ? feature_corr_sum / static_cast<float>(feature_corr_count) : 0.0f;
              const float membership_score = std::max(0.0f, feature_mean_corr) * component_density;

              ft.feature_component = component_id;
              ft.component_size = member_count;
              ft.component_rt_center = component_rt_center;
              ft.component_rt_spread = component_rt_spread;
              ft.component_density = component_density;
              ft.component_mean_correlation = component_mean_corr;
              ft.component_best_partner = best_partner;
              ft.component_max_correlation = feature_max_corr;
              ft.component_mean_correlation_to_component = feature_mean_corr;
              ft.component_membership_score = membership_score;
              ft.component_is_core = (component_degrees[mi] >= median_degree) && (feature_mean_corr >= component_mean_corr);
              ft.component_bridge_flag = articulation_flags[mi];
              fts.set_feature(feature_idx, ft);
            }
          }
        }
      }

      if (debug_mode) {
        utils::close_debug_log();
      }
    }

  } // namespace componentization
} // namespace nta

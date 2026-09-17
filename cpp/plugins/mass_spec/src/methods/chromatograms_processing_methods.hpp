#pragma once

#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::mass_spec::processing {
STREAMFIND_DOMAIN_API Json load_chromatograms_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters);
STREAMFIND_DOMAIN_API Json filter_chromatograms_retention_time_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters);
STREAMFIND_DOMAIN_API Json find_chromatogram_peaks_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters);
STREAMFIND_DOMAIN_API Json correct_chromatogram_baseline_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters);
STREAMFIND_DOMAIN_API Json smooth_chromatograms_with_access(
    sdk::PluginProjectAccess &access, const Json &parameters);

}

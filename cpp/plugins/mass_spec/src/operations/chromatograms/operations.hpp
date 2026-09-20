#pragma once

#include "streamfind/export.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::mass_spec::chromatograms
{
STREAMFIND_DOMAIN_API Json get_chromatograms(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_chromatogram_peaks(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json load_chromatograms(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json filter_chromatograms_retention_time(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json find_chromatogram_peaks(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json correct_chromatogram_baseline(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json smooth_chromatograms(sdk::PluginProjectAccess &, const Json &);
}

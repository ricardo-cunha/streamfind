#pragma once

#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/export.hpp"
#include "utils/base.hpp"

namespace streamfind::mass_spec
{


    namespace base
    {
        STREAMFIND_DOMAIN_API Json add_analyses(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json remove_analyses(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_analyses_info(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_analysis_names(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_replicate_names(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_blank_names(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_concentrations(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json set_replicate_names(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json set_blank_names(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json set_concentrations(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_spectra_headers(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_chromatograms_headers(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_spectra_tic(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_raw_spectra(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_raw_spectra_eic(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_raw_spectra_ms1(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_raw_spectra_ms2(sdk::PluginProjectAccess &, const Json &);
        STREAMFIND_DOMAIN_API Json get_raw_chromatograms(sdk::PluginProjectAccess &, const Json &);
    }

}

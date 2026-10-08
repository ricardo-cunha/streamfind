#include "streamfind/sdk/plugin_host_access.hpp"
#include "streamfind/sdk/capability_registry.hpp"
#include "operations/base.hpp"
#include "operations/fragmentation/fragment_suspect_targets.hpp"
#include "operations/fragmentation/fragmentation_to_suspect_targets.hpp"
#include "operations/chromatograms/operations.hpp"
#include "operations/nta/nta_deconvolution.hpp"
#include "operations/nta/operations.hpp"
#include "streamfind/plugin_abi.h"
#include "utils/tools_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::dynamic_detail {

using Json = nlohmann::json;

void report_error(const streamfind_plugin_host_api *host, const std::string &message) {
    if (host != nullptr && host->report_error != nullptr)
        host->report_error(message.data(), static_cast<uint32_t>(message.size()), host->user_data);
}

const sdk::CapabilityRegistry &capabilities() {
    static const sdk::CapabilityRegistry registry{
        {"mass_spec.read_mass_spec_files", sdk::CapabilityKind::Operation, &base::read_mass_spec_files},
        {"mass_spec.read_csv_targets", sdk::CapabilityKind::Operation, &base::read_csv_targets},
        {"mass_spec.read_mol_suspect_target", sdk::CapabilityKind::Operation, &base::read_mol_suspect_target},
        {"mass_spec.fragment_suspect_targets", sdk::CapabilityKind::Operation, &fragmentation::fragment_suspect_targets::run},
        {"mass_spec.fragmentation_to_suspect_targets", sdk::CapabilityKind::Operation, &fragmentation::fragmentation_to_suspect_targets::run},

        {"mass_spec.remove_analyses", sdk::CapabilityKind::Operation, &base::remove_analyses},
        {"mass_spec.get_analyses", sdk::CapabilityKind::Operation, &base::get_analyses},
        {"mass_spec.get_analysis_names", sdk::CapabilityKind::Operation, &base::get_analysis_names},
        {"mass_spec.get_replicate_names", sdk::CapabilityKind::Operation, &base::get_replicate_names},
        {"mass_spec.get_blank_names", sdk::CapabilityKind::Operation, &base::get_blank_names},
        {"mass_spec.get_concentrations", sdk::CapabilityKind::Operation, &base::get_concentrations},
        {"mass_spec.set_replicate_names", sdk::CapabilityKind::Operation, &base::set_replicate_names},
        {"mass_spec.set_blank_names", sdk::CapabilityKind::Operation, &base::set_blank_names},
        {"mass_spec.set_concentrations", sdk::CapabilityKind::Operation, &base::set_concentrations},
        {"mass_spec.get_spectra_headers", sdk::CapabilityKind::Operation, &base::get_spectra_headers},
        {"mass_spec.get_chromatograms_headers", sdk::CapabilityKind::Operation, &base::get_chromatograms_headers},
        {"mass_spec.get_spectra_tic", sdk::CapabilityKind::Operation, &base::get_spectra_tic},
        {"mass_spec.plot_spectra_tic", sdk::CapabilityKind::Operation, &base::plot_spectra_tic},
        {"mass_spec.get_raw_spectra", sdk::CapabilityKind::Operation, &base::get_raw_spectra},
        {"mass_spec.get_raw_spectra_eic", sdk::CapabilityKind::Operation, &base::get_raw_spectra_eic},
        {"mass_spec.get_raw_spectra_ms1", sdk::CapabilityKind::Operation, &base::get_raw_spectra_ms1},
        {"mass_spec.get_raw_spectra_ms2", sdk::CapabilityKind::Operation, &base::get_raw_spectra_ms2},
        {"mass_spec.get_raw_chromatograms", sdk::CapabilityKind::Operation, &base::get_raw_chromatograms},
        {"mass_spec.get_chromatograms", sdk::CapabilityKind::Operation, &chromatograms::get_chromatograms},
        {"mass_spec.get_chromatogram_peaks", sdk::CapabilityKind::Operation, &chromatograms::get_chromatogram_peaks},
        {"mass_spec.load_chromatograms", sdk::CapabilityKind::Operation, &chromatograms::load_chromatograms},
        {"mass_spec.filter_chromatograms_retention_time", sdk::CapabilityKind::Operation, &chromatograms::filter_chromatograms_retention_time},
        {"mass_spec.find_chromatogram_peaks", sdk::CapabilityKind::Operation, &chromatograms::find_chromatogram_peaks},
        {"mass_spec.correct_chromatogram_baseline", sdk::CapabilityKind::Operation, &chromatograms::correct_chromatogram_baseline},
        {"mass_spec.smooth_chromatograms", sdk::CapabilityKind::Operation, &chromatograms::smooth_chromatograms},
        {"mass_spec.find_features", sdk::CapabilityKind::Operation, &nta::deconvolution::find_features},
        {"mass_spec.load_features_ms1", sdk::CapabilityKind::Operation, &nta::load_features_ms1::run},
        {"mass_spec.load_features_ms2", sdk::CapabilityKind::Operation, &nta::load_features_ms2::run},
        {"mass_spec.subtract_blank", sdk::CapabilityKind::Operation, &nta::subtract_blank::run},
        {"mass_spec.filter_features", sdk::CapabilityKind::Operation, &nta::filter_features::run},
        {"mass_spec.filter_features_ms2", sdk::CapabilityKind::Operation, &nta::filter_features_ms2::run},
        {"mass_spec.group_features", sdk::CapabilityKind::Operation, &nta::group_features::run},
        {"mass_spec.fill_features", sdk::CapabilityKind::Operation, &nta::fill_features::run},
        {"mass_spec.create_components", sdk::CapabilityKind::Operation, &nta::create_components::run},
        {"mass_spec.annotate_components", sdk::CapabilityKind::Operation, &nta::annotate_components::run},
        {"mass_spec.suspect_screening", sdk::CapabilityKind::Operation, &nta::suspect_screening::run},
        {"mass_spec.read_csv_suspect_targets", sdk::CapabilityKind::Operation, &nta::read_csv_suspect_targets::run},
        {"mass_spec.find_internal_standards", sdk::CapabilityKind::Operation, &nta::find_internal_standards::run},
        {"mass_spec.filter_suspects", sdk::CapabilityKind::Operation, &nta::filter_suspects::run},
        {"mass_spec.filter_internal_standards", sdk::CapabilityKind::Operation, &nta::filter_internal_standards::run},
        {"mass_spec.correct_matrix_suppression", sdk::CapabilityKind::Operation, &nta::correct_matrix_suppression::run},
        {"mass_spec.assign_transformation_products", sdk::CapabilityKind::Operation, &nta::assign_transformation_products::run},
        {"mass_spec.metfrag_screening", sdk::CapabilityKind::Operation, &nta::metfrag_screening::run},
        {"mass_spec.get_features", sdk::CapabilityKind::Operation, &nta::get_features},
        {"mass_spec.get_suspects", sdk::CapabilityKind::Operation, &nta::get_suspects},
        {"mass_spec.get_internal_standards", sdk::CapabilityKind::Operation, &nta::get_internal_standards},
        {"mass_spec.get_transformation_products", sdk::CapabilityKind::Operation, &nta::get_transformation_products},
    };
    return registry;
}

streamfind_plugin_status invoke(
    void *execution_context, const char *request_json, uint32_t request_size,
    streamfind_plugin_buffer *result_json, void *user_data) {
    if (execution_context == nullptr || request_json == nullptr || result_json == nullptr || user_data == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    try {
        const auto request = Json::parse(std::string(request_json, request_size));
        const auto capability = request.at("capability_id").get<std::string>();
        const auto parameters = request.value("parameters", Json::object());
        sdk::PluginHostAccess access(*host, execution_context);
        Json response;
        const auto *binding = capabilities().find(capability);
        if (binding == nullptr)
            return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
        auto operation_parameters = parameters;
        operation_parameters["_inputs"] = request.value("inputs", Json::object());
        response = binding->handler(access, operation_parameters);
        const auto text = response.dump();
        auto *buffer = static_cast<char *>(host->allocate(text.size(), alignof(char), host->user_data));
        if (buffer == nullptr) return STREAMFIND_PLUGIN_ERROR;
        std::memcpy(buffer, text.data(), text.size());
        result_json->data = buffer;
        result_json->size = static_cast<uint32_t>(text.size());
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::exception &error) {
        report_error(host, error.what());
        if (host->is_cancelled != nullptr &&
            host->is_cancelled(execution_context, host->user_data) != 0)
            return STREAMFIND_PLUGIN_CANCELLED;
        return STREAMFIND_PLUGIN_ERROR;
    }
}

void release_buffer(streamfind_plugin_buffer *buffer, void *user_data) {
    if (buffer == nullptr || buffer->data == nullptr || user_data == nullptr) return;
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    if (host->deallocate != nullptr)
        host->deallocate(const_cast<char *>(buffer->data), buffer->size, alignof(char), host->user_data);
    buffer->data = nullptr;
    buffer->size = 0;
}

streamfind_plugin_status write_dependency_result(
    const Json &value, streamfind_plugin_buffer *result_json,
    const streamfind_plugin_host_api *host) {
    if (result_json == nullptr || host == nullptr || host->allocate == nullptr)
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    const auto text = value.dump();
    auto *buffer = static_cast<char *>(host->allocate(text.size(), alignof(char), host->user_data));
    if (buffer == nullptr) return STREAMFIND_PLUGIN_ERROR;
    std::memcpy(buffer, text.data(), text.size());
    result_json->data = buffer;
    result_json->size = static_cast<uint32_t>(text.size());
    return STREAMFIND_PLUGIN_OK;
}

streamfind_plugin_status describe_dependencies(streamfind_plugin_buffer *result_json, void *user_data) {
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    return write_dependency_result(Json::array({
        {{"id", "runtime.java"}, {"label", "Java Runtime"}, {"version", "21"}, {"kind", "runtime"},
         {"required_by", Json::array({"mass_spec.metfrag_screening", "mass_spec.fragment_suspect_targets"})}, {"managed_path", ".streamfind/tools/java"},
         {"installable", true}, {"network_required", true},
         {"available", ::streamfind::mass_spec::tools::resolve_java().has_value()}},
        {{"id", "mass_spec.metfrag_fragmenter"}, {"label", "MetFrag Fragmenter"}, {"version", "0.1.0"}, {"kind", "jar"},
         {"required_by", Json::array({"mass_spec.fragment_suspect_targets"})}, {"managed_path", ".streamfind/tools/metfrag/streamfind-metfrag-fragmenter.jar"},
         {"installable", true}, {"network_required", true},
         {"available", ::streamfind::mass_spec::tools::resolve_metfrag_fragmenter_jar().has_value()}},
        {{"id", "mass_spec.metfrag"}, {"label", "MetFragCL"}, {"version", "2.6.11"}, {"kind", "jar"},
         {"required_by", Json::array({"mass_spec.metfrag_screening"})}, {"managed_path", ".streamfind/tools/metfrag/MetFragCL.jar"},
         {"installable", true}, {"network_required", true},
         {"available", ::streamfind::mass_spec::tools::resolve_metfrag_jar().has_value()}}}), result_json, host);
}

streamfind_plugin_status install_dependencies(
    const char *request_json, uint32_t request_size,
    streamfind_plugin_buffer *result_json, void *user_data) {
    const auto *host = static_cast<const streamfind_plugin_host_api *>(user_data);
    try {
        const auto request = Json::parse(std::string(request_json == nullptr ? "" : request_json, request_size));
        Json results = Json::array();
        for (const auto &value : request.value("dependency_ids", Json::array())) {
            const auto id = value.get<std::string>();
            std::string path;
            if (id == "runtime.java") path = ::streamfind::mass_spec::tools::install_java();
            else if (id == "mass_spec.metfrag") path = ::streamfind::mass_spec::tools::install_metfrag();
            else if (id == "mass_spec.metfrag_fragmenter") path = ::streamfind::mass_spec::tools::install_metfrag_fragmenter();
            else throw std::invalid_argument("mass_spec does not provide dependency " + id);
            results.push_back({{"dependency_id", id}, {"status", "installed"}, {"path", path}});
        }
        return write_dependency_result(Json{{"results", results}}, result_json, host);
    } catch (const std::exception &error) {
        report_error(host, error.what());
        return STREAMFIND_PLUGIN_ERROR;
    }
}

streamfind_plugin_status register_plugin(
    const streamfind_plugin_host_api *host, streamfind_plugin_api *plugin, void *) {
    if (host == nullptr || plugin == nullptr) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    plugin->module_id = "mass_spec.nta";
    plugin->domain_id = "mass_spec";
    plugin->invoke = &invoke;
    plugin->release_buffer = &release_buffer;
    plugin->describe_dependencies = &describe_dependencies;
    plugin->install_dependencies = &install_dependencies;
    plugin->user_data = const_cast<streamfind_plugin_host_api *>(host);
    return STREAMFIND_PLUGIN_OK;
}

void shutdown_plugin(streamfind_plugin_api *, void *) {}

}  // namespace streamfind::mass_spec::dynamic_detail

extern "C" STREAMFIND_PLUGIN_EXPORT streamfind_plugin_status
streamfind_plugin_get_descriptor(
    uint32_t requested_abi_major, uint32_t requested_abi_minor,
    streamfind_plugin_descriptor *descriptor) {
    if (descriptor == nullptr || requested_abi_major != STREAMFIND_PLUGIN_ABI_MAJOR ||
        requested_abi_minor > STREAMFIND_PLUGIN_ABI_MINOR)
        return STREAMFIND_PLUGIN_INCOMPATIBLE_ABI;
    *descriptor = {};
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    descriptor->abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    descriptor->plugin_id = "mass_spec";
    descriptor->plugin_version = "0.5.0";
    descriptor->register_plugin = &streamfind::mass_spec::dynamic_detail::register_plugin;
    descriptor->shutdown_plugin = &streamfind::mass_spec::dynamic_detail::shutdown_plugin;
    return STREAMFIND_PLUGIN_OK;
}

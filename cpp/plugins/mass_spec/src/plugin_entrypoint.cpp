#include "streamfind/sdk/plugin_host_access.hpp"
#include "streamfind/sdk/capability_registry.hpp"
#include "methods/nta_processing_methods.hpp"
#include "methods/chromatograms_processing_methods.hpp"
#include "operations/operations.hpp"
#include "streamfind/plugin_abi.h"

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
        {"mass_spec.add_analyses", sdk::CapabilityKind::Operation, &operations::add_analyses},
        {"mass_spec.get_analyses_info", sdk::CapabilityKind::Operation, &operations::get_analyses_info},
        {"mass_spec.remove_analyses", sdk::CapabilityKind::Operation, &operations::remove_analyses},
        {"mass_spec.get_analysis_names", sdk::CapabilityKind::Operation, &operations::get_analysis_names},
        {"mass_spec.get_replicate_names", sdk::CapabilityKind::Operation, &operations::get_replicate_names},
        {"mass_spec.get_blank_names", sdk::CapabilityKind::Operation, &operations::get_blank_names},
        {"mass_spec.get_concentrations", sdk::CapabilityKind::Operation, &operations::get_concentrations},
        {"mass_spec.set_replicate_names", sdk::CapabilityKind::Operation, &operations::set_replicate_names},
        {"mass_spec.set_blank_names", sdk::CapabilityKind::Operation, &operations::set_blank_names},
        {"mass_spec.set_concentrations", sdk::CapabilityKind::Operation, &operations::set_concentrations},
        {"mass_spec.get_spectra_headers", sdk::CapabilityKind::Operation, &operations::get_spectra_headers},
        {"mass_spec.get_chromatograms_headers", sdk::CapabilityKind::Operation, &operations::get_chromatograms_headers},
        {"mass_spec.get_spectra_tic", sdk::CapabilityKind::Operation, &operations::get_spectra_tic},
        {"mass_spec.get_raw_spectra", sdk::CapabilityKind::Operation, &operations::get_raw_spectra},
        {"mass_spec.get_raw_spectra_eic", sdk::CapabilityKind::Operation, &operations::get_raw_spectra_eic},
        {"mass_spec.get_raw_spectra_ms1", sdk::CapabilityKind::Operation, &operations::get_raw_spectra_ms1},
        {"mass_spec.get_raw_spectra_ms2", sdk::CapabilityKind::Operation, &operations::get_raw_spectra_ms2},
        {"mass_spec.get_chromatograms", sdk::CapabilityKind::Operation, &operations::get_chromatograms},
        {"mass_spec.get_raw_chromatograms", sdk::CapabilityKind::Operation, &operations::get_raw_chromatograms},
        {"mass_spec.get_features", sdk::CapabilityKind::Operation, &operations::get_features},
        {"mass_spec.get_internal_standards", sdk::CapabilityKind::Operation, &operations::get_internal_standards},
        {"mass_spec.get_suspects", sdk::CapabilityKind::Operation, &operations::get_suspects},
        {"mass_spec.get_transformation_products", sdk::CapabilityKind::Operation, &operations::get_transformation_products},
        {"mass_spec.find_features", sdk::CapabilityKind::Method, &processing_methods::find_features_with_access},
        {"mass_spec.load_features_ms1", sdk::CapabilityKind::Method, &processing_methods::load_features_ms1_with_access},
        {"mass_spec.load_features_ms2", sdk::CapabilityKind::Method, &processing_methods::load_features_ms2_with_access},
        {"mass_spec.subtract_blank", sdk::CapabilityKind::Method, &processing_methods::subtract_blank_with_access},
        {"mass_spec.filter_features", sdk::CapabilityKind::Method, &processing_methods::filter_features_with_access},
        {"mass_spec.filter_features_ms2", sdk::CapabilityKind::Method, &processing_methods::filter_features_ms2_with_access},
        {"mass_spec.group_features", sdk::CapabilityKind::Method, &processing_methods::group_features_with_access},
        {"mass_spec.fill_features", sdk::CapabilityKind::Method, &processing_methods::fill_features_with_access},
        {"mass_spec.create_components", sdk::CapabilityKind::Method, &processing_methods::create_components_with_access},
        {"mass_spec.annotate_components", sdk::CapabilityKind::Method, &processing_methods::annotate_components_with_access},
        {"mass_spec.suspect_screening", sdk::CapabilityKind::Method, &processing_methods::suspect_screening_with_access},
        {"mass_spec.filter_suspects", sdk::CapabilityKind::Method, &processing_methods::filter_suspects_with_access},
        {"mass_spec.find_internal_standards", sdk::CapabilityKind::Method, &processing_methods::find_internal_standards_with_access},
        {"mass_spec.filter_internal_standards", sdk::CapabilityKind::Method, &processing_methods::filter_internal_standards_with_access},
        {"mass_spec.correct_matrix_suppression", sdk::CapabilityKind::Method, &processing_methods::correct_matrix_suppression_with_access},
        {"mass_spec.assign_transformation_products", sdk::CapabilityKind::Method, &processing_methods::assign_transformation_products_with_access},
        {"mass_spec.metfrag_screening", sdk::CapabilityKind::Method, &processing_methods::metfrag_screening_with_access},
        {"mass_spec.load_chromatograms", sdk::CapabilityKind::Method, &processing::load_chromatograms_with_access},
        {"mass_spec.filter_chromatograms_retention_time", sdk::CapabilityKind::Method, &processing::filter_chromatograms_retention_time_with_access},
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
        response = binding->handler(access, parameters);
        const auto text = response.dump();
        auto *buffer = static_cast<char *>(host->allocate(text.size(), alignof(char), host->user_data));
        if (buffer == nullptr) return STREAMFIND_PLUGIN_ERROR;
        std::memcpy(buffer, text.data(), text.size());
        result_json->data = buffer;
        result_json->size = static_cast<uint32_t>(text.size());
        return STREAMFIND_PLUGIN_OK;
    } catch (const std::exception &error) {
        report_error(host, error.what());
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

streamfind_plugin_status register_plugin(
    const streamfind_plugin_host_api *host, streamfind_plugin_api *plugin, void *) {
    if (host == nullptr || plugin == nullptr) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    plugin->module_id = "mass_spec.nta";
    plugin->domain_id = "mass_spec";
    plugin->invoke = &invoke;
    plugin->release_buffer = &release_buffer;
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
    descriptor->plugin_version = "0.2.0";
    descriptor->register_plugin = &streamfind::mass_spec::dynamic_detail::register_plugin;
    descriptor->shutdown_plugin = &streamfind::mass_spec::dynamic_detail::shutdown_plugin;
    return STREAMFIND_PLUGIN_OK;
}

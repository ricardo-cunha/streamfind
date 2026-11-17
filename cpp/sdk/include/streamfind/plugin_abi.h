#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define STREAMFIND_PLUGIN_EXPORT __declspec(dllexport)
#else
#define STREAMFIND_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STREAMFIND_PLUGIN_ABI_MAJOR = 3,
    STREAMFIND_PLUGIN_ABI_MINOR = 0,
};

typedef int32_t streamfind_plugin_status;

enum {
    STREAMFIND_PLUGIN_OK = 0,
    STREAMFIND_PLUGIN_ERROR = 1,
    STREAMFIND_PLUGIN_INCOMPATIBLE_ABI = 2,
    STREAMFIND_PLUGIN_INVALID_ARGUMENT = 3,
    STREAMFIND_PLUGIN_REGISTRATION_FAILED = 4,
    STREAMFIND_PLUGIN_SCHEMA_ERROR = 5,
    STREAMFIND_PLUGIN_CANCELLED = 6,
    STREAMFIND_PLUGIN_NOT_ALLOWED = 7,
};

typedef struct streamfind_plugin_host_api streamfind_plugin_host_api;
typedef struct streamfind_plugin_api streamfind_plugin_api;
typedef struct streamfind_plugin_buffer streamfind_plugin_buffer;
/* Batch callbacks transfer Arrow C Data ownership to the receiver. The receiver
 * imports or releases both objects before returning; a still-populated release
 * callback is released by the caller after the callback returns. */
struct ArrowArray;
struct ArrowSchema;
typedef streamfind_plugin_status (*streamfind_plugin_consume_batch_fn)(
    void *consumer_context,
    struct ArrowArray *array,
    struct ArrowSchema *schema);

typedef enum streamfind_plugin_column_type {
    STREAMFIND_PLUGIN_COLUMN_UTF8 = 1,
    STREAMFIND_PLUGIN_COLUMN_INT64 = 2,
    STREAMFIND_PLUGIN_COLUMN_FLOAT64 = 3,
    STREAMFIND_PLUGIN_COLUMN_BOOL = 4,
    STREAMFIND_PLUGIN_COLUMN_TIMESTAMP = 5,
    STREAMFIND_PLUGIN_COLUMN_DECIMAL = 6,
    STREAMFIND_PLUGIN_COLUMN_BINARY = 7,
} streamfind_plugin_column_type;

enum { STREAMFIND_PLUGIN_COLUMN_FLAG_ARRAY = 1u << 0 };

typedef void (*streamfind_plugin_report_error_fn)(
    const char *message,
    uint32_t message_size,
    void *user_data);

typedef void *(*streamfind_plugin_allocate_fn)(
    uint64_t size,
    uint64_t alignment,
    void *user_data);

typedef void (*streamfind_plugin_deallocate_fn)(
    void *pointer,
    uint64_t size,
    uint64_t alignment,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_has_table_fn)(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    uint8_t *exists,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_read_batch_fn)(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    const char *const *requested_columns,
    uint32_t column_count,
    uint64_t offset,
    uint64_t limit,
    streamfind_plugin_consume_batch_fn consume,
    void *consumer_context,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_count_rows_fn)(
    void *execution_context,
    const char *table_name,
    uint32_t table_name_size,
    uint64_t *row_count,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_emit_table_batch_fn)(
    void *execution_context,
    const char *output_contract_id,
    uint32_t output_contract_id_size,
    struct ArrowArray *array,
    struct ArrowSchema *schema,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_emit_result_fn)(
    void *execution_context,
    const char *output_contract_id,
    uint32_t output_contract_id_size,
    const char *payload,
    uint64_t payload_size,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_report_progress_fn)(
    void *execution_context,
    double fraction,
    const char *message,
    uint32_t message_size,
    void *user_data);

typedef uint8_t (*streamfind_plugin_is_cancelled_fn)(
    void *execution_context,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_invoke_fn)(
    void *execution_context,
    const char *request_json,
    uint32_t request_size,
    streamfind_plugin_buffer *result_json,
    void *user_data);

typedef void (*streamfind_plugin_release_buffer_fn)(
    streamfind_plugin_buffer *buffer,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_describe_dependencies_fn)(
    streamfind_plugin_buffer *result_json,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_install_dependencies_fn)(
    const char *request_json,
    uint32_t request_size,
    streamfind_plugin_buffer *result_json,
    void *user_data);

typedef streamfind_plugin_status (*streamfind_plugin_register_fn)(
    const streamfind_plugin_host_api *host,
    streamfind_plugin_api *plugin,
    void *user_data);

typedef void (*streamfind_plugin_shutdown_fn)(
    streamfind_plugin_api *plugin,
    void *user_data);

struct streamfind_plugin_host_api {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    streamfind_plugin_report_error_fn report_error;
    streamfind_plugin_allocate_fn allocate;
    streamfind_plugin_deallocate_fn deallocate;
    streamfind_plugin_has_table_fn has_table;
    streamfind_plugin_read_batch_fn read_batch;
    streamfind_plugin_report_progress_fn report_progress;
    streamfind_plugin_is_cancelled_fn is_cancelled;
    void *user_data;
    streamfind_plugin_emit_table_batch_fn emit_table_batch;
    streamfind_plugin_emit_result_fn emit_result;
    streamfind_plugin_count_rows_fn count_rows;
};

struct streamfind_plugin_api {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    const char *module_id;
    const char *domain_id;
    streamfind_plugin_invoke_fn invoke;
    streamfind_plugin_release_buffer_fn release_buffer;
    void *user_data;
    streamfind_plugin_describe_dependencies_fn describe_dependencies;
    streamfind_plugin_install_dependencies_fn install_dependencies;
};

struct streamfind_plugin_buffer {
    const char *data;
    uint32_t size;
};

struct streamfind_plugin_descriptor {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    const char *plugin_id;
    const char *plugin_version;
    streamfind_plugin_register_fn register_plugin;
    streamfind_plugin_shutdown_fn shutdown_plugin;
    void *user_data;
};

typedef streamfind_plugin_status (*streamfind_plugin_get_descriptor_fn)(
    uint32_t requested_abi_major,
    uint32_t requested_abi_minor,
    streamfind_plugin_descriptor *descriptor);

STREAMFIND_PLUGIN_EXPORT streamfind_plugin_status
streamfind_plugin_get_descriptor(
    uint32_t requested_abi_major,
    uint32_t requested_abi_minor,
    streamfind_plugin_descriptor *descriptor);

#ifdef __cplusplus
}
#endif

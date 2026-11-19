#include "../acquisition/observation.hpp"

#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <MQTTClient.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cmath>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {
namespace mqtt_internal {

using Json = nlohmann::json;
using Cell = sdk::TableBatchBuilder::Cell;

struct Message {
    std::string topic;
    std::string payload;
};

struct State {
    std::mutex mutex;
    std::deque<Message> messages;
    std::size_t pending_bytes = 0;
    std::size_t max_pending_bytes = 0;
    bool queue_overflow = false;
    std::string connection_error;
};

struct ClientHolder {
    MQTTClient client = nullptr;

    ~ClientHolder() {
        if (client != nullptr) MQTTClient_destroy(&client);
    }
};

std::string string_parameter(const Json &parameters, const char *name,
                             const std::string &default_value = {}) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    return found->get<std::string>();
}

std::size_t positive_integer_parameter(const Json &parameters, const char *name,
                                       std::size_t default_value,
                                       std::size_t maximum) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_number_integer() || found->get<std::int64_t>() <= 0 ||
        static_cast<std::uint64_t>(found->get<std::int64_t>()) > maximum)
        throw std::invalid_argument(std::string(name) + " must be a positive integer within its limit");
    return static_cast<std::size_t>(found->get<std::int64_t>());
}

std::size_t non_negative_integer_parameter(const Json &parameters, const char *name,
                                           std::size_t default_value,
                                           std::size_t maximum) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_number_integer() || found->get<std::int64_t>() < 0 ||
        static_cast<std::uint64_t>(found->get<std::int64_t>()) > maximum)
        throw std::invalid_argument(std::string(name) + " must be a non-negative integer within its limit");
    return static_cast<std::size_t>(found->get<std::int64_t>());
}

int qos_parameter(const Json &parameters) {
    const auto found = parameters.find("qos");
    if (found == parameters.end() || found->is_null()) return 1;
    if (!found->is_number_integer()) throw std::invalid_argument("qos must be an integer from 0 to 2");
    const auto value = found->get<std::int64_t>();
    if (value < 0 || value > 2) throw std::invalid_argument("qos must be an integer from 0 to 2");
    return static_cast<int>(value);
}

double positive_real_parameter(const Json &parameters, const char *name,
                               double default_value, double maximum) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_number())
        throw std::invalid_argument(std::string(name) + " must be numeric");
    const auto value = found->get<double>();
    if (!std::isfinite(value) || value <= 0.0 || value > maximum)
        throw std::invalid_argument(std::string(name) + " must be finite, positive, and within its limit");
    return value;
}

double non_negative_real_parameter(const Json &parameters, const char *name,
                                   double default_value, double maximum) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_number())
        throw std::invalid_argument(std::string(name) + " must be numeric");
    const auto value = found->get<double>();
    if (!std::isfinite(value) || value < 0.0 || value > maximum)
        throw std::invalid_argument(std::string(name) + " must be finite, non-negative, and within its limit");
    return value;
}

std::string utc_now_text() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - seconds).count();
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(6) << std::setfill('0') << micros << 'Z';
    return output.str();
}

std::string mqtt_error(int code, std::string_view action) {
    return std::string(action) + " failed with MQTT error " + std::to_string(code);
}

sdk::TableBatchBuilder make_output_batch() {
    return sdk::TableBatchBuilder({
        {"source_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"signal_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"event_time", STREAMFIND_PLUGIN_COLUMN_TIMESTAMP},
        {"received_at", STREAMFIND_PLUGIN_COLUMN_TIMESTAMP},
        {"time_origin", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_type", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_double", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"value_int", STREAMFIND_PLUGIN_COLUMN_INT64},
        {"value_bool", STREAMFIND_PLUGIN_COLUMN_BOOL},
        {"value_text", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_json", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"unit", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"quality", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"status_code", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"capture_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"event_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
    });
}

std::size_t observation_size(const acquisition::SensorObservation &observation) {
    std::size_t size = observation.source_id.size() + observation.signal_id.size() +
        observation.event_time.size() + observation.received_at.size() +
        observation.capture_id.size() + observation.event_id.size() + 128;
    if (observation.value.text.has_value()) size += observation.value.text->size();
    if (observation.unit.has_value()) size += observation.unit->size();
    if (observation.status_code.has_value()) size += observation.status_code->size();
    return size;
}

acquisition::SensorValue sensor_value(const Json &value) {
    acquisition::SensorValue result;
    if (value.is_null()) return result;
    if (value.is_boolean()) {
        result.kind = acquisition::SensorValueKind::Boolean;
        result.boolean = value.get<bool>();
    } else if (value.is_number_integer()) {
        result.kind = acquisition::SensorValueKind::Integer;
        result.integer = value.get<std::int64_t>();
    } else if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            result.kind = acquisition::SensorValueKind::Integer;
            result.integer = static_cast<std::int64_t>(number);
        } else {
            result.kind = acquisition::SensorValueKind::Real;
            result.real = static_cast<double>(number);
        }
    } else if (value.is_number_float()) {
        result.kind = acquisition::SensorValueKind::Real;
        result.real = value.get<double>();
    } else if (value.is_string()) {
        result.kind = acquisition::SensorValueKind::Text;
        result.text = value.get<std::string>();
    } else {
        result.kind = acquisition::SensorValueKind::Json;
        result.text = value.dump();
    }
    return result;
}

std::string quality_value(const Json &payload, const acquisition::SensorValue &value) {
    const auto found = payload.find("quality");
    if (found != payload.end() && found->is_string()) {
        const auto quality = found->get<std::string>();
        if (quality == "good" || quality == "uncertain" || quality == "bad" || quality == "missing")
            return quality;
    }
    return value.kind == acquisition::SensorValueKind::Missing ? "missing" : "good";
}

acquisition::SensorObservation decode_message(
    const Message &message, const std::string &signal_id, const std::string &source_id,
    const std::string &capture_id, const std::string &payload_format, std::uint64_t sequence) {
    const auto received_at = utc_now_text();
    Json payload;
    if (payload_format == "json") {
        try {
            payload = Json::parse(message.payload);
        } catch (const Json::parse_error &error) {
            throw std::invalid_argument("MQTT JSON payload could not be parsed: " + std::string(error.what()));
        }
    } else {
        payload = message.payload;
    }

    Json value_payload = payload;
    std::string event_time = received_at;
    std::optional<std::string> unit;
    std::optional<std::string> status_code;
    std::string time_origin = "receiver";
    if (payload_format == "json" && payload.is_object()) {
        if (payload.contains("value")) value_payload = payload.at("value");
        if (payload.contains("event_time") && payload.at("event_time").is_string()) {
            event_time = payload.at("event_time").get<std::string>();
            time_origin = "source";
        }
        if (payload.contains("unit") && payload.at("unit").is_string())
            unit = payload.at("unit").get<std::string>();
        if (payload.contains("status_code") && payload.at("status_code").is_string())
            status_code = payload.at("status_code").get<std::string>();
        if (payload.contains("time_origin") && payload.at("time_origin").is_string())
            time_origin = payload.at("time_origin").get<std::string>();
    }

    acquisition::SensorObservation observation;
    observation.source_id = source_id;
    observation.signal_id = signal_id;
    observation.event_time = event_time;
    observation.received_at = received_at;
    observation.time_origin = time_origin;
    observation.value = sensor_value(value_payload);
    observation.unit = unit;
    observation.quality = quality_value(payload, observation.value);
    observation.status_code = status_code;
    observation.capture_id = capture_id;
    observation.event_id = capture_id + ":" + signal_id + ":" +
        std::to_string(sequence) + ":" + message.topic;
    return observation;
}

void connection_lost(void *context, char *cause) {
    auto *state = static_cast<State *>(context);
    std::lock_guard lock(state->mutex);
    state->connection_error = cause == nullptr ? "MQTT connection lost" :
        "MQTT connection lost: " + std::string(cause);
}

int message_arrived(void *context, char *topic_name, int, MQTTClient_message *message) {
    auto *state = static_cast<State *>(context);
    Message copy;
    if (topic_name != nullptr) copy.topic = topic_name;
    if (message != nullptr && message->payload != nullptr && message->payloadlen > 0)
        copy.payload.assign(static_cast<const char *>(message->payload),
                            static_cast<std::size_t>(message->payloadlen));
    {
        std::lock_guard lock(state->mutex);
        const auto message_bytes = copy.topic.size() + copy.payload.size();
        if (message_bytes > state->max_pending_bytes ||
            state->pending_bytes > state->max_pending_bytes - message_bytes) {
            state->queue_overflow = true;
        } else {
            state->pending_bytes += message_bytes;
            state->messages.push_back(std::move(copy));
        }
    }
    if (message != nullptr) MQTTClient_freeMessage(&message);
    if (topic_name != nullptr) MQTTClient_free(topic_name);
    return 1;
}

std::unordered_map<std::string, std::string> topic_parameters(const Json &parameters) {
    const auto found = parameters.find("topics");
    if (found == parameters.end() || !found->is_object() || found->empty())
        throw std::invalid_argument("topics must be a non-empty object mapping signal names to MQTT topics");
    std::unordered_map<std::string, std::string> topics;
    for (const auto &[signal_id, topic] : found->items()) {
        if (signal_id.empty() || !topic.is_string() || topic.get<std::string>().empty())
            throw std::invalid_argument("topics must map non-empty signal names to non-empty topic strings");
        topics.emplace(signal_id, topic.get<std::string>());
    }
    return topics;
}

int subscribe_topics(MQTTClient client, const std::unordered_map<std::string, std::string> &topics,
                     int qos, std::string &failed_topic) {
    for (const auto &[signal_id, topic] : topics) {
        (void)signal_id;
        const auto status = MQTTClient_subscribe(client, topic.c_str(), qos);
        if (status != MQTTCLIENT_SUCCESS) {
            failed_topic = topic;
            return status;
        }
    }
    return MQTTCLIENT_SUCCESS;
}

}  // namespace mqtt_internal

nlohmann::json capture_mqtt_timeseries(sdk::PluginProjectAccess &access,
                                       const nlohmann::json &parameters) {
    using namespace mqtt_internal;
    const auto endpoint = string_parameter(parameters, "broker_endpoint");
    if (endpoint.empty()) throw std::invalid_argument("broker_endpoint is required");
    const auto topics = topic_parameters(parameters);
    const auto payload_format = string_parameter(parameters, "payload_format", "json");
    if (payload_format != "json" && payload_format != "plain")
        throw std::invalid_argument("payload_format must be json or plain");
    const auto qos = qos_parameter(parameters);
    const auto duration = positive_real_parameter(parameters, "capture_duration", 10.0, 3600.0);
    const auto max_samples = positive_integer_parameter(parameters, "max_samples", 1000, 1000000);
    const auto max_buffer_bytes = positive_integer_parameter(
        parameters, "max_buffer_bytes", 64u * 1024u * 1024u, 512u * 1024u * 1024u);
    const auto max_reconnect_attempts = non_negative_integer_parameter(
        parameters, "max_reconnect_attempts", 3, 32);
    const auto reconnect_backoff_seconds = non_negative_real_parameter(
        parameters, "reconnect_backoff_seconds", 1.0, 60.0);
    const std::string operation_instance(access.operation_instance());
    const auto capture_id = string_parameter(parameters, "capture_id", operation_instance);
    const auto client_id = string_parameter(
        parameters, "client_id", std::string("streamfind-") + operation_instance);
    const auto username = string_parameter(parameters, "mqtt_username");
    const auto password = string_parameter(parameters, "mqtt_password");
    const auto tls_ca_file = string_parameter(parameters, "tls_ca_file");
    const auto tls_client_cert_file = string_parameter(parameters, "tls_client_certificate");
    const auto tls_client_key_file = string_parameter(parameters, "tls_client_key");
    const auto tls_client_key_password = string_parameter(parameters, "tls_client_key_password");
    const auto tls_enabled = parameters.value("tls_enabled", false);
    const auto tls_verify_server = parameters.value("tls_verify_server", true);
    if (!tls_enabled && (endpoint.rfind("ssl://", 0) == 0 || endpoint.rfind("wss://", 0) == 0))
        throw std::invalid_argument("tls_enabled must be true for ssl:// or wss:// broker endpoints");
    if (tls_enabled && endpoint.rfind("ssl://", 0) != 0 && endpoint.rfind("wss://", 0) != 0)
        throw std::invalid_argument("TLS MQTT endpoints must use ssl:// or wss://");

    ClientHolder holder;
    State state;
    state.max_pending_bytes = max_buffer_bytes;
    auto status = MQTTClient_create(&holder.client, endpoint.c_str(), client_id.c_str(),
                                    MQTTCLIENT_PERSISTENCE_NONE, nullptr);
    if (status != MQTTCLIENT_SUCCESS) throw std::runtime_error(mqtt_error(status, "MQTT client creation"));
    status = MQTTClient_setCallbacks(holder.client, &state, &connection_lost,
                                     &message_arrived, nullptr);
    if (status != MQTTCLIENT_SUCCESS) throw std::runtime_error(mqtt_error(status, "MQTT callback setup"));

    MQTTClient_connectOptions connect_options = MQTTClient_connectOptions_initializer;
    connect_options.keepAliveInterval = 30;
    connect_options.cleansession = 1;
    connect_options.username = username.empty() ? nullptr : username.c_str();
    connect_options.password = password.empty() ? nullptr : password.c_str();
    MQTTClient_SSLOptions ssl_options = MQTTClient_SSLOptions_initializer;
    if (tls_enabled) {
        ssl_options.trustStore = tls_ca_file.empty() ? nullptr : tls_ca_file.c_str();
        ssl_options.keyStore = tls_client_cert_file.empty() ? nullptr : tls_client_cert_file.c_str();
        ssl_options.privateKey = tls_client_key_file.empty() ? nullptr : tls_client_key_file.c_str();
        ssl_options.privateKeyPassword = tls_client_key_password.empty() ? nullptr : tls_client_key_password.c_str();
        ssl_options.enableServerCertAuth = tls_verify_server ? 1 : 0;
        connect_options.ssl = &ssl_options;
    }
    status = MQTTClient_connect(holder.client, &connect_options);
    if (status != MQTTCLIENT_SUCCESS) throw std::runtime_error(mqtt_error(status, "MQTT connection"));
    std::string failed_topic;
    status = subscribe_topics(holder.client, topics, qos, failed_topic);
    if (status != MQTTCLIENT_SUCCESS)
        throw std::runtime_error(mqtt_error(status, "MQTT subscription to " + failed_topic));

    auto output = make_output_batch();
    std::size_t buffered_bytes = 0;
    std::uint64_t rows_emitted = 0;
    std::uint64_t sequence = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t reconnections = 0;
    std::size_t reconnect_attempts = 0;
    std::unordered_set<std::string> seen;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(duration));
    auto flush = [&]() {
        if (output.row_count() == 0) return;
        rows_emitted += output.row_count();
        output.emit(access, "sensorTimeseriesTable");
        output = make_output_batch();
        buffered_bytes = 0;
    };

    while (std::chrono::steady_clock::now() < deadline && rows_emitted < max_samples) {
        if (access.is_cancelled()) throw std::runtime_error("MQTT time-series capture cancelled");
        MQTTClient_yield();
        std::deque<Message> pending;
        std::string connection_error;
        bool queue_overflow = false;
        {
            std::lock_guard lock(state.mutex);
            pending.swap(state.messages);
            state.pending_bytes = 0;
            connection_error = state.connection_error;
            queue_overflow = state.queue_overflow;
            state.queue_overflow = false;
        }
        if (!connection_error.empty()) {
            if (reconnect_attempts >= max_reconnect_attempts)
                throw std::runtime_error(connection_error + "; reconnect attempts exhausted");
            ++reconnect_attempts;
            access.report_progress(std::min(1.0,
                                            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() /
                                                duration),
                                   "Reconnecting to MQTT broker (attempt " +
                                       std::to_string(reconnect_attempts) + ")");
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto backoff = std::chrono::duration<double>(reconnect_backoff_seconds);
            if (remaining > std::chrono::steady_clock::duration::zero() && backoff.count() > 0.0) {
                const auto remaining_seconds = std::chrono::duration<double>(remaining);
                std::this_thread::sleep_for(std::min(backoff, remaining_seconds));
            }
            if (std::chrono::steady_clock::now() >= deadline) break;
            status = MQTTClient_connect(holder.client, &connect_options);
            if (status == MQTTCLIENT_SUCCESS) {
                failed_topic.clear();
                status = subscribe_topics(holder.client, topics, qos, failed_topic);
            }
            if (status == MQTTCLIENT_SUCCESS) {
                {
                    std::lock_guard lock(state.mutex);
                    state.connection_error.clear();
                }
                ++reconnections;
                reconnect_attempts = 0;
            } else if (reconnect_attempts >= max_reconnect_attempts) {
                throw std::runtime_error(mqtt_error(status, failed_topic.empty()
                                                               ? "MQTT reconnect"
                                                               : "MQTT resubscription to " + failed_topic));
            }
            continue;
        }
        if (queue_overflow)
            throw std::runtime_error("MQTT pending message buffer exceeded max_buffer_bytes");
        for (const auto &message : pending) {
            const auto topic = std::find_if(topics.begin(), topics.end(), [&](const auto &entry) {
                return entry.second == message.topic;
            });
            if (topic == topics.end()) continue;
            const auto observation = decode_message(message, topic->first, endpoint, capture_id,
                                                    payload_format, sequence++);
            const auto fingerprint = observation.signal_id + "\n" + observation.event_time +
                "\n" + message.payload;
            if (!seen.insert(fingerprint).second) {
                ++duplicates;
                continue;
            }
            const auto row_bytes = observation_size(observation);
            if (row_bytes > max_buffer_bytes)
                throw std::runtime_error("single MQTT observation exceeds max_buffer_bytes");
            if (buffered_bytes != 0 && buffered_bytes + row_bytes > max_buffer_bytes) flush();
            const auto &value = observation.value;
            output.append_row({observation.source_id, observation.signal_id, observation.event_time,
                               observation.received_at, observation.time_origin,
                               std::string(value.kind == acquisition::SensorValueKind::Real ? "real" :
                               value.kind == acquisition::SensorValueKind::Integer ? "integer" :
                               value.kind == acquisition::SensorValueKind::Boolean ? "boolean" :
                               value.kind == acquisition::SensorValueKind::Text ? "string" :
                               value.kind == acquisition::SensorValueKind::Json ? "json" : "missing"),
                               value.real.has_value() ? Cell(*value.real) : Cell(std::monostate{}),
                               value.integer.has_value() ? Cell(*value.integer) : Cell(std::monostate{}),
                               value.boolean.has_value() ? Cell(*value.boolean) : Cell(std::monostate{}),
                               value.kind == acquisition::SensorValueKind::Text && value.text.has_value()
                                   ? Cell(*value.text) : Cell(std::monostate{}),
                               value.kind == acquisition::SensorValueKind::Json && value.text.has_value()
                                   ? Cell(*value.text) : Cell(std::monostate{}),
                               observation.unit.has_value() ? Cell(*observation.unit) : Cell(std::monostate{}),
                               observation.quality,
                               observation.status_code.has_value() ? Cell(*observation.status_code)
                                                                   : Cell(std::monostate{}),
                               observation.capture_id, observation.event_id});
            buffered_bytes += row_bytes;
            if (output.row_count() >= 1024 || rows_emitted + output.row_count() >= max_samples) {
                flush();
                if (rows_emitted >= max_samples) break;
            }
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        access.report_progress(std::min(1.0, elapsed / duration), "Capturing MQTT time-series observations");
    }
    flush();
    MQTTClient_disconnect(holder.client, 1000);
    access.report_progress(1.0, "MQTT time-series capture complete");
    return Json{{"broker_endpoint", endpoint}, {"capture_id", capture_id},
                {"rows_emitted", rows_emitted}, {"duplicates_skipped", duplicates},
                {"reconnections", reconnections},
                {"samples_limit", max_samples}, {"duration_seconds", duration},
                {"topics", static_cast<std::uint64_t>(topics.size())}};
}

}  // namespace streamfind::sensors::operations::detail

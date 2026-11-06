#include "streamfind/data_proxy_protocol.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const auto original = streamfind::data_proxy::request(
            "request-1", "session-1", "run-1", "mass_spec.find_features",
            "operation-6", "lease-1", {{"method", "lease_result_input"}, {"result_id", "result-features"}});
        const auto decoded = streamfind::data_proxy::Envelope::from_json(original.to_json());
        require(decoded.type == "request", "request type was not preserved");
        require(decoded.payload.at("method") == "lease_result_input", "request payload was not preserved");
        const auto success = streamfind::data_proxy::response(decoded, {{"rows", streamfind::data_proxy::Json::array()}});
        require(success.error_code.empty(), "success response unexpectedly has an error");
        const auto failed = streamfind::data_proxy::failure(decoded, "not_allowed", "table is not leased");
        require(failed.error_code == "not_allowed", "failure code was not preserved");
        const auto rows = streamfind::data_proxy::Json::array({
            streamfind::data_proxy::Json{{"id", 7}, {"ratio", 1.5}, {"enabled", true},
                                         {"name", "arrow"}, {"meta", {{"x", 1}}}, {"empty", nullptr}},
            streamfind::data_proxy::Json{{"id", 8}, {"ratio", 2}, {"enabled", false},
                                         {"name", "ipc"}, {"meta", {1, 2}}, {"empty", nullptr}}});
        const auto decoded_rows = streamfind::data_proxy::arrow_ipc_decode_rows(
            streamfind::data_proxy::arrow_ipc_encode_rows(rows));
        if (decoded_rows != rows)
            std::cerr << "expected: " << rows.dump() << " decoded: " << decoded_rows.dump() << '\n';
        require(decoded_rows == rows, "Arrow IPC row batch did not round-trip");
        const auto list_rows = streamfind::data_proxy::Json::array({
            streamfind::data_proxy::Json{{"trace", {1.25, 2.5}}},
            streamfind::data_proxy::Json{{"trace", streamfind::data_proxy::Json::array()}},
            streamfind::data_proxy::Json{{"trace", nullptr}}});
        const auto decoded_lists = streamfind::data_proxy::arrow_ipc_decode_rows(
            streamfind::data_proxy::arrow_ipc_encode_rows(
                list_rows, streamfind::data_proxy::Json::array({
                    streamfind::data_proxy::Json{{"name", "trace"}, {"type", "array"},
                        {"items", {{"type", "number"}}}}})));
        require(decoded_lists == list_rows,
                "Arrow numeric lists did not preserve values, empty lists, and nulls");
        const auto schema_rows = streamfind::data_proxy::Json::array({
            streamfind::data_proxy::Json{{"first", 7}}});
        const auto schema_decoded = streamfind::data_proxy::arrow_ipc_decode_rows(
            streamfind::data_proxy::arrow_ipc_encode_rows(
                schema_rows, streamfind::data_proxy::Json::array({
                    streamfind::data_proxy::Json{{"name", "declared_first"}, {"type", "integer"}},
                    streamfind::data_proxy::Json{{"name", "first"}, {"type", "integer"}}})));
        require(schema_decoded.size() == 1 && schema_decoded[0].contains("declared_first") &&
                    schema_decoded[0]["declared_first"].is_null() && schema_decoded[0]["first"] == 7,
                "Arrow output did not preserve declared columns that are null in a row");
        bool rejected = false;
        try { (void)streamfind::data_proxy::Envelope::from_json({{"protocol", "old"}}); }
        catch (const std::exception &) { rejected = true; }
        require(rejected, "unsupported protocol was accepted");
        std::cout << "data proxy protocol contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

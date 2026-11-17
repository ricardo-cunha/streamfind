#include "streamfind/data_proxy_protocol.hpp"

#include <iostream>
#include <stdexcept>

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

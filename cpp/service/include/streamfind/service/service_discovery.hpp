#pragma once

#include "streamfind/project.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace streamfind::service {

struct ServiceDiscoveryRecord {
    std::string service_instance_id;
    std::string endpoint;
    std::string protocol_version;
    std::string state{"starting"};
    std::string token;
    std::string executable;
    std::uint64_t process_id{0};
    std::string started_at;
    std::filesystem::path record_path;

    Json to_json() const;
};

class ServiceDiscovery {
public:
    static std::filesystem::path runtime_directory();
    static std::vector<std::filesystem::path> record_paths();
    static ServiceDiscoveryRecord read(const std::filesystem::path &path);

    ServiceDiscoveryRecord publish(std::uint16_t port, const std::string &protocol_version);
    void update_state(const std::string &state);
    void remove() noexcept;

    const ServiceDiscoveryRecord &record() const noexcept { return record_; }

private:
    static std::string create_id();
    static std::string create_token();
    static std::string timestamp();
    static std::uint64_t current_process_id();
    static void write_record(const ServiceDiscoveryRecord &record);

    ServiceDiscoveryRecord record_;
};

}  // namespace streamfind::service

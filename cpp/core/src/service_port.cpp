#include "streamfind/service_port.hpp"

#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace streamfind::detail {

bool loopback_port_available(std::uint16_t port) {
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
        throw std::runtime_error("Winsock initialization failed while selecting a service port");
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    const auto listener = socket(AF_INET, SOCK_STREAM, 0);
#endif
#ifdef _WIN32
    if (listener == INVALID_SOCKET) {
#else
    if (listener < 0) {
#endif
#ifdef _WIN32
        WSACleanup();
#endif
        throw std::runtime_error("unable to create a socket while selecting a service port");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
#ifdef _WIN32
    const auto available = bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    closesocket(listener);
    WSACleanup();
#else
    const auto available = bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    close(listener);
#endif
    return available;
}

}  // namespace streamfind::detail

namespace streamfind {

std::uint16_t select_service_port(const std::uint16_t preferred, const std::uint16_t attempts) {
    if (preferred == 0) throw std::invalid_argument("preferred service port must be between 1 and 65535");
    for (std::uint32_t offset = 0; offset < attempts; ++offset) {
        const auto candidate = static_cast<std::uint16_t>(
            ((static_cast<std::uint32_t>(preferred) - 1 + offset) % 65535) + 1);
        if (detail::loopback_port_available(candidate)) return candidate;
    }
    throw std::runtime_error("no available loopback service port in the configured fallback range");
}

}  // namespace streamfind

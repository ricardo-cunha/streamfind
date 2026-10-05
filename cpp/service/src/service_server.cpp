#include "streamfind/service/service_server.hpp"
#include "streamfind/service/service_protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windows.h>
using streamfind_socket_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using streamfind_socket_t = int;
#endif

namespace streamfind::service {
namespace detail {

#ifdef _WIN32
std::string pick_database_file(bool create) {
    const auto owner = GetForegroundWindow();
    wchar_t filename[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = filename;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"DuckDB files\0*.duckdb;*.db\0All files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_EXPLORER;
    if (create) {
        dialog.Flags |= OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
        dialog.lpstrDefExt = L"duckdb";
        if (!GetSaveFileNameW(&dialog)) throw std::invalid_argument("database file selection cancelled");
    } else {
        dialog.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) throw std::invalid_argument("database file selection cancelled");
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, filename, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string utf8(const std::wstring &value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::vector<std::string> pick_files(const std::vector<std::string> &extensions) {
    const auto wildcard = [](const std::string &extension) {
        return (extension.starts_with('.') ? "*" : "*.") + extension;
    };
    std::wstring filter = L"Supported files (";
    for (std::size_t index = 0; index < extensions.size(); ++index) {
        if (index) filter += L", ";
        const auto pattern = wildcard(extensions[index]);
        filter += std::wstring(pattern.begin(), pattern.end());
    }
    filter += L")";
    filter.push_back(L'\0');
    for (const auto &extension : extensions) {
        if (filter.back() != L'\0') filter += L';';
        const auto pattern = wildcard(extension);
        filter += std::wstring(pattern.begin(), pattern.end());
    }
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    std::vector<wchar_t> buffer(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetForegroundWindow();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter = filter.c_str();
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) throw std::invalid_argument("file selection cancelled");

    std::vector<std::string> result;
    const wchar_t *value = buffer.data();
    const std::wstring first(value);
    value += first.size() + 1;
    if (*value == L'\0') {
        result.push_back(utf8(first));
        return result;
    }
    while (*value != L'\0') {
        result.push_back(utf8(first + L"\\" + value));
        value += std::wcslen(value) + 1;
    }
    return result;
}

std::vector<std::string> pick_folders() {
    BROWSEINFOW dialog{};
    dialog.hwndOwner = GetForegroundWindow();
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    const auto item = SHBrowseForFolderW(&dialog);
    if (!item) throw std::invalid_argument("folder selection cancelled");
    wchar_t path[MAX_PATH]{};
    const auto resolved = SHGetPathFromIDListW(item, path);
    CoTaskMemFree(item);
    if (!resolved) throw std::invalid_argument("selected folder path could not be resolved");
    return {utf8(path)};
}

std::vector<std::string> pick_paths(const std::vector<std::string> &extensions) {
    const auto choice = MessageBoxW(
        GetForegroundWindow(),
        L"Choose mass-spectrometry files, or choose a vendor folder such as an Agilent .d directory.",
        L"Choose analysis paths",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (choice == IDYES) return pick_files(extensions);
    if (choice == IDNO) return pick_folders();
    throw std::invalid_argument("path selection cancelled");
}
#endif

Json browse_file_system(const std::string &requested_path) {
#ifdef _WIN32
    if (requested_path.empty()) {
        Json entries = Json::array();
        const auto drives = GetLogicalDrives();
        for (unsigned int index = 0; index < 26; ++index) {
            if ((drives & (1u << index)) == 0) continue;
            const std::string drive{static_cast<char>('A' + index), ':', '\\'};
            entries.push_back(Json{{"name", drive}, {"path", drive}, {"isDirectory", true}});
        }
        return Json{{"path", ""}, {"entries", std::move(entries)}};
    }
#endif
    const auto root = requested_path.empty() ? std::filesystem::current_path() : std::filesystem::path(requested_path);
    if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root))
        throw std::invalid_argument("filesystem browse path is not a directory");
    Json entries = Json::array();
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        const auto status = entry.symlink_status();
        if (std::filesystem::is_symlink(status)) continue;
        const bool directory = std::filesystem::is_directory(status);
        Json item{{"name", entry.path().filename().string()},
                  {"path", entry.path().string()},
                  {"isDirectory", directory}};
        if (!directory && std::filesystem::is_regular_file(status)) item["size"] = std::filesystem::file_size(entry.path());
        entries.push_back(std::move(item));
    }
    std::sort(entries.begin(), entries.end(), [](const Json &left, const Json &right) {
        if (left.at("isDirectory") != right.at("isDirectory")) return left.at("isDirectory").get<bool>();
        return left.at("name").get<std::string>() < right.at("name").get<std::string>();
    });
    return Json{{"path", root.string()}, {"entries", std::move(entries)}};
}
void close_socket(std::intptr_t socket) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket));
#else
    close(static_cast<int>(socket));
#endif
}

std::string header_value(const std::string &headers, std::string_view name) {
    std::string lowered_headers = headers;
    std::string lowered_name(name);
    for (auto &character : lowered_headers) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    for (auto &character : lowered_name) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    const auto start = lowered_headers.find(lowered_name + ":");
    if (start == std::string::npos) return {};
    const auto value_start = headers.find_first_not_of(" \t", start + name.size() + 1);
    const auto end = headers.find("\r\n", value_start);
    return headers.substr(value_start, end - value_start);
}

std::string percent_decode(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto digit = [](char character) -> int {
                if (character >= '0' && character <= '9') return character - '0';
                if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                return -1;
            };
            const auto high = digit(value[index + 1]);
            const auto low = digit(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    return result;
}

std::string base64(const std::array<unsigned char, 20> &input) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto a = input[i];
        const auto b = i + 1 < input.size() ? input[i + 1] : 0;
        const auto c = i + 2 < input.size() ? input[i + 2] : 0;
        result += alphabet[a >> 2];
        result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < input.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < input.size() ? alphabet[c & 63] : '=';
    }
    return result;
}

std::array<unsigned char, 20> sha1(std::string_view input) {
    std::vector<unsigned char> data(input.begin(), input.end());
    const auto bits = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<unsigned char>(bits >> (i * 8)));
    std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<std::uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i) w[i] = (data[offset + i * 4] << 24) | (data[offset + i * 4 + 1] << 16) | (data[offset + i * 4 + 2] << 8) | data[offset + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        auto a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            const auto f = i < 20 ? ((b & c) | ((~b) & d)) : i < 40 ? (b ^ c ^ d) : i < 60 ? ((b & c) | (b & d) | (c & d)) : (b ^ c ^ d);
            const auto k = i < 20 ? 0x5A827999u : i < 40 ? 0x6ED9EBA1u : i < 60 ? 0x8F1BBCDCu : 0xCA62C1D6u;
            const auto temp = std::rotl(a, 5) + f + e + k + w[i]; e = d; d = c; c = std::rotl(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::array<unsigned char, 20> result{};
    const std::array<std::uint32_t, 5> words{h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) result[i * 4 + j] = static_cast<unsigned char>(words[i] >> (24 - j * 8));
    return result;
}

void send_all(std::intptr_t socket, const std::string &data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const auto count = send(static_cast<SOCKET>(socket), data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        const auto count = send(static_cast<int>(socket), data.data() + sent, data.size() - sent, 0);
#endif
        if (count <= 0) throw std::runtime_error("socket send failed");
        sent += static_cast<std::size_t>(count);
    }
}

void send_http(std::intptr_t socket, int status, const Json &body) {
    const auto text = body.dump();
    send_all(socket, "HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(text.size()) + "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n" + text);
}

void send_file(std::intptr_t socket, const std::filesystem::path &root, const std::string &request_path) {
    const auto relative = request_path == "/" ? std::filesystem::path("index.html") : std::filesystem::path(request_path.substr(1));
    if (relative.empty() || relative.string().find("..") != std::string::npos) {
        send_http(socket, 404, Json{{"error", "application asset not found"}});
        return;
    }
    const auto canonical_root = std::filesystem::weakly_canonical(root);
    const auto file = std::filesystem::weakly_canonical(root / relative);
    if (!std::filesystem::exists(file) || file.string().rfind(canonical_root.string(), 0) != 0) {
        send_http(socket, 404, Json{{"error", "application asset not found"}});
        return;
    }
    std::ifstream input(file, std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto extension = file.extension().string();
    const auto content_type = extension == ".html" ? "text/html; charset=utf-8" : extension == ".js" ? "text/javascript; charset=utf-8" : extension == ".css" ? "text/css; charset=utf-8" : extension == ".png" ? "image/png" : extension == ".svg" ? "image/svg+xml" : "application/octet-stream";
    send_all(socket, "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(content_type) + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" + body);
}

std::string websocket_frame(const std::string &payload, unsigned char opcode = 0x1) {
    std::string frame{static_cast<char>(0x80 | opcode)};
    if (payload.size() < 126) frame.push_back(static_cast<char>(payload.size()));
    else if (payload.size() <= 65535) { frame.push_back(126); frame.push_back(static_cast<char>(payload.size() >> 8)); frame.push_back(static_cast<char>(payload.size())); }
    else throw std::runtime_error("event payload too large");
    return frame + payload;
}

bool receive_exact(std::intptr_t socket, unsigned char *buffer, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        const auto count = recv(static_cast<SOCKET>(socket), reinterpret_cast<char *>(buffer + received), static_cast<int>(size - received), 0);
#else
        const auto count = recv(static_cast<int>(socket), buffer + received, size - received, 0);
#endif
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace detail

void EventBroker::add(std::intptr_t socket) { std::lock_guard lock(mutex_); clients_.push_back(socket); }
void EventBroker::remove(std::intptr_t socket) { std::lock_guard lock(mutex_); clients_.erase(std::remove(clients_.begin(), clients_.end(), socket), clients_.end()); }
void EventBroker::publish(const Json &event) {
    std::lock_guard lock(mutex_);
    for (const auto socket : clients_) {
        try { detail::send_all(socket, detail::websocket_frame(event.dump())); } catch (...) {}
    }
}

ServiceServer::ServiceServer(std::uint16_t port, const std::filesystem::path &configuration_path,
    const std::filesystem::path &application_root)
    : port_(port), application_root_(application_root), projects_(operations_) {
    projects_.set_operation_log_callback([this](const std::string &session_id, std::string_view message) {
        events_.publish(Json{{"type", "operation.log"}, {"project", session_id},
                             {"payload", Json{{"level", "info"}, {"message", std::string(message)}}}});
    });
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Winsock initialization failed");
#endif
    plugin_runtime_.load(configuration_path, methods_, operations_);
}
ServiceServer::~ServiceServer() { stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

void ServiceServer::stop() { stopping_ = true; if (listener_ != -1) detail::close_socket(listener_); }

void ServiceServer::handle_client(std::intptr_t socket) {
    try {
        std::string request;
        std::array<char, 8192> buffer{};
        for (;;) {
#ifdef _WIN32
            const auto count = recv(static_cast<SOCKET>(socket), buffer.data(), static_cast<int>(buffer.size()), 0);
#else
            const auto count = recv(static_cast<int>(socket), buffer.data(), buffer.size(), 0);
#endif
            if (count <= 0) break;
            request.append(buffer.data(), static_cast<std::size_t>(count));
            const auto header_end = request.find("\x0d\x0a\x0d\x0a");
            if (header_end == std::string::npos) continue;
            std::size_t content_length = 0;
            const auto content_length_text = detail::header_value(
                request.substr(0, header_end), "Content-Length");
            if (!content_length_text.empty()) {
                try { content_length = std::stoull(content_length_text); }
                catch (...) { throw std::runtime_error("invalid Content-Length header"); }
            }
            if (request.size() >= header_end + 4 + content_length) break;
        }
        const auto separator = request.find("\r\n\r\n");
        if (separator == std::string::npos) throw std::runtime_error("invalid HTTP request");
        const auto headers = request.substr(0, separator);
        const auto first_end = headers.find("\r\n");
        const auto first = headers.substr(0, first_end);
        std::istringstream line(first);
        std::string method, path, version;
        line >> method >> path >> version;
        const auto query_marker = path.find('?');
        const auto route = query_marker == std::string::npos ? path : path.substr(0, query_marker);
        const auto query = query_marker == std::string::npos ? std::string{} : path.substr(query_marker + 1);
        const auto query_value = [&](const std::string &name) {
            const auto prefix = name + "=";
            std::size_t begin = 0;
            while (begin < query.size()) {
                const auto end = query.find('&', begin);
                const auto part = query.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                if (part.rfind(prefix, 0) == 0) return detail::percent_decode(part.substr(prefix.size()));
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            return std::string{};
        };
        if (std::filesystem::exists(application_root_)) {
            const auto host = detail::header_value(headers, "Host");
            const auto origin = detail::header_value(headers, "Origin");
            const auto local_origin = std::string("http://127.0.0.1:") + std::to_string(port_);
            const auto branded_origin = std::string("http://streamfind.localhost:") + std::to_string(port_);
            const auto is_loopback_origin = origin.rfind("http://127.0.0.1:", 0) == 0 ||
                origin.rfind("http://localhost:", 0) == 0 ||
                origin.rfind("http://streamfind.localhost:", 0) == 0;
            if (host != std::string("127.0.0.1:") + std::to_string(port_) && host != std::string("streamfind.localhost:") + std::to_string(port_)) {
                detail::send_http(socket, 403, Json{{"error", "invalid StreamFind host"}});
                return;
            }
            if (!origin.empty() && origin != local_origin && origin != branded_origin &&
                !is_loopback_origin) {
                detail::send_http(socket, 403, Json{{"error", "invalid StreamFind origin"}});
                return;
            }
        }
        if (path == "/events" && detail::header_value(headers, "Upgrade") == "websocket") {
            const auto key = detail::header_value(headers, "Sec-WebSocket-Key");
            const auto accept = detail::base64(detail::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
            detail::send_all(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
            events_.add(socket);
            for (;;) {
                unsigned char frame_header[2]{};
                if (!detail::receive_exact(socket, frame_header, sizeof(frame_header))) break;
                const auto opcode = static_cast<unsigned char>(frame_header[0] & 0x0F);
                const auto masked = (frame_header[1] & 0x80) != 0;
                std::uint64_t payload_size = frame_header[1] & 0x7F;
                if (payload_size == 126) {
                    unsigned char extended[2]{};
                    if (!detail::receive_exact(socket, extended, sizeof(extended))) break;
                    payload_size = (static_cast<std::uint64_t>(extended[0]) << 8) | extended[1];
                } else if (payload_size == 127) {
                    unsigned char extended[8]{};
                    if (!detail::receive_exact(socket, extended, sizeof(extended))) break;
                    payload_size = 0;
                    for (const auto byte : extended) payload_size = (payload_size << 8) | byte;
                }
                if (payload_size > 1024 * 1024) break;
                unsigned char mask[4]{};
                if (masked && !detail::receive_exact(socket, mask, sizeof(mask))) break;
                std::vector<unsigned char> payload(static_cast<std::size_t>(payload_size));
                if (!payload.empty() && !detail::receive_exact(socket, payload.data(), payload.size())) break;
                if (masked) for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
                if (opcode == 0x8) {
                    detail::send_all(socket, detail::websocket_frame(std::string("\x03\xE8", 2), 0x8));
                    break;
                }
                if (opcode == 0x9) detail::send_all(socket, detail::websocket_frame(std::string(payload.begin(), payload.end()), 0xA));
            }
            events_.remove(socket);
        } else {
            const auto body_start = separator + 4;
            const auto body = body_start < request.size() ? request.substr(body_start) : std::string{};
            if (method == "OPTIONS") detail::send_http(socket, 204, Json::object());
            else if (method == "GET" && std::filesystem::exists(application_root_) && (path == "/" || path.rfind("/assets/", 0) == 0)) detail::send_file(socket, application_root_, path);
            else if (method == "GET" && path == "/session") detail::send_http(socket, 200, SessionDto{});
            else if (method == "GET" && path == "/dependencies")
                detail::send_http(socket, 200, Json{{"dependencies", plugin_runtime_.dependencies()}});
            else if (method == "POST" && path == "/dependencies/install") {
                const auto request = Json::parse(body);
                if (!request.value("allow_network", false))
                    throw std::invalid_argument("dependency installation requires allow_network=true");
                detail::send_http(socket, 200, plugin_runtime_.install_dependencies(request));
            }
            else if (method == "GET" && route == "/capabilities/index") detail::send_http(socket, 200, capability_index_json());
            else if (method == "GET" && route.rfind("/capabilities/domains/", 0) == 0 && route.ends_with("/modules")) {
                const auto domain = detail::percent_decode(route.substr(std::string("/capabilities/domains/").size(), route.size() - std::string("/capabilities/domains/").size() - std::string("/modules").size()));
                Json modules = Json::array();
                const auto entries = catalogue::entries_json();
                std::set<std::string> unique;
                if (entries) for (const auto &entry : *entries)
                    if (entry.value("kind", "") == "operation" && entry.value("exposed", false) && entry.value("executable", false) && entry.value("domain", "") == domain)
                        unique.insert(entry.value("module_id", ""));
                for (const auto &module : unique) modules.push_back(module);
                detail::send_http(socket, 200, Json{{"domain", domain}, {"modules", modules}});
            }
            else if (method == "GET" && route == "/capabilities/operations") {
                const auto domain = query_value("domain");
                if (domain.empty()) detail::send_http(socket, 400, Json{{"error", "domain is required"}});
                else detail::send_http(socket, 200, Json{{"operations", capability_operations_json(domain, query_value("module"), query_value("search"), query_value("include_schema") == "true")} });
            }
            else if (method == "GET" && route.rfind("/capabilities/operations/", 0) == 0) {
                const auto operation_id = detail::percent_decode(route.substr(std::string("/capabilities/operations/").size()));
                const auto entries = catalogue::entries_json();
                bool found = false;
                if (entries) for (const auto &entry : *entries)
                    if (entry.value("kind", "") == "operation" && entry.value("exposed", false) && entry.value("canonical_id", "") == operation_id) {
                        detail::send_http(socket, 200, entry);
                        found = true;
                        break;
                    }
                if (!found) detail::send_http(socket, 404, Json{{"error", "operation not found"}, {"operation", operation_id}});
            }
            else if (method == "GET" && path == "/capabilities") detail::send_http(socket, 200, capabilities_json());
            else if (method == "GET" && path == "/projects") {
                Json result = Json::array(); for (const auto &project : projects_.list()) result.push_back(project); detail::send_http(socket, 200, Json{{"projects", result}});
            } else if (method == "POST" && path == "/projects/file-picker/files") {
#ifdef _WIN32
                const auto input = Json::parse(body);
                std::vector<std::string> extensions;
                for (const auto &extension : input.value("extensions", Json::array())) extensions.push_back(extension.get<std::string>());
                if (extensions.empty()) throw std::invalid_argument("at least one file extension is required");
                detail::send_http(socket, 200, Json{{"paths", detail::pick_files(extensions)}});
#else
                detail::send_http(socket, 501, Json{{"error", "native file picker is only available on Windows"}});
#endif
            } else if (method == "POST" && path == "/projects/file-picker/browse") {
                const auto input = Json::parse(body);
                detail::send_http(socket, 200, detail::browse_file_system(input.value("path", std::string{})));
            } else if (method == "POST" && path == "/projects/file-picker/folders") {
#ifdef _WIN32
                detail::send_http(socket, 200, Json{{"paths", detail::pick_folders()}});
#else
                detail::send_http(socket, 501, Json{{"error", "native folder picker is only available on Windows"}});
#endif
            } else if (method == "POST" && path == "/projects/file-picker/paths") {
#ifdef _WIN32
                const auto input = Json::parse(body);
                std::vector<std::string> extensions;
                for (const auto &extension : input.value("extensions", Json::array())) extensions.push_back(extension.get<std::string>());
                if (extensions.empty()) throw std::invalid_argument("at least one file extension is required");
                detail::send_http(socket, 200, Json{{"paths", detail::pick_paths(extensions)}});
#else
                detail::send_http(socket, 501, Json{{"error", "native path picker is only available on Windows"}});
#endif
            } else if (method == "GET" && (path == "/projects/file-picker" || path == "/projects/file-picker/create")) {
#ifdef _WIN32
                detail::send_http(socket, 200, Json{{"database_path", detail::pick_database_file(path.ends_with("/create"))}});
#else
                detail::send_http(socket, 501, Json{{"error", "native file picker is only available on Windows"}});
#endif
            } else if (method == "GET" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow") && !path.ends_with("/workflow/state")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/workflow");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, projects_.workflow_definition(session_id));
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow/history/clear")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/workflow/history/clear");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, projects_.clear_workflow_history(session_id));
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/artifacts/cache/clear")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/artifacts/cache/clear");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, projects_.clear_artifact_cache(session_id));
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/artifacts/clear")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/artifacts/clear");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, projects_.clear_all_artifacts(session_id));
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow") && !path.ends_with("/workflow/state")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/workflow");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, projects_.save_workflow(session_id, Json::parse(body)));
            } else if (method == "GET" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow/state")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/workflow/state");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, projects_.workflow_snapshot(session_id));
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/artifacts/data")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/artifacts/data");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, projects_.artifact_data(session_id, Json::parse(body)));
            } else if (method == "GET" && path.rfind("/projects/", 0) == 0 && path.ends_with("/artifacts")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/artifacts");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, Json{{"artifacts", projects_.artifact_inventory(session_id)}});
            } else if (method == "GET" && path.rfind("/projects/", 0) == 0 && path.ends_with("/artifacts/current")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/artifacts/current");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                detail::send_http(socket, 200, Json{{"artifacts", projects_.current_artifact_inventory(session_id)}});
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow/validate")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/workflow/validate");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                const auto request = Json::parse(body);
                const auto validation = projects_.validate_workflow(
                    session_id, request.contains("workflow") ? request.at("workflow") : Json(nullptr));
                detail::send_http(socket, 200, validation);
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow/run")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/workflow/run");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                const auto state = projects_.start_workflow(session_id);
                events_.publish(Json{{"type", "workflow.started"}, {"project", session_id},
                                     {"payload", Json{{"message", "Workflow execution started."}}}});
                detail::send_http(socket, 202, Json{{"session_id", session_id}, {"state", state}});
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow/cancel")) {
                const auto prefix = std::string("/projects/");
                const auto suffix = std::string("/workflow/cancel");
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
                const auto state = projects_.cancel_workflow(session_id);
                events_.publish(Json{{"type", "workflow.cancelled"}, {"project", session_id},
                                     {"payload", Json{{"message", "Workflow cancellation requested."}}}});
                detail::send_http(socket, 200, Json{{"session_id", session_id}, {"state", state}});
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.ends_with("/workflow/pause")) {
                detail::send_http(socket, 501, Json{{"error", "workflow pause is not supported by the current execution engine"}});
            } else if (method == "POST" && path.rfind("/projects/", 0) == 0 && path.find("/operations/") != std::string::npos) {
                const auto prefix = std::string("/projects/");
                const auto marker = std::string("/operations/");
                const auto marker_position = path.find(marker, prefix.size());
                const auto session_id = detail::percent_decode(path.substr(prefix.size(), marker_position - prefix.size()));
                const auto operation_id = detail::percent_decode(path.substr(marker_position + marker.size()));
                events_.publish(Json{{"type", "operation.started"}, {"project", session_id},
                                     {"operation_id", operation_id},
                                     {"payload", Json{{"message", "Operation execution started."}}}});
                const auto request = Json::parse(body);
                const auto parameters = request.contains("parameters")
                    ? request.at("parameters")
                    : request;
                const auto operation_instance = request.value("operation_instance", operation_id);
                Json result;
                try {
                    result = projects_.run_operation(session_id, operation_id, parameters, operation_instance);
                } catch (const std::exception &error) {
                    events_.publish(Json{{"type", "operation.failed"}, {"project", session_id},
                                         {"operation_id", operation_id},
                                         {"payload", Json{{"message", error.what()}}}});
                    throw;
                }
                // Operations may legitimately return an array of published artifacts.
                // Only object results can carry the optional cache_hit flag.
                const bool cache_hit = result.is_object() && result.value("cache_hit", false);
                events_.publish(Json{{"type", "operation.completed"}, {"project", session_id},
                                     {"operation_id", operation_id},
                                     {"payload", Json{{"message", cache_hit
                                         ? "Operation reused cached artifacts; execution skipped."
                                         : "Operation execution completed."},
                                                       {"cache_hit", cache_hit}}}});
                detail::send_http(socket, 200, Json{{"session_id", session_id}, {"operation_id", operation_id}, {"result", result}});
            } else if (method == "DELETE" && path.rfind("/projects/", 0) == 0) {
                const auto session_id = detail::percent_decode(path.substr(std::string("/projects/").size()));
                if (session_id.empty()) throw std::invalid_argument("project session ID is required");
                const auto project = projects_.close(session_id);
                events_.publish(Json{{"type", "project.closed"}, {"project", project}});
                detail::send_http(socket, 200, project);
            } else if (method == "POST" && path == "/projects") {
                const auto input = Json::parse(body);
                ProjectOptions options{input.at("database_path").get<std::string>(), input.value("metadata", Json::object())};
                const auto session_id = input.at("session_id").get<std::string>();
                const auto project = input.value("mode", "create") == "open"
                                         ? projects_.open(session_id, options)
                                         : projects_.create(session_id, options);
                const auto event_type = input.value("mode", "create") == "open" ? "project.opened" : "project.created";
                Json response = project;
                response["initialization"] = project_initialization_json(project.domains);
                events_.publish(Json{{"type", event_type}, {"project", response}});
                detail::send_http(socket, 201, response);
            } else detail::send_http(socket, 404, Json{{"error", "endpoint not found"}});
        }
    } catch (const std::exception &error) { try { detail::send_http(socket, 400, Json{{"error", error.what()}}); } catch (...) {} }
    detail::close_socket(socket);
}

void ServiceServer::run() {
    listener_ = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener_ == -1) throw std::runtime_error("unable to create service socket");
    int reuse = 1;
    setsockopt(static_cast<streamfind_socket_t>(listener_), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port_);
    if (bind(static_cast<streamfind_socket_t>(listener_), reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || listen(static_cast<streamfind_socket_t>(listener_), 16) < 0) throw std::runtime_error("unable to bind service socket");
    while (!stopping_) {
        sockaddr_in client{};
#ifdef _WIN32
        int length = sizeof(client); const auto client_socket = ::accept(static_cast<SOCKET>(listener_), reinterpret_cast<sockaddr *>(&client), &length);
#else
        socklen_t length = sizeof(client); const auto client_socket = ::accept(static_cast<int>(listener_), reinterpret_cast<sockaddr *>(&client), &length);
#endif
        if (client_socket == static_cast<streamfind_socket_t>(-1)) { if (!stopping_) continue; break; }
        std::thread(&ServiceServer::handle_client, this, static_cast<std::intptr_t>(client_socket)).detach();
    }
}

}  // namespace streamfind::service

#include "utils/tools_resolver.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace fs = std::filesystem;
using namespace streamfind::mass_spec::tools;

static void put16(std::ostream& out, uint16_t value) {
    out.put(static_cast<char>(value)); out.put(static_cast<char>(value >> 8));
}
static void put32(std::ostream& out, uint32_t value) {
    put16(out, static_cast<uint16_t>(value)); put16(out, static_cast<uint16_t>(value >> 16));
}
static void write_jar(const fs::path& path, const std::string& version, bool deflate_entry = false) {
    const std::string name = "META-INF/maven/org.streamfind.tools/streamfind-metfrag-fragmenter/pom.properties";
    const std::string data = "groupId=org.streamfind.tools\nartifactId=streamfind-metfrag-fragmenter\nversion=" + version + "\n";
    const auto crc = crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    std::string compressed = data;
    if (deflate_entry) {
        compressed.resize(compressBound(static_cast<uLong>(data.size())));
        z_stream stream{};
        if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw std::runtime_error("Could not initialize the ZIP test compressor.");
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_out = reinterpret_cast<Bytef*>(compressed.data());
        stream.avail_out = static_cast<uInt>(compressed.size());
        const auto status = deflate(&stream, Z_FINISH);
        const auto compressed_size = stream.total_out;
        deflateEnd(&stream);
        if (status != Z_STREAM_END) throw std::runtime_error("Could not create the deflated ZIP fixture.");
        compressed.resize(compressed_size);
    }
    const auto method = static_cast<uint16_t>(deflate_entry ? 8 : 0);
    std::ofstream out(path, std::ios::binary);
    put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, method); put16(out, 0); put16(out, 0);
    put32(out, crc); put32(out, static_cast<uint32_t>(compressed.size())); put32(out, static_cast<uint32_t>(data.size()));
    put16(out, static_cast<uint16_t>(name.size())); put16(out, 0); out.write(name.data(), name.size()); out.write(compressed.data(), compressed.size());
    const auto central_offset = static_cast<uint32_t>(out.tellp());
    put32(out, 0x02014b50); put16(out, 20); put16(out, 20); put16(out, 0); put16(out, method);
    put16(out, 0); put16(out, 0); put32(out, crc); put32(out, static_cast<uint32_t>(compressed.size()));
    put32(out, static_cast<uint32_t>(data.size())); put16(out, static_cast<uint16_t>(name.size()));
    put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0); put32(out, 0); put32(out, 0);
    out.write(name.data(), name.size());
    const auto central_size = static_cast<uint32_t>(static_cast<std::streamoff>(out.tellp()) - central_offset);
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, 1); put16(out, 1);
    put32(out, central_size); put32(out, central_offset); put16(out, 0);
}

static void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
static void clear_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

int main() {
    const auto scratch = fs::current_path() / "metfrag-installer-contract";
    fs::remove_all(scratch);
    fs::create_directories(scratch / "tools" / "metfrag");
    set_env("STREAMFIND_HOME", scratch.string());
    clear_env("STREAMFIND_METFRAG_FRAGMENTER_JAR");
    const auto installed = scratch / "tools" / "metfrag" /
        ("streamfind-metfrag-fragmenter-" + std::string(kMetFragFragmenterVersion) + ".jar");
    const auto legacy_installed = scratch / "tools" / "metfrag" / "streamfind-metfrag-fragmenter.jar";
    write_jar(legacy_installed, "0.1.0-SNAPSHOT");
    if (resolve_metfrag_fragmenter_jar())
        throw std::runtime_error("The stale unversioned fragmenter JAR was selected.");
    write_jar(installed, "0.1.0-SNAPSHOT");
    if (detail::metfrag_fragmenter_jar_version(installed.string()) != "0.1.0-SNAPSHOT")
        throw std::runtime_error("Embedded Maven version was not read from a stored JAR entry.");
    if (resolve_metfrag_fragmenter_jar())
        throw std::runtime_error("A stale fragmenter JAR was treated as current.");
    write_jar(installed, std::string(kMetFragFragmenterVersion), true);
    if (!resolve_metfrag_fragmenter_jar())
        throw std::runtime_error("The pinned fragmenter version in a deflated ZIP entry was not recognized.");

    const auto override_jar = scratch / "override.jar";
    write_jar(override_jar, "0.1.0-SNAPSHOT");
    set_env("STREAMFIND_METFRAG_FRAGMENTER_JAR", override_jar.string());
    if (resolve_metfrag_fragmenter_jar())
        throw std::runtime_error("A stale explicit fragmenter override was accepted.");
    clear_env("STREAMFIND_METFRAG_FRAGMENTER_JAR");

    const auto staged_jar = scratch / "candidate.jar";
    const auto stale_destination = scratch / "stale-destination.jar";
    write_jar(stale_destination, "0.1.0-SNAPSHOT");
    write_jar(staged_jar, "0.1.0-SNAPSHOT");
    bool stale_candidate_rejected = false;
    try { detail::promote_metfrag_fragmenter_jar(staged_jar.string(), stale_destination.string()); }
    catch (const std::runtime_error&) { stale_candidate_rejected = true; }
    if (!stale_candidate_rejected || detail::metfrag_fragmenter_jar_version(stale_destination.string()) != "0.1.0-SNAPSHOT")
        throw std::runtime_error("A stale candidate replaced the existing fragmenter JAR.");
    const auto published_dir = scratch / "published";
    fs::create_directories(published_dir);
    const auto published = published_dir / ("streamfind-metfrag-fragmenter-" + std::string(kMetFragFragmenterVersion) + ".jar");
    write_jar(staged_jar, std::string(kMetFragFragmenterVersion), true);
    detail::promote_metfrag_fragmenter_jar(staged_jar.string(), published.string());
    if (detail::metfrag_fragmenter_jar_version(published.string()) != kMetFragFragmenterVersion)
        throw std::runtime_error("The configured release's embedded version was not accepted and published.");

    const auto default_args = detail::curl_download_arguments("out.jar", "https://example.invalid/file", true);
    for (const auto& arg : default_args)
        if (arg == "--ssl-no-revoke") throw std::runtime_error("Revocation checks were bypassed by default.");
#ifdef _WIN32
    set_env("STREAMFIND_CURL_SSL_NO_REVOKE", "1");
    const auto opted_in_args = detail::curl_download_arguments("out.jar", "https://example.invalid/file", true);
    bool opt_in_present = false;
    for (const auto& arg : opted_in_args) opt_in_present |= arg == "--ssl-no-revoke";
    if (!opt_in_present) throw std::runtime_error("The explicit Windows Schannel workaround was not added.");
    clear_env("STREAMFIND_CURL_SSL_NO_REVOKE");
#endif
    const auto curl_error = scratch / "curl.stderr";
    { std::ofstream error(curl_error); error << "Schannel: next InitializeSecurityContext failed: CRYPT_E_NO_REVOCATION_CHECK\r\n"; }
    const auto diagnostic = detail::read_curl_diagnostic(curl_error.string());
    if (diagnostic.find("CRYPT_E_NO_REVOCATION_CHECK") == std::string::npos)
        throw std::runtime_error("The captured curl proxy diagnostic was not readable.");
    clear_env("STREAMFIND_HOME");
    fs::remove_all(scratch);
    return 0;
}

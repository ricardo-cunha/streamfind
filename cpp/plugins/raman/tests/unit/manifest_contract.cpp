#include <fstream>
#include <stdexcept>
#include <string>

int main() {
    std::ifstream input(RAMAN_PLUGIN_MANIFEST);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.find("\"plugin_id\": \"raman\"") == std::string::npos)
        throw std::runtime_error("Raman plugin manifest does not identify the raman domain");
    return 0;
}

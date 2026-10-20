#include <fstream>
#include <stdexcept>
#include <string>

int main() {
    std::ifstream input(SENSORS_PLUGIN_MANIFEST);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.find("\"plugin_id\": \"sensors\"") == std::string::npos)
        throw std::runtime_error("Sensors plugin manifest does not identify the sensors domain");
    return 0;
}

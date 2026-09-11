#include "streamfind/sensors/register.hpp"
#include "streamfind/catalogue_binding.hpp"

namespace streamfind::sensors {

void register_plugin(const Json &entries, MethodRegistry &methods, OperationRegistry &operations) {
    catalogue::register_module({"sensors.base", "sensors", "1", {}, {}, {}, {}, {}}, entries, methods, operations);
}

}

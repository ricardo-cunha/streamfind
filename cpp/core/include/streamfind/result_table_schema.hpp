#pragma once

#include "streamfind/export.hpp"
#include "streamfind/data_proxy_protocol.hpp"

#include <memory>

namespace arrow {
class Schema;
}

namespace streamfind {

/** Build the physical Arrow schema declared by a published table descriptor. */
STREAMFIND_CORE_API std::shared_ptr<arrow::Schema> result_table_arrow_schema(const data_proxy::Json &columns);

}  // namespace streamfind

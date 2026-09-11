#pragma once

#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"

namespace streamfind::mass_spec::processing {

STREAMFIND_DOMAIN_API void install_chromatograms_schema(streamfind::ProjectTableStore &tables);

Json load_chromatograms(streamfind::Project &project, const Json &parameters);
Json filter_chromatograms_retention_time(streamfind::Project &project, const Json &parameters);

}

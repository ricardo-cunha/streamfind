#pragma once

#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"

namespace streamfind::mass_spec::processing_methods {

STREAMFIND_DOMAIN_API void install_nta_schema(streamfind::ProjectTableStore &tables);

Json find_features(streamfind::Project &project, const Json &parameters);

Json load_features_ms1(streamfind::Project &project, const Json &parameters);

Json load_features_ms2(streamfind::Project &project, const Json &parameters);

Json subtract_blank(streamfind::Project &project, const Json &parameters);

Json filter_features(streamfind::Project &project, const Json &parameters);

Json filter_features_ms2(streamfind::Project &project, const Json &parameters);

Json group_features(streamfind::Project &project, const Json &parameters);

Json fill_features(streamfind::Project &project, const Json &parameters);

Json create_components(streamfind::Project &project, const Json &parameters);

Json annotate_components(streamfind::Project &project, const Json &parameters);

Json suspect_screening(streamfind::Project &project, const Json &parameters);

Json find_internal_standards(streamfind::Project &project, const Json &parameters);

Json filter_suspects(streamfind::Project &project, const Json &parameters);

Json filter_internal_standards(streamfind::Project &project, const Json &parameters);

Json correct_matrix_suppression(streamfind::Project &project, const Json &parameters);

Json assign_transformation_products(streamfind::Project &project, const Json &parameters);

Json metfrag_screening(streamfind::Project &project, const Json &parameters);

}

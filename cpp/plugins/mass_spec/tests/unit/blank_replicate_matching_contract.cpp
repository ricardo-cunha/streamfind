#include "operations/nta/nta_blank_subtraction.hpp"

#include <cassert>
#include <string>
#include <vector>

int main()
{
    const auto indices = streamfind::mass_spec::nta::blank_subtraction::index_analyses_by_replicate(
        {"sample-A", "blank-A", "sample-B", "blank-A", "sample-A"});

    assert(indices.at("blank-A") == std::vector<std::size_t>({1, 3}));
    assert(indices.at("sample-A") == std::vector<std::size_t>({0, 4}));
    assert(indices.at("sample-B") == std::vector<std::size_t>({2}));
    assert(indices.find("missing-blank") == indices.end());
    return 0;
}

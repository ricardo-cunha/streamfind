#include "utils/nta.hpp"

#include <stdexcept>
#include <string>

int main()
{
  std::string reasons = "low_signal low_area";
  streamfind::mass_spec::nta::utils::append_filter_reason(reasons, "blank_subtraction");
  if (reasons != "low_signal low_area blank_subtraction")
    throw std::runtime_error("Appending a filter reason did not preserve prior reasons.");

  streamfind::mass_spec::nta::utils::append_filter_reason(reasons, "blank_subtraction");
  if (reasons != "low_signal low_area blank_subtraction")
    throw std::runtime_error("Appending an existing filter reason duplicated it.");

  std::string empty;
  streamfind::mass_spec::nta::utils::append_filter_reason(empty, "blank_subtraction");
  if (empty != "blank_subtraction")
    throw std::runtime_error("Appending a filter reason to an empty value failed.");

  return 0;
}

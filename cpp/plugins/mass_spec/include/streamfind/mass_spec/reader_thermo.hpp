#ifndef STREAMFIND_MASS_SPEC_READER_THERMO_HPP
#define STREAMFIND_MASS_SPEC_READER_THERMO_HPP

#include "streamfind/mass_spec/reader.hpp"

#include <memory>
#include <string>

namespace mass_spec::reader::thermo
{
  bool is_thermo_raw(const std::string &path);
  std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &path);
}

#endif

#include "readers/reader_bruker.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
  if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--all-spectra"))
  {
    std::cerr << "Usage: validate_bruker_calibration <vendor-bruker-directory> [--all-spectra]\n";
    return 2;
  }

  try
  {
    const auto family = mass_spec::reader::bruker::detect_family(argv[1]);
    const auto reader = family == mass_spec::reader::bruker::Family::Tsf
                            ? mass_spec::reader::bruker::create_tsf_reader(argv[1])
                            : family == mass_spec::reader::bruker::Family::Baf
                                  ? mass_spec::reader::bruker::create_baf_reader(argv[1])
                                  : throw std::runtime_error("The path is not a supported Bruker TSF or BAF directory.");
    const auto count = reader->get_number_spectra();
    if (count <= 0)
      throw std::runtime_error("The Bruker reader returned no spectra.");

    const bool all_spectra = argc == 3;
    std::vector<int> indices;
    if (all_spectra)
    {
      indices.resize(static_cast<std::size_t>(count));
      for (int index = 0; index < count; ++index)
        indices[static_cast<std::size_t>(index)] = index;
    }
    else
      indices = {0, count / 2, count - 1};

    const auto headers = reader->get_spectra_headers(indices);
    std::size_t checked_peaks = 0;
    for (const auto index : indices)
    {
      const auto spectrum = reader->get_spectrum(index);
      if (spectrum.binary_data.size() < 2 || spectrum.binary_data[0].empty() ||
          spectrum.binary_data[0].size() != spectrum.binary_data[1].size())
        throw std::runtime_error("A Bruker spectrum has incomplete m/z or intensity arrays.");
      for (const auto mz : spectrum.binary_data[0])
      {
        if (!(mz > 0.0f) || !std::isfinite(mz))
          throw std::runtime_error("A Bruker spectrum contains an invalid calibrated m/z value.");
      }
      checked_peaks += spectrum.binary_data[0].size();
    }

    std::cout << "Calibration: " << reader->get_calibration_provenance() << '\n'
              << "Spectra: " << count << '\n'
              << (all_spectra ? "Validated spectra: " : "Sampled spectra: ") << indices.size() << '\n'
              << "Finite calibrated peaks: " << checked_peaks << '\n'
              << "Header rows: " << headers.index.size() << '\n';
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

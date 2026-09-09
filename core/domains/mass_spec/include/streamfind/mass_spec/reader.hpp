#ifndef MASS_SPEC_READER_H
#define MASS_SPEC_READER_H

#include <limits>
#include <vector>
#include <string>
#include <unordered_set>
#include <cstdlib>
#include <cstdio>

namespace mass_spec::spectra {
struct MASS_SPEC_TARGETS {
    std::vector<std::string> id;
    std::vector<int> index;
    std::vector<int> level;
    std::vector<int> polarity;
    std::vector<bool> precursor;
    std::vector<float> mzmin;
    std::vector<float> mzmax;
    std::vector<float> rtmin;
    std::vector<float> rtmax;
    std::vector<float> mz;
    std::vector<float> mass;
    std::vector<float> rt;
    std::vector<float> mobility;
    std::vector<float> mobilitymin;
    std::vector<float> mobilitymax;

    void resize_all(int n)
    {
      id.resize(n); index.resize(n); level.resize(n); polarity.resize(n);
      precursor.resize(n); mzmin.resize(n); mzmax.resize(n); rtmin.resize(n);
      rtmax.resize(n); mz.resize(n); mass.resize(n); rt.resize(n);
      mobility.resize(n); mobilitymin.resize(n); mobilitymax.resize(n);
    }
};

struct MASS_SPEC_TARGETS_SPECTRA {
    std::vector<std::string> id;
    std::vector<int> polarity;
    std::vector<int> level;
    std::vector<float> pre_mz;
    std::vector<float> pre_mzlow;
    std::vector<float> pre_mzhigh;
    std::vector<float> pre_ce;
    std::vector<float> rt;
    std::vector<float> mobility;
    std::vector<float> mz;
    std::vector<float> intensity;

    void resize_all(int n)
    {
      id.resize(n); polarity.resize(n); level.resize(n); pre_mz.resize(n);
      pre_mzlow.resize(n); pre_mzhigh.resize(n); pre_ce.resize(n); rt.resize(n);
      mobility.resize(n); mz.resize(n); intensity.resize(n);
    }

    size_t size() const { return id.size(); }

    int number_ids() const
    {
      std::unordered_set<std::string> unique_ids(id.begin(), id.end());
      return static_cast<int>(unique_ids.size());
    }

    // R-compatible accessor: return the slice of this spectra belonging to one
    // target id (used by NTA gap filling and blank subtraction).
    MASS_SPEC_TARGETS_SPECTRA operator[](const std::string &unique_id) const
    {
      MASS_SPEC_TARGETS_SPECTRA target;
      for (size_t i = 0; i < id.size(); ++i)
      {
        if (id[i] == unique_id)
        {
          target.id.push_back(id[i]);
          target.polarity.push_back(i < polarity.size() ? polarity[i] : 0);
          target.level.push_back(i < level.size() ? level[i] : 0);
          if (i < pre_mz.size()) target.pre_mz.push_back(pre_mz[i]);
          if (i < pre_mzlow.size()) target.pre_mzlow.push_back(pre_mzlow[i]);
          if (i < pre_mzhigh.size()) target.pre_mzhigh.push_back(pre_mzhigh[i]);
          if (i < pre_ce.size()) target.pre_ce.push_back(pre_ce[i]);
          if (i < rt.size()) target.rt.push_back(rt[i]);
          if (i < mobility.size()) target.mobility.push_back(mobility[i]);
          if (i < mz.size()) target.mz.push_back(mz[i]);
          if (i < intensity.size()) target.intensity.push_back(intensity[i]);
        }
      }
      return target;
    }
};

// R-package-compatible aliases so NTA algorithms written against the former
// reader API keep their original operations unchanged.
using MS_TARGETS = MASS_SPEC_TARGETS;
using MS_TARGETS_SPECTRA = MASS_SPEC_TARGETS_SPECTRA;
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mass_spec
{
  namespace reader
  {
    namespace utils
    {

      std::string encode_little_endian_from_float(const std::vector<float> &input, int precision);
      std::string encode_little_endian_from_double(const std::vector<double> &input, int precision);
      std::vector<float> decode_little_endian_to_float(const std::string &str, int precision);
      std::vector<double> decode_little_endian_to_double(const std::string &str, int precision);
      std::vector<float> decode_big_endian_to_float(const std::string &str, int precision);
      std::vector<double> decode_big_endian_to_double(const std::string &str, int precision);
      std::string encode_base64(const std::string &input);
      std::string decode_base64(const std::string &input);
      std::string decode_base64_simduft(const std::string &encoded_string);
      std::string encode_base64_simduft(const std::string &input);
      std::string compress_zlib(const std::string &str);
      std::string decompress_zlib(const std::string &input);

    } // namespace utils

    namespace ole
    {
      struct StreamInfo
      {
        std::string path;
        std::string normalized_path;
        std::uint64_t size = 0;
        bool is_mini_stream = false;
      };

      bool is_compound_file(const std::string &file_path);
      std::vector<StreamInfo> list_streams(const std::string &file_path);
      std::vector<std::uint8_t> read_stream(const std::string &file_path, const std::string &normalized_path);
    } // namespace ole

    struct MASS_SPEC_SPECTRUM
    {
      int index;
      int scan;
      int array_length;
      int level;
      int mode;
      int polarity;
      float lowmz;
      float highmz;
      float bpmz;
      float bpint;
      float tic;
      int configuration;
      float rt;
      float mobility;
      float window_mz;
      float window_mzlow;
      float window_mzhigh;
      float precursor_mz;
      float precursor_intensity;
      int precursor_charge;
      float activation_ce;
      int binary_arrays_count;
      std::vector<std::string> binary_names;
      std::vector<std::vector<float>> binary_data;
    };

    struct MASS_SPEC_SPECTRA_HEADERS
    {
      std::vector<int> index;
      std::vector<int> scan;
      std::vector<int> array_length;
      std::vector<int> level;
      std::vector<int> mode;
      std::vector<int> polarity;
      std::vector<float> lowmz;
      std::vector<float> highmz;
      std::vector<float> bpmz;
      std::vector<float> bpint;
      std::vector<float> tic;
      std::vector<int> configuration;
      std::vector<float> rt;
      std::vector<float> mobility;
      std::vector<float> window_mz;
      std::vector<float> window_mzlow;
      std::vector<float> window_mzhigh;
      std::vector<float> precursor_mz;
      std::vector<float> precursor_intensity;
      std::vector<int> precursor_charge;
      std::vector<float> activation_ce;

      void resize_all(int n)
      {
        index.resize(n);
        scan.resize(n);
        array_length.resize(n);
        level.resize(n);
        mode.resize(n);
        polarity.resize(n);
        lowmz.resize(n);
        highmz.resize(n);
        bpmz.resize(n);
        bpint.resize(n);
        tic.resize(n);
        configuration.resize(n);
        rt.resize(n);
        mobility.resize(n);
        window_mz.resize(n);
        window_mzlow.resize(n);
        window_mzhigh.resize(n);
        precursor_mz.resize(n);
        precursor_intensity.resize(n);
        precursor_charge.resize(n);
        activation_ce.resize(n);
      }

      size_t size() const { return index.size(); }
    };

    struct MASS_SPEC_SUMMARY
    {
      std::string file_name;
      std::string file_path;
      std::string file_dir;
      std::string file_extension;
      int number_spectra;
      int number_chromatograms;
      int number_spectra_binary_arrays;
      std::string format;
      std::string time_stamp;
      std::vector<int> polarity;
      std::vector<int> mode;
      std::vector<int> level;
      std::vector<int> configuration;
      std::string type;
      float min_mz;
      float max_mz;
      float start_rt;
      float end_rt;
      bool has_ion_mobility;
    };

    struct MASS_SPEC_ANALYSIS
    {
      int analysis_index = 0;
      int source_analysis_number = 0;
      std::string name;
      int analysis_count = 1;
    };

    struct MASS_SPEC_CHROMATOGRAMS_HEADERS
    {
      std::vector<int> index;
      std::vector<std::string> chromatogram_id;
      std::vector<int> array_length;
      std::vector<int> polarity;
      std::vector<float> precursor_mz;
      std::vector<float> activation_ce;
      std::vector<float> product_mz;
      std::vector<std::string> signal_type;
      std::vector<std::string> chromatogram_type;
      std::vector<std::string> detector;
      std::vector<std::string> channel;
      std::vector<std::string> units;
      std::vector<float> wavelength_nm;
      std::vector<float> interval_ms;
      std::vector<float> start_time;
      std::vector<float> end_time;
      std::vector<float> intensity_multiplier;

      void resize_all(int n)
      {
        const float na = std::numeric_limits<float>::quiet_NaN();
        index.resize(n);
        chromatogram_id.resize(n);
        array_length.resize(n);
        polarity.resize(n);
        precursor_mz.assign(n, na);
        activation_ce.assign(n, na);
        product_mz.assign(n, na);
        signal_type.resize(n);
        chromatogram_type.resize(n);
        detector.resize(n);
        channel.resize(n);
        units.resize(n);
        wavelength_nm.assign(n, na);
        interval_ms.assign(n, na);
        start_time.assign(n, na);
        end_time.assign(n, na);
        intensity_multiplier.assign(n, 1.0f);
      }

      size_t size() const { return index.size(); }
    };

    class MASS_SPEC_READER
    {
    public:
      explicit MASS_SPEC_READER(const std::string &file) : file_(file) {}
      virtual ~MASS_SPEC_READER() = default;

      virtual int get_number_spectra() = 0;
      virtual int get_number_chromatograms() = 0;
      virtual int get_number_spectra_binary_arrays() = 0;
      virtual std::string get_format() = 0;
      virtual std::string get_type() = 0;
      virtual std::string get_time_stamp() = 0;
      virtual std::vector<int> get_polarity() = 0;
      virtual std::vector<int> get_mode() = 0;
      virtual std::vector<int> get_level() = 0;
      virtual std::vector<int> get_configuration() = 0;
      virtual float get_min_mz() = 0;
      virtual float get_max_mz() = 0;
      virtual float get_start_rt() = 0;
      virtual float get_end_rt() = 0;
      virtual bool has_ion_mobility() = 0;
      virtual MASS_SPEC_SUMMARY get_summary() = 0;
      virtual std::vector<int> get_spectra_index(std::vector<int> indices = {}) = 0;
      virtual std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) = 0;
      virtual std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) = 0;
      virtual std::vector<int> get_spectra_level(std::vector<int> indices = {}) = 0;
      virtual std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) = 0;
      virtual std::vector<int> get_spectra_mode(std::vector<int> indices = {}) = 0;
      virtual std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_tic(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_rt(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) = 0;
      virtual std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) = 0;
      virtual std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) = 0;
      virtual MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {},
                                                     bool derive_missing_stats = false) = 0;
      virtual MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) = 0;
      virtual std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) = 0;
      virtual std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) = 0;
      virtual std::vector<std::vector<std::string>> get_software() = 0;
      virtual std::vector<std::vector<std::string>> get_hardware() = 0;
      virtual MASS_SPEC_SPECTRUM get_spectrum(const int &idx) = 0;

      const std::string &file() const { return file_; }

    protected:
      void trace_payload_decode(const char *kind, const int index) const
      {
        const char *enabled = std::getenv("STREAMFIND_TRACE_PAYLOAD_DECODES");
        if (enabled != nullptr && enabled[0] != '\0' && std::string(enabled) != "0")
          std::fprintf(stderr, "STREAMFIND_PAYLOAD_DECODE kind=%s index=%d\n", kind, index);
      }

      void trace_spectrum_decode(const int index) const { trace_payload_decode("spectrum", index); }
      void trace_chromatogram_decode(const int index) const { trace_payload_decode("chromatogram", index); }

      std::string file_;
    };

    class MASS_SPEC_FILE
    {
    public:
      explicit MASS_SPEC_FILE(const std::string &file);

      const std::vector<MASS_SPEC_ANALYSIS> &get_analysis_catalog() const { return analysis_catalog; }
      void select_analysis(int index);
      int selected_analysis_index() const { return selected_analysis; }

      int get_number_spectra() { return ms->get_number_spectra(); }
      int get_number_chromatograms() { return ms->get_number_chromatograms(); }
      int get_number_spectra_binary_arrays() { return ms->get_number_spectra_binary_arrays(); }
      std::string get_format() { return ms->get_format(); }
      std::string get_type() { return ms->get_type(); }
      std::string get_time_stamp() { return ms->get_time_stamp(); }
      std::vector<int> get_polarity() { return ms->get_polarity(); }
      std::vector<int> get_mode() { return ms->get_mode(); }
      std::vector<int> get_level() { return ms->get_level(); }
      std::vector<int> get_configuration() { return ms->get_configuration(); }
      float get_min_mz() { return ms->get_min_mz(); }
      float get_max_mz() { return ms->get_max_mz(); }
      float get_start_rt() { return ms->get_start_rt(); }
      float get_end_rt() { return ms->get_end_rt(); }
      bool has_ion_mobility() { return ms->has_ion_mobility(); }
      MASS_SPEC_SUMMARY get_summary() { return ms->get_summary(); }
      std::vector<int> get_spectra_index(std::vector<int> indices = {}) { return ms->get_spectra_index(indices); }
      std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) { return ms->get_spectra_scan_number(indices); }
      std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) { return ms->get_spectra_array_length(indices); }
      std::vector<int> get_spectra_level(std::vector<int> indices = {}) { return ms->get_spectra_level(indices); }
      std::vector<int> get_spectra_mode(std::vector<int> indices = {}) { return ms->get_spectra_mode(indices); }
      std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) { return ms->get_spectra_polarity(indices); }
      std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) { return ms->get_spectra_lowmz(indices); }
      std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) { return ms->get_spectra_highmz(indices); }
      std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) { return ms->get_spectra_bpmz(indices); }
      std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) { return ms->get_spectra_bpint(indices); }
      std::vector<float> get_spectra_tic(std::vector<int> indices = {}) { return ms->get_spectra_tic(indices); }
      std::vector<float> get_spectra_rt(std::vector<int> indices = {}) { return ms->get_spectra_rt(indices); }
      std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) { return ms->get_spectra_mobility(indices); }
      std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) { return ms->get_spectra_precursor_scan(indices); }
      std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) { return ms->get_spectra_precursor_mz(indices); }
      std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) { return ms->get_spectra_precursor_window_mz(indices); }
      std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) { return ms->get_spectra_precursor_window_mzlow(indices); }
      std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) { return ms->get_spectra_precursor_window_mzhigh(indices); }
      std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) { return ms->get_spectra_collision_energy(indices); }
      MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {},
                                             bool derive_missing_stats = false) { return ms->get_spectra_headers(indices, derive_missing_stats); }
      MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) { return ms->get_chromatograms_headers(indices); }
      std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) { return ms->get_spectra(indices); }
      std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) { return ms->get_chromatograms(indices); }
      std::vector<std::vector<std::string>> get_software() { return ms->get_software(); }
      std::vector<std::vector<std::string>> get_hardware() { return ms->get_hardware(); }
      MASS_SPEC_SPECTRUM get_spectrum(const int &index) { return ms->get_spectrum(index); }
      mass_spec::spectra::MASS_SPEC_TARGETS_SPECTRA get_spectra_targets(const mass_spec::spectra::MASS_SPEC_TARGETS &targets, const MASS_SPEC_SPECTRA_HEADERS &hd, const float &minIntLv1, const float &minIntLv2);

      std::unique_ptr<MASS_SPEC_READER> ms;
      std::string file_path;
      std::string file_dir;
      std::string file_name;
      std::string file_extension;
      std::string format;
      int format_case = -1;
      std::vector<MASS_SPEC_ANALYSIS> analysis_catalog;
      int selected_analysis = 0;

    private:
      const std::vector<std::string> possible_formats = {"mzML", "mzXML", "ShimadzuTXT", "ASC", "ShimadzuLCD"};
    };

    // R-package-compatible aliases so NTA algorithms written against the former
    // reader API keep their original operations unchanged.
    using MS_FILE = MASS_SPEC_FILE;
    using MS_SPECTRA_HEADERS = MASS_SPEC_SPECTRA_HEADERS;

    namespace mzxml
    {

      struct Impl;

      class Reader : public MASS_SPEC_READER
      {
      public:
        explicit Reader(const std::string &file);
        ~Reader() override;

        int get_number_spectra() override;
        int get_number_chromatograms() override;
        int get_number_spectra_binary_arrays() override;
        std::string get_format() override;
        std::string get_type() override;
        std::string get_time_stamp() override;
        std::vector<int> get_polarity() override;
        std::vector<int> get_mode() override;
        std::vector<int> get_level() override;
        std::vector<int> get_configuration() override;
        float get_min_mz() override;
        float get_max_mz() override;
        float get_start_rt() override;
        float get_end_rt() override;
        bool has_ion_mobility() override;
        MASS_SPEC_SUMMARY get_summary() override;
        std::vector<int> get_spectra_index(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_level(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override;
        MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {},
                                               bool derive_missing_stats = false) override;
        MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::string>> get_software() override;
        std::vector<std::vector<std::string>> get_hardware() override;
        MASS_SPEC_SPECTRUM get_spectrum(const int &idx) override;

      private:
        std::unique_ptr<Impl> pimpl;
      };
    } // namespace mzxml

    namespace mzml
    {

      struct Impl;

      class Reader : public MASS_SPEC_READER
      {
      public:
        explicit Reader(const std::string &file);
        ~Reader() override;

        int get_number_spectra() override;
        int get_number_chromatograms() override;
        int get_number_spectra_binary_arrays() override;
        std::string get_format() override;
        std::string get_type() override;
        std::string get_time_stamp() override;
        std::vector<int> get_polarity() override;
        std::vector<int> get_mode() override;
        std::vector<int> get_level() override;
        std::vector<int> get_configuration() override;
        float get_min_mz() override;
        float get_max_mz() override;
        float get_start_rt() override;
        float get_end_rt() override;
        bool has_ion_mobility() override;
        MASS_SPEC_SUMMARY get_summary() override;
        std::vector<int> get_spectra_index(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_level(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override;
        MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {},
                                               bool derive_missing_stats = false) override;
        MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::string>> get_software() override;
        std::vector<std::vector<std::string>> get_hardware() override;
        MASS_SPEC_SPECTRUM get_spectrum(const int &idx) override;

      private:
        std::unique_ptr<Impl> pimpl;
      };
    } // namespace mzml

    namespace shimadzu_txt
    {
      struct Impl;

      class Reader : public MASS_SPEC_READER
      {
      public:
        explicit Reader(const std::string &file);
        ~Reader() override;

        int get_number_spectra() override;
        int get_number_chromatograms() override;
        int get_number_spectra_binary_arrays() override;
        std::string get_format() override;
        std::string get_type() override;
        std::string get_time_stamp() override;
        std::vector<int> get_polarity() override;
        std::vector<int> get_mode() override;
        std::vector<int> get_level() override;
        std::vector<int> get_configuration() override;
        float get_min_mz() override;
        float get_max_mz() override;
        float get_start_rt() override;
        float get_end_rt() override;
        bool has_ion_mobility() override;
        MASS_SPEC_SUMMARY get_summary() override;
        std::vector<int> get_spectra_index(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_level(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override;
        MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {},
                                               bool derive_missing_stats = false) override;
        MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::string>> get_software() override;
        std::vector<std::vector<std::string>> get_hardware() override;
        MASS_SPEC_SPECTRUM get_spectrum(const int &idx) override;

      private:
        std::unique_ptr<Impl> pimpl;
      };
    } // namespace shimadzu_txt

    namespace asc
    {
      struct Impl;

      class Reader : public MASS_SPEC_READER
      {
      public:
        explicit Reader(const std::string &file);
        ~Reader() override;

        int get_number_spectra() override;
        int get_number_chromatograms() override;
        int get_number_spectra_binary_arrays() override;
        std::string get_format() override;
        std::string get_type() override;
        std::string get_time_stamp() override;
        std::vector<int> get_polarity() override;
        std::vector<int> get_mode() override;
        std::vector<int> get_level() override;
        std::vector<int> get_configuration() override;
        float get_min_mz() override;
        float get_max_mz() override;
        float get_start_rt() override;
        float get_end_rt() override;
        bool has_ion_mobility() override;
        MASS_SPEC_SUMMARY get_summary() override;
        std::vector<int> get_spectra_index(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_level(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override;
        MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {},
                                               bool derive_missing_stats = false) override;
        MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::string>> get_software() override;
        std::vector<std::vector<std::string>> get_hardware() override;
        MASS_SPEC_SPECTRUM get_spectrum(const int &idx) override;

      private:
        std::unique_ptr<Impl> pimpl;
      };
    } // namespace asc

    namespace shimadzu_lcd
    {
      struct Impl;

      class Reader : public MASS_SPEC_READER
      {
      public:
        explicit Reader(const std::string &file);
        ~Reader() override;

        int get_number_spectra() override;
        int get_number_chromatograms() override;
        int get_number_spectra_binary_arrays() override;
        std::string get_format() override;
        std::string get_type() override;
        std::string get_time_stamp() override;
        std::vector<int> get_polarity() override;
        std::vector<int> get_mode() override;
        std::vector<int> get_level() override;
        std::vector<int> get_configuration() override;
        float get_min_mz() override;
        float get_max_mz() override;
        float get_start_rt() override;
        float get_end_rt() override;
        bool has_ion_mobility() override;
        MASS_SPEC_SUMMARY get_summary() override;
        std::vector<int> get_spectra_index(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_level(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override;
        std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override;
        std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override;
        MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {},
                                               bool derive_missing_stats = false) override;
        MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override;
        std::vector<std::vector<std::string>> get_software() override;
        std::vector<std::vector<std::string>> get_hardware() override;
        MASS_SPEC_SPECTRUM get_spectrum(const int &idx) override;

      private:
        std::unique_ptr<Impl> pimpl;
      };
    } // namespace shimadzu_lcd

    std::string detect_format(const std::string &file_path);
    std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &file_path);

  } // namespace reader
} // namespace mass_spec

#endif // MASS_SPEC_READER_H

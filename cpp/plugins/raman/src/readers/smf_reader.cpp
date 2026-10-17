#include "readers/reader.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace streamfind::raman::reader::smf_detail {

// This reader handles the observed Berkeley DB v5 legacy hash pages (type 2)
// and overflow pages (type 7); it intentionally rejects other BDB layouts.
class Database {
  public:
    explicit Database(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {
        if (bytes_.size() < 512 || u32(12) != 0x00061561 || u32(16) != 5)
            throw std::runtime_error("SMF is not the supported Berkeley DB hash version 5 layout");
        page_size_ = u32(20);
        if (page_size_ < 512 || (page_size_ & (page_size_ - 1)) != 0 ||
            bytes_.size() % page_size_ != 0)
            throw std::runtime_error("SMF Berkeley DB page size or file length is invalid");
    }

    std::uint16_t u16(std::uint64_t at) const {
        require(at, 2);
        return static_cast<std::uint16_t>(bytes_[at]) |
               static_cast<std::uint16_t>(bytes_[at + 1] << 8);
    }
    std::uint32_t u32(std::uint64_t at) const {
        require(at, 4);
        return static_cast<std::uint32_t>(bytes_[at]) |
               (static_cast<std::uint32_t>(bytes_[at + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes_[at + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes_[at + 3]) << 24);
    }
    std::uint64_t u64(std::uint64_t at) const {
        return static_cast<std::uint64_t>(u32(at)) |
               (static_cast<std::uint64_t>(u32(at + 4)) << 32);
    }

    struct Record {
        std::string key;
        std::vector<std::uint8_t> value;
    };

    std::vector<Record> records() const {
        std::vector<Record> out;
        const auto page_count = bytes_.size() / page_size_;
        for (std::uint64_t page_no = 1; page_no < page_count; ++page_no) {
            const auto page = page_no * page_size_;
            if (u32(page + 8) != page_no || bytes_[page + 25] != 2) continue;
            const auto entries = u16(page + 20);
            if ((entries & 1U) != 0 || entries > (page_size_ - 26) / 2)
                throw std::runtime_error("SMF hash bucket has an invalid item index");
            for (std::uint16_t index = 0; index < entries; index += 2) {
                const auto key = item(page, index);
                const auto value = item(page, static_cast<std::uint16_t>(index + 1));
                if (key.empty()) continue;
                std::string key_text(key.begin() + 1, key.end());
                while (!key_text.empty() && (key_text.back() == '\0' || key_text.back() == ' '))
                    key_text.pop_back();
                out.push_back({std::move(key_text), value});
            }
        }
        return out;
    }

  private:
    void require(std::uint64_t at, std::uint64_t size) const {
        if (at > bytes_.size() || size > bytes_.size() - at)
            throw std::runtime_error("SMF database contains an out-of-range page reference");
    }

    std::vector<std::uint8_t> item(std::uint64_t page, std::uint16_t index) const {
        const auto slot = page + 26 + static_cast<std::uint64_t>(index) * 2;
        const auto offset = u16(slot);
        const auto previous = index == 0 ? page_size_ : u16(slot - 2);
        if (offset < 26 || previous < offset || previous > page_size_)
            throw std::runtime_error("SMF hash item points outside its page");
        const auto length = static_cast<std::uint64_t>(previous - offset);
        const auto absolute = page + offset;
        require(absolute, length);
        if (length == 0) return {};
        const auto type = bytes_[absolute];
        if (type == 1) {
            return {bytes_.begin() + static_cast<std::ptrdiff_t>(absolute + 1),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(absolute + length)};
        }
        if (type != 3 || length < 12)
            throw std::runtime_error("SMF uses an unrecognized Berkeley DB hash item type");
        const auto first_page = u32(absolute + 4);
        const auto total_size = u32(absolute + 8);
        if (total_size > 128U * 1024U * 1024U)
            throw std::runtime_error("SMF overflow record exceeds the supported size limit");
        std::vector<std::uint8_t> result;
        result.reserve(total_size);
        std::uint32_t current = first_page;
        std::uint32_t previous_page = 0;
        std::uint64_t remaining = total_size;
        std::uint64_t visited = 0;
        while (remaining != 0) {
            if (current == 0 || ++visited > bytes_.size() / page_size_)
                throw std::runtime_error("SMF overflow chain is truncated or cyclic");
            const auto at = static_cast<std::uint64_t>(current) * page_size_;
            require(at, page_size_);
            if (u32(at + 8) != current || bytes_[at + 25] != 7 || u32(at + 12) != previous_page)
                throw std::runtime_error("SMF overflow chain has an invalid page link");
            const auto available = u16(at + 22);
            if (available == 0 || available > page_size_ - 26)
                throw std::runtime_error("SMF overflow page has an invalid payload length");
            const auto take = std::min<std::uint64_t>(remaining, available);
            result.insert(result.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(at + 26),
                          bytes_.begin() + static_cast<std::ptrdiff_t>(at + 26 + take));
            remaining -= take;
            previous_page = current;
            current = u32(at + 16);
        }
        return result;
    }

    std::vector<std::uint8_t> bytes_;
    std::uint32_t page_size_ = 0;
};

std::string text_field(const std::vector<std::uint8_t> &bytes, std::size_t offset,
                       std::size_t capacity) {
    if (offset >= bytes.size()) return {};
    const auto end = std::min(bytes.size(), offset + capacity);
    auto first = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    auto last = std::find(first, bytes.begin() + static_cast<std::ptrdiff_t>(end), 0);
    return std::string(first, last);
}

bool is_wavenumber_unit(std::string unit) {
    unit.erase(std::remove_if(unit.begin(), unit.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }), unit.end());
    std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return unit == "1/cm" || unit == "cm-1" || unit == "cm^-1" || unit == "cm⁻¹";
}

std::uint32_t u32_at(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("SMF spectrum-axis record is truncated");
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

double f64_at(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    if (offset + 8 > bytes.size()) throw std::runtime_error("SMF spectrum-axis record is truncated");
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < 8; ++i)
        bits |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return std::bit_cast<double>(bits);
}

}  // namespace streamfind::raman::reader::smf_detail

namespace streamfind::raman::reader {

SpectrumFile read_smf_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback) {
    using namespace smf_detail;
    if (!spectrum_callback) throw std::invalid_argument("SMF spectrum callback is required");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open Shimadzu IRsolution SMF file: " + path.string());
    const auto size = input.tellg();
    if (size < 512 || size > static_cast<std::streamoff>(512ULL * 1024 * 1024))
        throw std::runtime_error("SMF file size is outside supported bounds");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), size))
        throw std::runtime_error("cannot read Shimadzu IRsolution SMF file: " + path.string());

    Database database(std::move(bytes));
    const auto records = database.records();
    std::map<std::string, std::vector<std::uint8_t>> by_key;
    for (const auto &record : records) by_key.emplace(record.key, record.value);
    std::map<std::string, std::string> signal_labels;
    for (const auto &record : records) {
        const auto space = record.key.find(' ');
        if (space == std::string::npos || record.key.substr(space + 1) != "100003") continue;
        const auto label = text_field(record.value, 12, 20);
        if (label.empty()) continue;
        auto &selected = signal_labels[record.key];
        if (selected.empty() || label == "%T") selected = label;
    }

    struct Candidate {
        std::string object_id;
        std::string source_axis_unit;
        std::vector<std::uint8_t> axis_record;
        std::vector<std::uint8_t> spectrum;
        std::string description;
        std::string signal_label;
    };
    struct AuxiliaryRecord {
        std::string object_id;
        std::string axis_unit;
        std::uint32_t point_count = 0;
        std::size_t payload_bytes = 0;
    };
    std::vector<Candidate> candidates;
    std::vector<AuxiliaryRecord> auxiliary_records;
    std::string axis_records_seen;
    for (const auto &[key, value] : by_key) {
        const auto space = key.find(' ');
        if (space == std::string::npos || key.substr(space + 1) != "100001") continue;
        const auto id = key.substr(0, space);
        const auto unit = text_field(value, 12, 20);
        const auto spectrum_it = by_key.find(id + " 100008");
        axis_records_seen += " [object=" + id + ", unit=" + unit + ", axis_bytes=" +
                             std::to_string(value.size()) + ", has_100008=" +
                             (spectrum_it == by_key.end() ? "no" : "yes") + "]";
        if (spectrum_it == by_key.end()) continue;
        if (!is_wavenumber_unit(unit)) {
            if (value.size() >= 80)
                auxiliary_records.push_back({id, unit, u32_at(value, 76), spectrum_it->second.size()});
            continue;
        }
        Candidate candidate;
        candidate.object_id = id;
        candidate.source_axis_unit = unit;
        candidate.axis_record = value;
        candidate.spectrum = spectrum_it->second;
        const auto signal_label = signal_labels.find(id + " 100003");
        if (signal_label != signal_labels.end()) candidate.signal_label = signal_label->second;
        const auto description = by_key.find(id + " 2 Description");
        if (description != by_key.end())
            candidate.description = text_field(description->second, 0, description->second.size());
        candidates.push_back(std::move(candidate));
    }
    if (candidates.empty())
        throw std::runtime_error("SMF contains no supported 1/cm spectrum with a matching 100008 payload; axis records:" +
                                 axis_records_seen);

    SpectrumFile result;
    result.path = path;
    result.format = "shimadzu_irsolution_smf";
    result.axis_kind = "wavenumber";
    result.axis_unit = "cm^-1";
    result.signal_kind = "IR signal";
    result.signal_unit = "unknown";
    result.metadata.push_back({"Container", "Shimadzu IRsolution SMF (Berkeley DB hash v5)", 0});
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].signal_label == "%T") {
            result.signal_kind = "transmittance";
            result.signal_unit = "%";
            break;
        }
    }
    for (std::size_t i = 0; i < auxiliary_records.size(); ++i) {
        const auto &aux = auxiliary_records[i];
        result.metadata.push_back({"Auxiliary SMF object " + aux.object_id,
            "axis unit=" + aux.axis_unit + "; points=" + std::to_string(aux.point_count) +
            "; record 100008 payload bytes=" + std::to_string(aux.payload_bytes), i});
    }

    for (std::size_t spectrum_index = 0; spectrum_index < candidates.size(); ++spectrum_index) {
        const auto &candidate = candidates[spectrum_index];
        const auto count = u32_at(candidate.axis_record, 76);
        const auto first = f64_at(candidate.axis_record, 96);
        const auto last = f64_at(candidate.axis_record, 112);
        const auto step = f64_at(candidate.axis_record, 128);
        if (count < 2 || count > 10'000'000 || candidate.spectrum.size() != count * sizeof(double) ||
            !std::isfinite(first) || !std::isfinite(last) || !std::isfinite(step) || step == 0.0)
            throw std::runtime_error("SMF axis dimensions do not match the stored spectrum payload");
        const auto expected_step = (last - first) / static_cast<double>(count - 1);
        const auto step_tolerance = std::max(1e-10, std::abs(expected_step) * 1e-6);
        if (std::abs(expected_step - step) > step_tolerance)
            throw std::runtime_error("SMF axis endpoints and increment are inconsistent");

        if (!candidate.description.empty())
            result.metadata.push_back({"Description", candidate.description, spectrum_index});
        if (!candidate.signal_label.empty())
            result.metadata.push_back({"Source Signal Label", candidate.signal_label, spectrum_index});
        result.metadata.push_back({"SMF Record 100001", "axis definition", spectrum_index});
        result.metadata.push_back({"SMF Record 100008", "spectrum values (IEEE float64)", spectrum_index});
        result.metadata.push_back({"SMF Spectrum Object ID", candidate.object_id, spectrum_index});
        result.metadata.push_back({"Source Axis Unit", candidate.source_axis_unit, spectrum_index});
        result.metadata.push_back({"Axis Start", std::to_string(first), spectrum_index});
        result.metadata.push_back({"Axis End", std::to_string(last), spectrum_index});
        result.metadata.push_back({"Axis Increment", std::to_string(step), spectrum_index});
        SpectrumFrame frame;
        frame.index = spectrum_index;
        frame.points.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto intensity = f64_at(candidate.spectrum, static_cast<std::size_t>(i) * 8);
            const auto coordinate = first + static_cast<double>(i) * step;
            if (!std::isfinite(intensity) || !std::isfinite(coordinate))
                throw std::runtime_error("SMF spectrum contains non-finite numeric values");
            frame.points.push_back({coordinate, intensity, i});
        }
        result.point_count += frame.points.size();
        spectrum_callback(result, frame);
        ++result.spectrum_count;
    }
    return result;
}

}  // namespace streamfind::raman::reader

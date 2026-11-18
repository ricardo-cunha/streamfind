#include "operations/nta/operations.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include "utils/nta.hpp"
#include "readers/reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::mass_spec::nta::update_suspect_targets
{
using Json = nlohmann::json;

namespace detail
{
struct Peak { double mz = 0.0; double intensity = 0.0; };

struct Evidence
{
    std::vector<double> masses;
    std::vector<double> rts;
    std::vector<double> xlogp;
    std::vector<Peak> peaks;
};

struct TargetRow
{
    std::string name, formula, smiles, inchi, inchikey;
    bool has_name = false, has_formula = false, has_smiles = false, has_inchi = false, has_inchikey = false;
    bool has_mass = false, has_mz = false, has_rt = false, has_xlogp = false, has_polarity = false;
    bool has_fragments_mz_pos = false, has_fragments_intensity_pos = false;
    bool has_fragments_mz_neg = false, has_fragments_intensity_neg = false;
    double mass = std::numeric_limits<double>::quiet_NaN();
    double mz = std::numeric_limits<double>::quiet_NaN();
    double rt = std::numeric_limits<double>::quiet_NaN();
    double xlogp = std::numeric_limits<double>::quiet_NaN();
    int polarity = 0;
    std::vector<double> fragments_mz_pos, fragments_intensity_pos;
    std::vector<double> fragments_mz_neg, fragments_intensity_neg;
};

struct SuspectRow
{
    std::string analysis, name, formula, smiles, inchi, inchikey, db_ms2_formula;
    double db_mass = std::numeric_limits<double>::quiet_NaN();
    double exp_rt = std::numeric_limits<double>::quiet_NaN();
    double intensity = std::numeric_limits<double>::quiet_NaN();
    double area = std::numeric_limits<double>::quiet_NaN();
    double error_mass = std::numeric_limits<double>::quiet_NaN();
    double cosine_similarity = std::numeric_limits<double>::quiet_NaN();
    double xlogp = std::numeric_limits<double>::quiet_NaN();
    int polarity = 0, id_level = 0, candidate_rank = 0, shared_fragments = 0, db_ms2_size = 0;
    std::vector<double> db_ms2_mz, db_ms2_intensity;
};

TargetRow read_target_row(const sdk::PluginProjectAccess::BatchView &batch, std::uint64_t row)
{
    TargetRow value;
    const auto read_text = [&](const char *name) {
        const auto column = batch.column_index(name);
        return batch.is_null(column, row) ? std::string{} : std::string(batch.text(column, row));
    };
    const auto read_real = [&](const char *name) {
        const auto column = batch.column_index(name);
        return batch.is_null(column, row) ? std::numeric_limits<double>::quiet_NaN() : batch.real(column, row);
    };
    const auto read_int = [&](const char *name) {
        const auto column = batch.column_index(name);
        return batch.is_null(column, row) ? 0 : static_cast<int>(batch.integer(column, row));
    };
    const auto read_list = [&](const char *name) {
        const auto column = batch.column_index(name);
        std::vector<double> result;
        if (batch.is_null(column, row)) return result;
        const auto size = batch.list_size(column, row);
        result.reserve(static_cast<std::size_t>(size));
        for (std::uint64_t index = 0; index < size; ++index)
            result.push_back(batch.list_is_null(column, row, index)
                ? std::numeric_limits<double>::quiet_NaN() : batch.list_real(column, row, index));
        return result;
    };
    value.name = read_text("name"); value.formula = read_text("formula");
    value.smiles = read_text("SMILES"); value.inchi = read_text("InChI");
    value.inchikey = read_text("InChIKey");
    value.has_name = !batch.is_null(batch.column_index("name"), row);
    value.has_formula = !batch.is_null(batch.column_index("formula"), row);
    value.has_smiles = !batch.is_null(batch.column_index("SMILES"), row);
    value.has_inchi = !batch.is_null(batch.column_index("InChI"), row);
    value.has_inchikey = !batch.is_null(batch.column_index("InChIKey"), row);
    value.mass = read_real("mass"); value.mz = read_real("mz"); value.rt = read_real("rt"); value.xlogp = read_real("xLogP");
    value.has_mass = !batch.is_null(batch.column_index("mass"), row);
    value.has_mz = !batch.is_null(batch.column_index("mz"), row);
    value.has_rt = !batch.is_null(batch.column_index("rt"), row);
    value.has_xlogp = !batch.is_null(batch.column_index("xLogP"), row);
    value.polarity = read_int("polarity");
    value.has_polarity = !batch.is_null(batch.column_index("polarity"), row);
    value.fragments_mz_pos = read_list("fragments_mz_pos");
    value.fragments_intensity_pos = read_list("fragments_intensity_pos");
    value.fragments_mz_neg = read_list("fragments_mz_neg");
    value.fragments_intensity_neg = read_list("fragments_intensity_neg");
    value.has_fragments_mz_pos = !batch.is_null(batch.column_index("fragments_mz_pos"), row);
    value.has_fragments_intensity_pos = !batch.is_null(batch.column_index("fragments_intensity_pos"), row);
    value.has_fragments_mz_neg = !batch.is_null(batch.column_index("fragments_mz_neg"), row);
    value.has_fragments_intensity_neg = !batch.is_null(batch.column_index("fragments_intensity_neg"), row);
    return value;
}

SuspectRow read_suspect_row(const sdk::PluginProjectAccess::BatchView &batch, std::uint64_t row)
{
    SuspectRow value;
    const auto read_text = [&](const char *name) {
        const auto column = batch.column_index(name);
        return batch.is_null(column, row) ? std::string{} : std::string(batch.text(column, row));
    };
    const auto read_real = [&](const char *name) {
        const auto column = batch.column_index(name);
        return batch.is_null(column, row) ? std::numeric_limits<double>::quiet_NaN() : batch.real(column, row);
    };
    const auto read_int = [&](const char *name) {
        const auto column = batch.column_index(name);
        return batch.is_null(column, row) ? 0 : static_cast<int>(batch.integer(column, row));
    };
    const auto read_list = [&](const char *name) {
        const auto column = batch.column_index(name);
        std::vector<double> result;
        if (batch.is_null(column, row)) return result;
        const auto size = batch.list_size(column, row);
        result.reserve(static_cast<std::size_t>(size));
        for (std::uint64_t index = 0; index < size; ++index)
            result.push_back(batch.list_is_null(column, row, index)
                ? std::numeric_limits<double>::quiet_NaN() : batch.list_real(column, row, index));
        return result;
    };
    value.analysis = read_text("analysis"); value.name = read_text("name");
    value.formula = read_text("formula"); value.smiles = read_text("SMILES");
    value.inchi = read_text("InChI"); value.inchikey = read_text("InChIKey");
    value.db_ms2_formula = read_text("db_ms2_formula"); value.db_mass = read_real("db_mass");
    value.exp_rt = read_real("exp_rt"); value.intensity = read_real("intensity");
    value.area = read_real("area"); value.error_mass = read_real("error_mass");
    value.cosine_similarity = read_real("cosine_similarity"); value.xlogp = read_real("xLogP");
    value.polarity = read_int("polarity"); value.id_level = read_int("id_level");
    value.candidate_rank = read_int("candidate_rank"); value.shared_fragments = read_int("shared_fragments");
    value.db_ms2_size = read_int("db_ms2_size"); value.db_ms2_mz = read_list("db_ms2_mz");
    value.db_ms2_intensity = read_list("db_ms2_intensity");
    return value;
}

std::string text(const TargetRow &row, const char *column)
{
    if (std::strcmp(column, "name") == 0) return row.name;
    if (std::strcmp(column, "formula") == 0) return row.formula;
    if (std::strcmp(column, "SMILES") == 0) return row.smiles;
    if (std::strcmp(column, "InChI") == 0) return row.inchi;
    if (std::strcmp(column, "InChIKey") == 0) return row.inchikey;
    return {};
}

std::string text(const SuspectRow &row, const char *column)
{
    if (std::strcmp(column, "analysis") == 0) return row.analysis;
    if (std::strcmp(column, "name") == 0) return row.name;
    if (std::strcmp(column, "formula") == 0) return row.formula;
    if (std::strcmp(column, "SMILES") == 0) return row.smiles;
    if (std::strcmp(column, "InChI") == 0) return row.inchi;
    if (std::strcmp(column, "InChIKey") == 0) return row.inchikey;
    if (std::strcmp(column, "db_ms2_formula") == 0) return row.db_ms2_formula;
    return {};
}

double number(const TargetRow &row, const char *column)
{
    if (std::strcmp(column, "mass") == 0) return row.mass;
    if (std::strcmp(column, "mz") == 0) return row.mz;
    if (std::strcmp(column, "rt") == 0) return row.rt;
    if (std::strcmp(column, "xLogP") == 0) return row.xlogp;
    return std::numeric_limits<double>::quiet_NaN();
}

double number(const SuspectRow &row, const char *column)
{
    if (std::strcmp(column, "db_mass") == 0) return row.db_mass;
    if (std::strcmp(column, "exp_rt") == 0) return row.exp_rt;
    if (std::strcmp(column, "intensity") == 0) return row.intensity;
    if (std::strcmp(column, "area") == 0) return row.area;
    if (std::strcmp(column, "error_mass") == 0) return row.error_mass;
    if (std::strcmp(column, "cosine_similarity") == 0) return row.cosine_similarity;
    if (std::strcmp(column, "xLogP") == 0) return row.xlogp;
    return std::numeric_limits<double>::quiet_NaN();
}

int integer(const SuspectRow &row, const char *column)
{
    if (std::strcmp(column, "polarity") == 0) return row.polarity;
    if (std::strcmp(column, "id_level") == 0) return row.id_level;
    if (std::strcmp(column, "candidate_rank") == 0) return row.candidate_rank;
    if (std::strcmp(column, "shared_fragments") == 0) return row.shared_fragments;
    if (std::strcmp(column, "db_ms2_size") == 0) return row.db_ms2_size;
    return 0;
}

std::vector<std::string> fragment_formulas(const std::string &encoded)
{
    std::vector<std::string> result;
    if (encoded.empty()) return result;
    std::istringstream stream(encoded);
    std::string formula;
    while (std::getline(stream, formula, ';'))
        result.push_back(formula);
    return result;
}

std::string identity(const TargetRow &row)
{
    for (const auto *column : {"InChIKey", "InChI", "SMILES"})
    {
        const auto value = text(row, column);
        if (!value.empty()) return std::string(column) + ":" + value;
    }
    if (!row.formula.empty() && std::isfinite(row.mass))
        return "formula:" + row.formula + ":" + std::to_string(std::round(row.mass * 10000.0) / 10000.0);
    return {};
}

std::string suspect_identity(const SuspectRow &row)
{
    for (const auto *column : {"InChIKey", "InChI", "SMILES"})
    {
        const auto value = text(row, column);
        if (!value.empty()) return std::string(column) + ":" + value;
    }
    if (!row.formula.empty() && std::isfinite(row.db_mass))
        return "formula:" + row.formula + ":" + std::to_string(std::round(row.db_mass * 10000.0) / 10000.0);
    return {};
}

double quality_number(const SuspectRow &row, const char *column)
{
    const auto value = number(row, column);
    return std::isfinite(value) ? value : -std::numeric_limits<double>::infinity();
}

bool better_representative(const SuspectRow &candidate, const SuspectRow &current, const double target_rt)
{
    if (std::isfinite(target_rt))
    {
        const auto candidate_distance = std::isfinite(candidate.exp_rt)
            ? std::abs(candidate.exp_rt - target_rt) : std::numeric_limits<double>::infinity();
        const auto current_distance = std::isfinite(current.exp_rt)
            ? std::abs(current.exp_rt - target_rt) : std::numeric_limits<double>::infinity();
        if (candidate_distance != current_distance) return candidate_distance < current_distance;
    }
    if (candidate.shared_fragments != current.shared_fragments)
        return candidate.shared_fragments > current.shared_fragments;
    const auto candidate_error = std::abs(quality_number(candidate, "error_mass"));
    const auto current_error = std::abs(quality_number(current, "error_mass"));
    if (candidate_error != current_error) return candidate_error < current_error;
    if (quality_number(candidate, "intensity") != quality_number(current, "intensity"))
        return quality_number(candidate, "intensity") > quality_number(current, "intensity");
    if (quality_number(candidate, "area") != quality_number(current, "area"))
        return quality_number(candidate, "area") > quality_number(current, "area");
    if (quality_number(candidate, "cosine_similarity") != quality_number(current, "cosine_similarity"))
        return quality_number(candidate, "cosine_similarity") > quality_number(current, "cosine_similarity");
    if (candidate.id_level != current.id_level) return candidate.id_level > current.id_level;
    if (candidate.candidate_rank != current.candidate_rank) return candidate.candidate_rank < current.candidate_rank;
    return candidate.name < current.name;
}

void add_spectrum(Evidence &evidence, const SuspectRow &row)
{
    const auto formulas = fragment_formulas(row.db_ms2_formula);
    const auto count = row.db_ms2_size > 0
        ? std::min(static_cast<std::size_t>(row.db_ms2_size), row.db_ms2_mz.size())
        : row.db_ms2_mz.size();
    if (row.db_ms2_mz.size() != row.db_ms2_intensity.size() || formulas.size() != row.db_ms2_mz.size()) return;
    for (std::size_t index = 0; index < count; ++index)
        if (!formulas[index].empty() && std::isfinite(row.db_ms2_mz[index]) &&
            std::isfinite(row.db_ms2_intensity[index]) && row.db_ms2_mz[index] > 0.0 &&
            row.db_ms2_intensity[index] >= 0.0)
            evidence.peaks.push_back({row.db_ms2_mz[index], row.db_ms2_intensity[index]});
}

std::vector<Peak> harmonize(std::vector<Peak> peaks, double tolerance)
{
    std::sort(peaks.begin(), peaks.end(), [](const Peak &left, const Peak &right) { return left.mz < right.mz; });
    std::vector<Peak> result;
    for (const auto &peak : peaks)
    {
        if (result.empty() || std::abs(peak.mz - result.back().mz) > tolerance)
            result.push_back(peak);
        else
        {
            auto &cluster = result.back();
            cluster.mz = (cluster.mz + peak.mz) / 2.0;
            cluster.intensity += peak.intensity;
        }
    }
    const auto maximum = result.empty() ? 0.0 : std::max_element(result.begin(), result.end(),
        [](const Peak &left, const Peak &right) { return left.intensity < right.intensity; })->intensity;
    for (auto &peak : result)
    {
        peak.mz = std::round(peak.mz * 10000.0) / 10000.0;
        peak.intensity = maximum > 0.0 ? std::round(100.0 * peak.intensity / maximum * 10.0) / 10.0 : 0.0;
    }
    return result;
}

void merge_metadata(TargetRow &target, const Evidence &evidence)
{
    if (!evidence.masses.empty())
        target.mass = std::accumulate(evidence.masses.begin(), evidence.masses.end(), 0.0) / evidence.masses.size();
    if (!evidence.rts.empty())
        target.rt = std::accumulate(evidence.rts.begin(), evidence.rts.end(), 0.0) / evidence.rts.size();
    if (!evidence.xlogp.empty())
        target.xlogp = std::accumulate(evidence.xlogp.begin(), evidence.xlogp.end(), 0.0) / evidence.xlogp.size();
}

void merge_spectrum(TargetRow &target, const Evidence &evidence, int polarity, double tolerance)
{
    const auto peaks = harmonize(evidence.peaks, tolerance);
    if (peaks.empty()) return;
    auto &mz = polarity > 0 ? target.fragments_mz_pos : target.fragments_mz_neg;
    auto &intensity = polarity > 0 ? target.fragments_intensity_pos : target.fragments_intensity_neg;
    if (polarity == 0) return;
    mz.clear(); intensity.clear();
    if (polarity > 0) {
        target.has_fragments_mz_pos = true; target.has_fragments_intensity_pos = true;
    } else {
        target.has_fragments_mz_neg = true; target.has_fragments_intensity_neg = true;
    }
    for (const auto &peak : peaks) { mz.push_back(peak.mz); intensity.push_back(peak.intensity); }
}
}

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto target_input = inputs.find("suspectTargetsTable");
    const auto suspects_input = inputs.find("suspectsTable");
    if (target_input == inputs.end() || !target_input->is_object() || !target_input->contains("result_ref"))
        throw std::invalid_argument("Connect a suspectTargetsTable input.");
    if (suspects_input == inputs.end() || !suspects_input->is_object() || !suspects_input->contains("result_ref"))
        throw std::invalid_argument("Connect a suspectsTable input.");

    const double tolerance = parameters.value("mz_clust", 0.003);
    const double minimum_cosine = parameters.value("min_cosine_similarity", 0.7);
    const int minimum_shared = parameters.value("min_shared_fragments", 3);
    if (!std::isfinite(tolerance) || tolerance <= 0.0)
        throw std::invalid_argument("mz_clust must be greater than zero");
    if (!std::isfinite(minimum_cosine) || minimum_cosine < 0.0 || minimum_cosine > 1.0)
        throw std::invalid_argument("min_cosine_similarity must be between zero and one");
    if (minimum_shared < 0)
        throw std::invalid_argument("min_shared_fragments must be non-negative");

    const std::vector<std::string> target_columns = {"name","mass","polarity","mz","rt","formula","SMILES","InChI","InChIKey","xLogP","fragments_mz_pos","fragments_intensity_pos","fragments_mz_neg","fragments_intensity_neg"};
    std::map<std::string, detail::TargetRow> targets;
    std::size_t target_count=0, suspect_count=0;
    access.read_typed_batches(target_input->at("result_ref").get<std::string>(),target_columns,[&](const sdk::PluginProjectAccess::BatchView &batch){
        for(std::uint64_t i=0;i<batch.row_count();++i){++target_count; auto row=detail::read_target_row(batch,i); const auto key=detail::identity(row); if(key.empty()) throw std::invalid_argument("Suspect target CSV contains a row without chemical identity"); targets[key]=std::move(row);}
    });
    std::set<std::string> original_keys; for(const auto &[key,row]:targets){(void)row;original_keys.insert(key);}
    const auto &suspect_columns=utils::suspects_columns();
    std::map<std::string,std::map<int,detail::Evidence>> evidence;
    std::map<std::string,detail::SuspectRow> representatives;
    access.read_typed_batches(suspects_input->at("result_ref").get<std::string>(),suspect_columns,[&](const sdk::PluginProjectAccess::BatchView &batch){
        for(std::uint64_t i=0;i<batch.row_count();++i){
            ++suspect_count; const auto row=detail::read_suspect_row(batch,i);
            if(row.db_ms2_size<=0||row.shared_fragments<minimum_shared||!std::isfinite(row.cosine_similarity)||row.cosine_similarity<minimum_cosine) continue;
            const auto key=detail::suspect_identity(row); if(key.empty()) continue;
            const auto found_target=targets.find(key); const auto rt=found_target==targets.end()?std::numeric_limits<double>::quiet_NaN():found_target->second.rt;
            const auto representative_key=row.analysis+"|"+key+"|polarity:"+std::to_string(row.polarity);
            const auto found=representatives.find(representative_key);
            if(found==representatives.end()||detail::better_representative(row,found->second,rt)) representatives[representative_key]=row;
        }
    });
    for(const auto &[rep_key,row]:representatives){(void)rep_key; const auto key=detail::suspect_identity(row); auto &e=evidence[key][row.polarity]; if(std::isfinite(row.db_mass)&&row.db_mass>0)e.masses.push_back(row.db_mass); if(std::isfinite(row.exp_rt))e.rts.push_back(row.exp_rt); if(std::isfinite(row.xlogp))e.xlogp.push_back(row.xlogp); detail::add_spectrum(e,row);}
    std::size_t added=0;
    for(const auto &[rep_key,s]:representatives){(void)rep_key;const auto key=detail::suspect_identity(s); if(key.empty()||targets.contains(key)||s.name.empty()||s.formula.empty()||(s.smiles.empty()&&s.inchi.empty())||!std::isfinite(s.db_mass)||s.db_mass<=0)continue;
        detail::TargetRow row; row.name=s.name;row.formula=s.formula;row.smiles=s.smiles;row.inchi=s.inchi;row.inchikey=s.inchikey;row.has_name=true;row.has_formula=true;row.has_smiles=!s.smiles.empty();row.has_inchi=!s.inchi.empty();row.has_inchikey=!s.inchikey.empty();row.mass=s.db_mass;row.has_mass=true;row.rt=s.exp_rt;row.has_rt=std::isfinite(s.exp_rt);row.xlogp=s.xlogp;row.has_xlogp=std::isfinite(s.xlogp);row.polarity=s.polarity;row.has_polarity=true;targets.emplace(key,std::move(row));++added;}
    std::size_t matched=0;
    for(const auto &[key,modes]:evidence){auto found=targets.find(key);if(found==targets.end())continue;if(original_keys.contains(key))++matched;detail::Evidence combined;
        for(const auto &[polarity,mode]:modes){combined.masses.insert(combined.masses.end(),mode.masses.begin(),mode.masses.end());combined.rts.insert(combined.rts.end(),mode.rts.begin(),mode.rts.end());combined.xlogp.insert(combined.xlogp.end(),mode.xlogp.begin(),mode.xlogp.end());detail::merge_spectrum(found->second,mode,polarity,tolerance);}
        detail::merge_metadata(found->second,combined);if(!combined.masses.empty())found->second.has_mass=true;if(!combined.rts.empty())found->second.has_rt=true;if(!combined.xlogp.empty())found->second.has_xlogp=true;}
    sdk::TableBatchBuilder output({{"name",STREAMFIND_PLUGIN_COLUMN_UTF8},{"mass",STREAMFIND_PLUGIN_COLUMN_FLOAT64},{"polarity",STREAMFIND_PLUGIN_COLUMN_INT64},{"mz",STREAMFIND_PLUGIN_COLUMN_FLOAT64},{"rt",STREAMFIND_PLUGIN_COLUMN_FLOAT64},{"formula",STREAMFIND_PLUGIN_COLUMN_UTF8},{"SMILES",STREAMFIND_PLUGIN_COLUMN_UTF8},{"InChI",STREAMFIND_PLUGIN_COLUMN_UTF8},{"InChIKey",STREAMFIND_PLUGIN_COLUMN_UTF8},{"xLogP",STREAMFIND_PLUGIN_COLUMN_FLOAT64},{"fragments_mz_pos",STREAMFIND_PLUGIN_COLUMN_UTF8,true,STREAMFIND_PLUGIN_COLUMN_FLOAT64},{"fragments_intensity_pos",STREAMFIND_PLUGIN_COLUMN_UTF8,true,STREAMFIND_PLUGIN_COLUMN_FLOAT64},{"fragments_mz_neg",STREAMFIND_PLUGIN_COLUMN_UTF8,true,STREAMFIND_PLUGIN_COLUMN_FLOAT64},{"fragments_intensity_neg",STREAMFIND_PLUGIN_COLUMN_UTF8,true,STREAMFIND_PLUGIN_COLUMN_FLOAT64}});
    using Cell=sdk::TableBatchBuilder::Cell; using List=std::vector<sdk::TableBatchBuilder::ListCell>;
    const auto tx=[](const std::string &v,bool has)->Cell{return has?Cell{v}:Cell{std::monostate{}};};
    const auto real=[](double v,bool has)->Cell{return has&&std::isfinite(v)?Cell{v}:Cell{std::monostate{}};};
    const auto list=[](const std::vector<double>&v,bool has)->Cell{if(!has)return std::monostate{};List out;out.reserve(v.size());for(double x:v)out.emplace_back(std::isfinite(x)?sdk::TableBatchBuilder::ListCell{x}:sdk::TableBatchBuilder::ListCell{std::monostate{}});return out;};
    for(const auto &[key,r]:targets){(void)key;output.append_row({tx(r.name,r.has_name),real(r.mass,r.has_mass),r.has_polarity?Cell{static_cast<std::int64_t>(r.polarity)}:Cell{std::monostate{}},real(r.mz,r.has_mz),real(r.rt,r.has_rt),tx(r.formula,r.has_formula),tx(r.smiles,r.has_smiles),tx(r.inchi,r.has_inchi),tx(r.inchikey,r.has_inchikey),real(r.xlogp,r.has_xlogp),list(r.fragments_mz_pos,r.has_fragments_mz_pos),list(r.fragments_intensity_pos,r.has_fragments_intensity_pos),list(r.fragments_mz_neg,r.has_fragments_mz_neg),list(r.fragments_intensity_neg,r.has_fragments_intensity_neg)});}
    output.emit(access,"suspectTargetsTable"); access.report_progress(1.0,"Updated suspect targets with connected suspect evidence.");
    return Json{{"status","finished"},{"target_rows_before",target_count},{"suspect_rows",suspect_count},{"validated_suspect_rows",representatives.size()},{"matched_targets",matched},{"added_targets",added},{"target_rows",targets.size()}};
}
}

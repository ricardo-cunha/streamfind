//! Portable mass-spectrometry file reader.
//!
//! mzML and mzXML are decoded without vendor SDKs.  The API deliberately keeps
//! the same useful boundary as the C++ reader: a file summary, spectrum
//! headers, and the `m/z` and intensity arrays for each spectrum.

use base64::{engine::general_purpose::STANDARD, Engine as _};
use flate2::read::ZlibDecoder;
use quick_xml::{events::Event, Reader as XmlReader};
use std::{
    collections::BTreeSet,
    env, fs,
    io::{self, Read},
    path::{Path, PathBuf},
};

fn trace_payload_decode(kind: &str, index: usize) {
    if env::var("STREAMFIND_TRACE_PAYLOAD_DECODES")
        .map(|value| !value.is_empty() && value != "0")
        .unwrap_or(false)
    {
        eprintln!("STREAMFIND_PAYLOAD_DECODE kind={kind} index={index}");
    }
}

#[derive(Debug)]
pub enum ReaderError {
    Io(io::Error),
    Xml(quick_xml::Error),
    Invalid(String),
    Unsupported(String),
}

impl std::fmt::Display for ReaderError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "I/O error: {e}"),
            Self::Xml(e) => write!(f, "XML error: {e}"),
            Self::Invalid(e) => f.write_str(e),
            Self::Unsupported(e) => write!(f, "unsupported mass spectrometry format: {e}"),
        }
    }
}

impl std::error::Error for ReaderError {}
impl From<io::Error> for ReaderError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}
impl From<quick_xml::Error> for ReaderError {
    fn from(e: quick_xml::Error) -> Self {
        Self::Xml(e)
    }
}

pub type Result<T> = std::result::Result<T, ReaderError>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Format {
    MzMl,
    MzXml,
    Asc,
    ShimadzuLcd,
    SciexWiff,
    AgilentMassHunterD,
    AgilentChemStationD,
    BrukerTsf,
    BrukerBaf,
    ThermoRaw,
}

#[derive(Debug, Clone, Default)]
pub struct Spectrum {
    pub index: i32,
    pub scan: i32,
    pub array_length: i32,
    pub level: i32,
    pub mode: i32,
    pub configuration: i32,
    pub window_mz: f32,
    pub window_mzlow: f32,
    pub window_mzhigh: f32,
    pub polarity: i32,
    pub low_mz: f32,
    pub high_mz: f32,
    pub base_peak_mz: f32,
    pub base_peak_intensity: f32,
    pub tic: f32,
    pub retention_time: f32,
    pub mobility: f32,
    pub precursor_mz: f32,
    pub precursor_intensity: f32,
    pub precursor_charge: i32,
    pub collision_energy: f32,
    pub mz: Vec<f32>,
    pub intensity: Vec<f32>,
}

#[derive(Debug, Clone, Default)]
pub struct Chromatogram {
    pub id: String,
    pub array_length: i32,
    pub signal_type: String,
    pub chromatogram_type: String,
    pub detector: String,
    pub channel: String,
    pub units: String,
    pub wavelength_nm: f32,

    pub polarity: i32,
    pub interval_ms: f32,
    pub time: Vec<f32>,
    pub intensity: Vec<f32>,
    pub precursor_mz: Option<f32>,
    pub product_mz: Option<f32>,
    pub activation_ce: Option<f32>,
    pub start_time: Option<f32>,
    pub end_time: Option<f32>,
}

#[derive(Debug, Clone)]
pub struct Summary {
    pub file_name: String,
    pub file_path: PathBuf,
    pub format: Format,
    pub number_spectra: usize,
    pub number_chromatograms: usize,
    pub number_spectra_binary_arrays: usize,
    pub min_mz: f32,
    pub max_mz: f32,
    pub start_rt: f32,
    pub end_rt: f32,
    pub has_ion_mobility: bool,
    pub time_stamp: String,
}

fn convert_chromatogram_minutes_to_seconds(chromatograms: &mut [Chromatogram]) {
    for chromatogram in chromatograms {
        for value in &mut chromatogram.time {
            *value *= 60.0;
        }
        if let Some(value) = &mut chromatogram.start_time {
            *value *= 60.0;
        }
        if let Some(value) = &mut chromatogram.end_time {
            *value *= 60.0;
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Analysis {
    pub analysis_index: usize,
    pub source_analysis_number: Option<usize>,
    pub name: String,
    pub analysis_count: usize,
}

pub struct Reader {
    path: PathBuf,
    format: Format,
    analysis_catalog: Vec<Analysis>,
    selected_analysis: usize,
    spectra: Vec<Spectrum>,
    chromatograms: Vec<Chromatogram>,
    agilent_records: Option<Vec<crate::reader_agilent::ScanRecord>>,
    agilent_ims_records: Option<Vec<crate::reader_agilent_ims::ScanRecord>>,
    chemstation_data: Option<crate::reader_agilent_chemstation::DataFile>,
    bruker_baf_spectra: Option<Vec<crate::reader_bruker::BafSpectrumMetadata>>,
    bruker_tsf_frames: Option<Vec<crate::reader_bruker::TsfFrame>>,
    bruker_tsf_msms: Option<Vec<crate::reader_bruker::TsfMsMsInfo>>,
    sciex_tof_metadata: Option<crate::reader_sciex::TofMetadata>,
    sciex_mrm_metadata: Option<crate::reader_sciex::MrmMetadata>,
    mzml_bytes: Option<Vec<u8>>,
    mzml_spectrum_offsets: Vec<(usize, usize)>,
    mzml_arrays_loaded: bool,
    thermo_metadata: Option<crate::reader_thermo::ThermoMetadata>,
}

impl Reader {
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        let agilent_ims = crate::reader_agilent_ims::is_agilent_ion_mobility_directory(&path);
        let agilent =
            !agilent_ims && crate::reader_agilent::is_agilent_mass_hunter_directory(&path);
        let chemstation = crate::reader_agilent_chemstation::is_chemstation_directory(&path);
        let bruker_tsf =
            crate::reader_bruker::detect_family(&path) == crate::reader_bruker::Family::Tsf;
        let bruker_baf =
            crate::reader_bruker::detect_family(&path) == crate::reader_bruker::Family::Baf;
        let bytes = if agilent || agilent_ims || chemstation || bruker_tsf || bruker_baf {
            Vec::new()
        } else {
            fs::read(&path)?
        };
        let format = if agilent || agilent_ims {
            Format::AgilentMassHunterD
        } else if chemstation {
            Format::AgilentChemStationD
        } else if bruker_tsf {
            Format::BrukerTsf
        } else if bruker_baf {
            Format::BrukerBaf
        } else if crate::reader_thermo::is_thermo_raw(&path, &bytes) {
            Format::ThermoRaw
        } else {
            detect_format(&path, &bytes)?
        };
        let (mut spectra, mut chromatograms) = match format {
            Format::MzMl => (
                parse_mzml_with_arrays(&bytes, false)?,
                parse_mzml_chromatograms(&bytes, Some(&[]))?,
            ),
            Format::MzXml => (parse_mzxml(&bytes)?, Vec::new()),
            Format::Asc => (Vec::new(), parse_asc(&bytes)),
            Format::ShimadzuLcd => parse_lcd(&path)?,
            Format::SciexWiff => (Vec::new(), Vec::new()),
            Format::AgilentMassHunterD => (Vec::new(), Vec::new()),
            Format::AgilentChemStationD => (Vec::new(), Vec::new()),
            Format::BrukerTsf => (Vec::new(), Vec::new()),
            Format::BrukerBaf => (Vec::new(), Vec::new()),
            Format::ThermoRaw => (Vec::new(), Vec::new()),
        };
        let analysis_name = path
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        let agilent_records = if agilent {
            Some(crate::reader_agilent::read_scan_records(&path).map_err(ReaderError::Invalid)?)
        } else {
            None
        };
        let agilent_ims_records = if agilent_ims {
            Some(
                crate::reader_agilent_ims::read_scan_records(&path)
                    .map_err(ReaderError::Invalid)?,
            )
        } else {
            None
        };
        let chemstation_data = if format == Format::AgilentChemStationD
            && crate::reader_agilent_chemstation::has_ms_data_file(&path)
        {
            Some(
                crate::reader_agilent_chemstation::open_directory(&path)
                    .map_err(ReaderError::Invalid)?,
            )
        } else {
            None
        };
        let bruker_baf_spectra = if bruker_baf {
            Some(
                crate::reader_bruker::read_baf_spectra_metadata(&path)
                    .map_err(ReaderError::Invalid)?,
            )
        } else {
            None
        };
        let bruker_tsf_frames = if bruker_tsf {
            Some(crate::reader_bruker::read_tsf_frames(&path).map_err(ReaderError::Invalid)?)
        } else {
            None
        };
        let bruker_tsf_msms = if bruker_tsf {
            Some(crate::reader_bruker::read_tsf_msms_info(&path).map_err(ReaderError::Invalid)?)
        } else {
            None
        };
        if format == Format::AgilentChemStationD {
            chromatograms = crate::reader_agilent_chemstation::read_chromatograms(&path)
                .map_err(ReaderError::Invalid)?;
            convert_chromatogram_minutes_to_seconds(&mut chromatograms);
        }
        if let Some(records) = &agilent_records {
            spectra = records
                .iter()
                .enumerate()
                .map(|(index, record)| Spectrum {
                    index: index as i32,
                    scan: record.scan_id as i32,
                    array_length: if crate::reader_agilent::has_centroid(record) {
                        record.centroid_point_count
                    } else {
                        record.spectrum_point_count
                    } as i32,
                    level: record.ms_level,
                    polarity: record.polarity,
                    low_mz: record.spectrum_min_x as f32,
                    high_mz: record.spectrum_max_x as f32,
                    base_peak_mz: record.base_peak_mz as f32,
                    base_peak_intensity: record.base_peak_value as f32,
                    tic: record.tic as f32,
                    retention_time: record.scan_time_minutes as f32 * 60.0,
                    precursor_mz: record.precursor_mz as f32,
                    precursor_intensity: record.precursor_intensity as f32,
                    collision_energy: record.collision_energy as f32,
                    ..Default::default()
                })
                .collect();
        }
        if format == Format::AgilentMassHunterD {
            chromatograms = crate::reader_agilent::read_dad_chromatograms(&path)
                .map_err(ReaderError::Invalid)?;
        }
        if let Some(records) = &agilent_ims_records {
            spectra = records
                .iter()
                .enumerate()
                .map(|(index, record)| Spectrum {
                    index: index as i32,
                    scan: record.scan_id as i32,
                    array_length: record.profile_point_count as i32,
                    level: 1,
                    polarity: 1,
                    base_peak_mz: record.base_peak_mz as f32,
                    base_peak_intensity: record.base_peak_abundance as f32,
                    tic: record.tic as f32,
                    retention_time: record.scan_time_minutes as f32 * 60.0,
                    mobility: record.mobility as f32,
                    ..Default::default()
                })
                .collect();
        }
        if let Some(data) = &chemstation_data {
            spectra = data
                .index
                .iter()
                .enumerate()
                .map(|(index, entry)| {
                    let raw = crate::reader_agilent_chemstation::read_spectrum(data, index)
                        .map_err(ReaderError::Invalid)?;
                    let (low_mz, high_mz) = raw
                        .mz
                        .iter()
                        .copied()
                        .fold((f32::INFINITY, 0.0f32), |(low, high), value| {
                            (low.min(value), high.max(value))
                        });
                    let (base_peak_mz, base_peak_intensity) = raw
                        .intensity
                        .iter()
                        .enumerate()
                        .max_by(|(_, left), (_, right)| left.total_cmp(right))
                        .map(|(point, intensity)| (raw.mz[point], *intensity))
                        .unwrap_or((0.0, 0.0));
                    Ok(Spectrum {
                        index: index as i32,
                        scan: index as i32 + 1,
                        array_length: raw.mz.len() as i32,
                        level: 1,
                        low_mz: if low_mz.is_finite() { low_mz } else { 0.0 },
                        high_mz,
                        base_peak_mz,
                        base_peak_intensity,
                        tic: raw.intensity.iter().sum(),
                        retention_time: entry.retention_time_ms as f32 / 1000.0,
                        ..Default::default()
                    })
                })
                .collect::<Result<Vec<_>>>()?;
        }
        if let Some(metadata) = &bruker_baf_spectra {
            spectra = metadata
                .iter()
                .enumerate()
                .map(|(index, value)| -> Result<Spectrum> {
                    Ok(Spectrum {
                        index: index as i32,
                        scan: value.id as i32,
                        array_length: value.profile_point_count as i32,
                        level: value.ms_level,
                        mode: value.scan_mode,
                        configuration: value.acquisition_mode,
                        polarity: value.polarity,
                        low_mz: value.mz_lower as f32,
                        high_mz: value.mz_upper as f32,
                        base_peak_intensity: value.maximum_intensity as f32,
                        tic: value.summed_intensity as f32,
                        retention_time: value.retention_time as f32,
                        ..Default::default()
                    })
                })
                .collect::<Result<Vec<_>>>()?;
        }
        if let Some(frames) = &bruker_tsf_frames {
            let msms = bruker_tsf_msms.as_deref().unwrap_or_default();
            spectra = frames
                .iter()
                .enumerate()
                .map(|(index, frame)| {
                    let info = msms.iter().find(|value| value.frame == frame.id);
                    Spectrum {
                        index: index as i32,
                        scan: frame.id as i32,
                        array_length: frame.num_peaks,
                        level: if frame.msms_type == 0 { 1 } else { 2 },
                        mode: frame.scan_mode,
                        configuration: 0,
                        polarity: match frame.polarity.as_str() {
                            "+" => 1,
                            "-" => -1,
                            _ => 0,
                        },
                        low_mz: 95.0,
                        high_mz: 2505.0,
                        base_peak_intensity: frame.max_intensity as f32,
                        tic: frame.summed_intensities as f32,
                        retention_time: frame.retention_time as f32,
                        precursor_mz: info.map(|value| value.trigger_mass as f32).unwrap_or(0.0),
                        window_mz: info.map(|value| value.trigger_mass as f32).unwrap_or(0.0),
                        window_mzlow: info
                            .map(|value| (value.trigger_mass - value.isolation_width / 2.0) as f32)
                            .unwrap_or(0.0),
                        window_mzhigh: info
                            .map(|value| (value.trigger_mass + value.isolation_width / 2.0) as f32)
                            .unwrap_or(0.0),
                        precursor_charge: info.map(|value| value.precursor_charge).unwrap_or(0),
                        collision_energy: info
                            .map(|value| value.collision_energy as f32)
                            .unwrap_or(0.0),
                        ..Default::default()
                    }
                })
                .collect();
        }
        let analysis_catalog = if format == Format::SciexWiff {
            read_sciex_analysis_catalog(&path)?
        } else {
            vec![Analysis {
                analysis_index: 0,
                source_analysis_number: None,
                name: analysis_name,
                analysis_count: 1,
            }]
        };
        let sciex_tof_metadata = if format == Format::SciexWiff {
            let source = analysis_catalog[0].source_analysis_number.unwrap_or(1);
            match crate::reader_sciex::read_tof_metadata(&path, source) {
                Ok(metadata) => {
                    spectra = metadata
                        .public_indices
                        .iter()
                        .enumerate()
                        .map(|(index, source_index)| {
                            let record = &metadata.records[*source_index];
                            Spectrum {
                                index: index as i32,
                                scan: *source_index as i32,
                                array_length: -1,
                                level: if metadata.experiment_count > 1
                                    && source_index % metadata.experiment_count == 0
                                {
                                    1
                                } else {
                                    2
                                },
                                polarity: 1,
                                retention_time: record.retention_time_minutes * 60.0,
                                precursor_mz: crate::reader_sciex::tof_precursor_for_index(
                                    &metadata, index,
                                ),
                                precursor_intensity:
                                    crate::reader_sciex::tof_precursor_intensity_for_index(
                                        &metadata, index,
                                    ),
                                tic: record.tic as f32,
                                ..Default::default()
                            }
                        })
                        .collect();
                    chromatograms = Vec::new();
                    Some(metadata)
                }
                Err(_) => {
                    chromatograms = load_sciex_chromatograms(&path, &analysis_catalog[0])?;
                    None
                }
            }
        } else {
            None
        };
        let sciex_mrm_metadata = if format == Format::SciexWiff && sciex_tof_metadata.is_none() {
            let source = analysis_catalog[0].source_analysis_number.unwrap_or(1);
            match crate::reader_sciex::read_mrm_metadata(&path, source) {
                Ok(metadata) => {
                    chromatograms = render_sciex_mrm_headers(&metadata.transitions);
                    Some(metadata)
                }
                Err(_) => {
                    chromatograms = load_sciex_chromatograms(&path, &analysis_catalog[0])?;
                    None
                }
            }
        } else {
            None
        };
        let thermo_metadata = if format == Format::ThermoRaw {
            let metadata =
                crate::reader_thermo::read_metadata(&path, &bytes).map_err(ReaderError::Invalid)?;
            spectra = metadata
                .scans
                .iter()
                .enumerate()
                .map(|(index, scan)| Spectrum {
                    index: index as i32,
                    scan: scan.scan,
                    array_length: scan.centroid_count as i32,
                    level: scan.level,
                    mode: scan.mode,
                    polarity: scan.polarity,
                    window_mz: if scan.level >= 2 {
                        scan.precursor_mz as f32
                    } else {
                        0.0
                    },
                    window_mzlow: if scan.level >= 2 && scan.precursor_mz > 0.0 {
                        (scan.precursor_mz - scan.isolation_width as f64 / 2.0) as f32
                    } else {
                        0.0
                    },
                    window_mzhigh: if scan.level >= 2 && scan.precursor_mz > 0.0 {
                        (scan.precursor_mz + scan.isolation_width as f64 / 2.0) as f32
                    } else {
                        0.0
                    },
                    precursor_mz: scan.precursor_mz as f32,
                    precursor_charge: scan.precursor_charge,
                    collision_energy: scan.collision_energy,
                    low_mz: scan.low_mz,
                    high_mz: scan.high_mz,
                    base_peak_mz: scan.base_peak_mz,
                    base_peak_intensity: scan.base_peak_intensity,
                    tic: scan.tic,
                    retention_time: scan.retention_time,
                    ..Default::default()
                })
                .collect();
            chromatograms = metadata.chromatograms.clone();
            Some(metadata)
        } else {
            None
        };
        let mzml_spectrum_offsets = if format == Format::MzMl {
            mzml_spectrum_offsets(&bytes)?
        } else {
            Vec::new()
        };
        let mzml_bytes = (format == Format::MzMl).then_some(bytes);
        Ok(Self {
            path,
            format,
            analysis_catalog,
            selected_analysis: 0,
            spectra,
            chromatograms,
            agilent_records,
            agilent_ims_records,
            chemstation_data,
            bruker_baf_spectra,
            bruker_tsf_frames,
            bruker_tsf_msms,
            sciex_tof_metadata,
            sciex_mrm_metadata,
            mzml_bytes,
            mzml_spectrum_offsets,
            mzml_arrays_loaded: false,
            thermo_metadata,
        })
    }

    pub fn format(&self) -> Format {
        self.format
    }
    /// Return the acquisition timestamp in the same RFC3339 representation as
    /// the C++ reader. For Thermo RAW this is the audit-start FILETIME, which
    /// records the acquisition instant in UTC.
    pub fn get_time_stamp(&self) -> String {
        self.summary().time_stamp
    }
    pub fn analysis_catalog(&self) -> &[Analysis] {
        &self.analysis_catalog
    }
    pub fn select_analysis(&mut self, index: usize) -> Result<()> {
        if index >= self.analysis_catalog.len() {
            return Err(ReaderError::Invalid(format!(
                "mass spectrometry analysis index is out of range: {index}"
            )));
        }
        self.selected_analysis = index;
        if self.format == Format::SciexWiff {
            let source = self.analysis_catalog[index]
                .source_analysis_number
                .unwrap_or(1);
            match crate::reader_sciex::read_tof_metadata(&self.path, source) {
                Ok(metadata) => {
                    self.spectra = metadata
                        .public_indices
                        .iter()
                        .enumerate()
                        .map(|(public_index, source_index)| {
                            let record = &metadata.records[*source_index];
                            Spectrum {
                                index: public_index as i32,
                                scan: *source_index as i32,
                                array_length: -1,
                                level: if metadata.experiment_count > 1
                                    && source_index % metadata.experiment_count == 0
                                {
                                    1
                                } else {
                                    2
                                },
                                polarity: 1,
                                retention_time: record.retention_time_minutes * 60.0,
                                precursor_mz: crate::reader_sciex::tof_precursor_for_index(
                                    &metadata,
                                    public_index,
                                ),
                                precursor_intensity:
                                    crate::reader_sciex::tof_precursor_intensity_for_index(
                                        &metadata,
                                        public_index,
                                    ),
                                tic: record.tic as f32,
                                ..Default::default()
                            }
                        })
                        .collect();
                    self.sciex_tof_metadata = Some(metadata);
                    self.chromatograms.clear();
                }
                Err(_) => {
                    self.sciex_tof_metadata = None;
                    match crate::reader_sciex::read_mrm_metadata(&self.path, source) {
                        Ok(metadata) => {
                            self.chromatograms = render_sciex_mrm_headers(&metadata.transitions);
                            self.sciex_mrm_metadata = Some(metadata);
                        }
                        Err(_) => {
                            self.sciex_mrm_metadata = None;
                            self.chromatograms = load_sciex_chromatograms(
                                &self.path,
                                &self.analysis_catalog[index],
                            )?;
                        }
                    }
                }
            }
        }
        Ok(())
    }
    pub fn selected_analysis_index(&self) -> usize {
        self.selected_analysis
    }
    pub fn spectra(&self) -> &[Spectrum] {
        &self.spectra
    }
    pub fn chromatograms(&self) -> &[Chromatogram] {
        &self.chromatograms
    }

    pub fn chromatograms_data(&self, indices: &[usize]) -> Result<Vec<Chromatogram>> {
        if let Some(metadata) = &self.sciex_mrm_metadata {
            let all = load_sciex_chromatograms(
                &self.path,
                &Analysis {
                    analysis_index: self.selected_analysis,
                    source_analysis_number: Some(metadata.source_analysis_number),
                    name: String::new(),
                    analysis_count: 1,
                },
            )?;
            return Ok(if indices.is_empty() {
                all
            } else {
                indices
                    .iter()
                    .filter_map(|index| all.get(*index).cloned())
                    .collect()
            });
        }
        if self.format == Format::MzMl {
            let bytes = self
                .mzml_bytes
                .as_deref()
                .ok_or_else(|| ReaderError::Invalid("mzML bytes are not loaded".into()))?;
            let decoded = parse_mzml_chromatograms(
                bytes,
                if indices.is_empty() {
                    None
                } else {
                    Some(indices)
                },
            )?;
            return Ok(if indices.is_empty() {
                decoded
            } else {
                indices
                    .iter()
                    .filter_map(|index| decoded.get(*index).cloned())
                    .collect()
            });
        }
        Ok(if indices.is_empty() {
            self.chromatograms.clone()
        } else {
            indices
                .iter()
                .filter_map(|index| self.chromatograms.get(*index).cloned())
                .collect()
        })
    }

    pub fn spectrum(&self, index: usize) -> Option<&Spectrum> {
        self.spectra.get(index)
    }

    pub fn load_mzml_spectrum_data(&mut self) -> Result<()> {
        if self.format != Format::MzMl || self.mzml_arrays_loaded {
            return Ok(());
        }
        let bytes = self
            .mzml_bytes
            .as_deref()
            .ok_or_else(|| ReaderError::Invalid("mzML bytes are not loaded".into()))?;
        self.spectra = parse_mzml_with_arrays(bytes, true)?;
        self.mzml_arrays_loaded = true;
        Ok(())
    }

    pub fn spectrum_data(&self, index: usize) -> Result<Spectrum> {
        trace_payload_decode("spectrum", index);
        if self.mzml_arrays_loaded {
            return self.spectra.get(index).cloned().ok_or_else(|| {
                ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
            });
        }
        if let Some(bytes) = &self.mzml_bytes {
            let (start, end) = self
                .mzml_spectrum_offsets
                .get(index)
                .copied()
                .ok_or_else(|| {
                    ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
                })?;
            return parse_mzml_with_arrays(&bytes[start..end], true)?
                .into_iter()
                .next()
                .ok_or_else(|| ReaderError::Invalid(format!("mzML spectrum is empty: {index}")));
        }
        if self.format == Format::AgilentChemStationD {
            let data = self.chemstation_data.as_ref().ok_or_else(|| {
                ReaderError::Invalid("Agilent ChemStation data is not loaded".into())
            })?;
            let raw = crate::reader_agilent_chemstation::read_spectrum(data, index)
                .map_err(ReaderError::Invalid)?;
            let (low_mz, high_mz) = raw
                .mz
                .iter()
                .copied()
                .fold((f32::INFINITY, 0.0f32), |(low, high), value| {
                    (low.min(value), high.max(value))
                });
            let (base_peak_mz, base_peak_intensity) = raw
                .intensity
                .iter()
                .enumerate()
                .max_by(|(_, left), (_, right)| left.total_cmp(right))
                .map(|(point, intensity)| (raw.mz[point], *intensity))
                .unwrap_or((0.0, 0.0));
            return Ok(Spectrum {
                index: index as i32,
                scan: index as i32 + 1,
                array_length: raw.mz.len() as i32,
                level: 1,
                low_mz: if low_mz.is_finite() { low_mz } else { 0.0 },
                high_mz,
                base_peak_mz,
                base_peak_intensity,
                tic: raw.intensity.iter().sum(),
                retention_time: raw.retention_time_ms as f32 / 1000.0,
                mz: raw.mz,
                intensity: raw.intensity,
                ..Default::default()
            });
        }
        if let Some(records) = &self.agilent_ims_records {
            let record = records.get(index).ok_or_else(|| {
                ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
            })?;
            let profile = crate::reader_agilent_ims::read_profile_spectrum(&self.path, record)
                .map_err(ReaderError::Invalid)?;
            let (low_mz, high_mz) = profile
                .mz
                .iter()
                .copied()
                .fold((f32::INFINITY, 0.0f32), |(low, high), value| {
                    (low.min(value), high.max(value))
                });
            let (base_peak_mz, base_peak_intensity) = profile
                .intensity
                .iter()
                .enumerate()
                .max_by(|(_, left), (_, right)| left.total_cmp(right))
                .map(|(point, intensity)| (profile.mz[point], *intensity))
                .unwrap_or((0.0, 0.0));
            return Ok(Spectrum {
                index: index as i32,
                scan: record.scan_id as i32,
                array_length: profile.mz.len() as i32,
                level: 1,
                polarity: 1,
                low_mz: if low_mz.is_finite() { low_mz } else { 0.0 },
                high_mz,
                base_peak_mz,
                base_peak_intensity,
                tic: profile.intensity.iter().sum(),
                retention_time: record.scan_time_minutes as f32 * 60.0,
                mobility: record.mobility as f32,
                mz: profile.mz,
                intensity: profile.intensity,
                ..Default::default()
            });
        }
        if let Some(metadata) = &self.sciex_tof_metadata {
            return crate::reader_sciex::read_tof_spectrum(&self.path, metadata, index)
                .map_err(|error| error);
        }
        if let Some(frames) = &self.bruker_tsf_frames {
            let frame = frames.get(index).ok_or_else(|| {
                ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
            })?;
            let raw = crate::reader_bruker::read_tsf_line_spectrum(&self.path, frame)
                .map_err(ReaderError::Invalid)?;
            let calibration = crate::reader_bruker::read_tsf_calibration(&self.path, frame)
                .map_err(ReaderError::Invalid)?;
            let mz = crate::reader_bruker::tsf_tof_to_mz(&calibration, &raw.tof);
            let info = self
                .bruker_tsf_msms
                .as_deref()
                .unwrap_or_default()
                .iter()
                .find(|value| value.frame == frame.id);
            let (base_peak_mz, base_peak_intensity) = raw
                .intensity
                .iter()
                .enumerate()
                .max_by(|(_, left), (_, right)| left.total_cmp(right))
                .map(|(point, intensity)| (mz[point] as f32, *intensity as f32))
                .unwrap_or((0.0, 0.0));
            let polarity = match frame.polarity.as_str() {
                "+" => 1,
                "-" => -1,
                _ => 0,
            };
            return Ok(Spectrum {
                index: index as i32,
                scan: frame.id as i32,
                array_length: mz.len() as i32,
                level: if frame.msms_type == 0 { 1 } else { 2 },
                mode: frame.scan_mode,
                configuration: 0,
                polarity,
                low_mz: mz.first().copied().unwrap_or(0.0) as f32,
                high_mz: mz.last().copied().unwrap_or(0.0) as f32,
                base_peak_mz,
                base_peak_intensity,
                tic: raw.intensity.iter().map(|value| *value as f32).sum(),
                retention_time: frame.retention_time as f32,
                precursor_mz: info.map(|value| value.trigger_mass as f32).unwrap_or(0.0),
                window_mz: info.map(|value| value.trigger_mass as f32).unwrap_or(0.0),
                window_mzlow: info
                    .map(|value| (value.trigger_mass - value.isolation_width / 2.0) as f32)
                    .unwrap_or(0.0),
                window_mzhigh: info
                    .map(|value| (value.trigger_mass + value.isolation_width / 2.0) as f32)
                    .unwrap_or(0.0),
                precursor_charge: info.map(|value| value.precursor_charge).unwrap_or(0),
                collision_energy: info
                    .map(|value| value.collision_energy as f32)
                    .unwrap_or(0.0),
                mz: mz.into_iter().map(|value| value as f32).collect(),
                intensity: raw
                    .intensity
                    .into_iter()
                    .map(|value| value as f32)
                    .collect(),
                ..Default::default()
            });
        }
        if let Some(metadata) = &self.bruker_baf_spectra {
            let value = metadata.get(index).ok_or_else(|| {
                ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
            })?;
            let profile = crate::reader_bruker::read_baf_profile_spectrum(
                &self.path,
                value.profile_intensity_id,
            )
            .map_err(ReaderError::Invalid)?;
            let length = profile.intensity.len();
            let step = if length > 1 {
                (value.mz_upper - value.mz_lower) as f64 / (length - 1) as f64
            } else {
                0.0
            };
            let mz = (0..length)
                .map(|point| (value.mz_lower as f64 + step * point as f64) as f32)
                .collect::<Vec<_>>();
            let mut spectrum = Spectrum {
                index: index as i32,
                scan: value.id as i32,
                array_length: length as i32,
                level: value.ms_level,
                polarity: value.polarity,
                low_mz: value.mz_lower as f32,
                high_mz: value.mz_upper as f32,
                base_peak_intensity: value.maximum_intensity as f32,
                tic: value.summed_intensity as f32,
                retention_time: value.retention_time as f32,
                mz,
                intensity: profile
                    .intensity
                    .into_iter()
                    .map(|value| value as f32)
                    .collect(),
                ..Default::default()
            };
            if let Some((peak, intensity)) = spectrum
                .intensity
                .iter()
                .enumerate()
                .max_by(|(_, left), (_, right)| left.total_cmp(right))
            {
                spectrum.base_peak_intensity = *intensity;
                spectrum.base_peak_mz = spectrum.mz[peak];
                spectrum.tic = spectrum.intensity.iter().sum();
            }
            return Ok(spectrum);
        }
        if let Some(metadata) = &self.thermo_metadata {
            let scan = metadata.scans.get(index).ok_or_else(|| {
                ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
            })?;
            let spectrum = if scan.centroid_count > 0 {
                crate::reader_thermo::read_spectrum(&self.path, scan, index)
            } else {
                crate::reader_thermo::read_profile_spectrum(&self.path, scan, index)
            };
            return spectrum.map_err(ReaderError::Invalid);
        }
        if self.format != Format::AgilentMassHunterD {
            return self.spectra.get(index).cloned().ok_or_else(|| {
                ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
            });
        }
        let record = self
            .agilent_records
            .as_ref()
            .and_then(|records| records.get(index))
            .ok_or_else(|| {
                ReaderError::Invalid(format!("spectrum index is out of range: {index}"))
            })?;
        let profile = if crate::reader_agilent::has_centroid(record) {
            crate::reader_agilent::read_centroid_spectrum(&self.path, record)
        } else {
            crate::reader_agilent::read_profile_spectrum(&self.path, record)
        }
        .map_err(ReaderError::Invalid)?;
        let mut spectrum = Spectrum {
            index: index as i32,
            scan: record.scan_id as i32,
            array_length: profile.mz.len() as i32,
            level: record.ms_level,
            polarity: record.polarity,
            low_mz: record.spectrum_min_x as f32,
            high_mz: record.spectrum_max_x as f32,
            base_peak_mz: record.base_peak_mz as f32,
            base_peak_intensity: record.base_peak_value as f32,
            tic: record.tic as f32,
            retention_time: record.scan_time_minutes as f32 * 60.0,
            precursor_mz: record.precursor_mz as f32,
            precursor_intensity: record.precursor_intensity as f32,
            collision_energy: record.collision_energy as f32,
            mz: profile.mz.into_iter().map(|value| value as f32).collect(),
            intensity: profile.intensity,
            ..Default::default()
        };
        if !spectrum.mz.is_empty() {
            spectrum.low_mz = spectrum.mz[0];
            spectrum.high_mz = *spectrum.mz.last().unwrap_or(&spectrum.high_mz);
            spectrum.tic = spectrum.intensity.iter().sum();
            if let Some((peak, intensity)) = spectrum
                .intensity
                .iter()
                .enumerate()
                .max_by(|(_, left), (_, right)| left.total_cmp(right))
            {
                spectrum.base_peak_intensity = *intensity;
                spectrum.base_peak_mz = spectrum.mz[peak];
            }
        }
        Ok(spectrum)
    }

    pub fn summary(&self) -> Summary {
        let mzs = self
            .spectra
            .iter()
            .flat_map(|s| {
                if self.format == Format::AgilentMassHunterD || self.format == Format::ThermoRaw {
                    vec![s.low_mz, s.high_mz]
                } else {
                    s.mz.clone()
                }
            })
            .filter(|v| *v > 0.0);
        let (min_mz, max_mz) = mzs.fold((f32::INFINITY, 0.0f32), |(lo, hi), v| {
            (lo.min(v), hi.max(v))
        });
        let mut times = self
            .spectra
            .iter()
            .map(|s| s.retention_time)
            .chain(
                self.chromatograms
                    .iter()
                    .flat_map(|chromatogram| chromatogram.time.iter().copied()),
            )
            .filter(|v| *v > 0.0)
            .collect::<Vec<_>>();
        if let Some(metadata) = &self.sciex_mrm_metadata {
            for transition in &metadata.transitions {
                times.push(transition.start_time);
                times.push(transition.end_time);
            }
        }
        let (start_rt, end_rt) = times
            .into_iter()
            .filter(|v| *v > 0.0)
            .fold((f32::INFINITY, 0.0f32), |(lo, hi), v| {
                (lo.min(v), hi.max(v))
            });
        Summary {
            file_name: self
                .path
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned(),
            file_path: self.path.clone(),
            format: self.format,
            number_spectra: self.spectra.len(),
            number_chromatograms: self.chromatograms.len(),
            number_spectra_binary_arrays: self.spectra.len() * 2,
            min_mz: if min_mz.is_finite() { min_mz } else { 0.0 },
            max_mz,
            start_rt: if start_rt.is_finite() { start_rt } else { 0.0 },
            end_rt,
            has_ion_mobility: self.spectra.iter().any(|s| s.mobility != 0.0),
            time_stamp: self
                .thermo_metadata
                .as_ref()
                .map(|metadata| metadata.time_stamp.clone())
                .unwrap_or_default(),
        }
    }
}

fn render_sciex_mrm_headers(transitions: &[crate::reader_sciex::Transition]) -> Vec<Chromatogram> {
    let mut output = Vec::with_capacity(transitions.len() + 2);
    for (index, (id, kind)) in [("TIC", "TIC"), ("BPC", "BPC")].into_iter().enumerate() {
        output.push(Chromatogram {
            id: id.into(),
            signal_type: "MS".into(),
            chromatogram_type: kind.into(),
            detector: "SCIEX".into(),
            units: "counts".into(),
            ..Default::default()
        });
        let _ = index;
    }
    output.extend(transitions.iter().map(|transition| Chromatogram {
        id: transition.name.clone(),
        signal_type: "MS".into(),
        chromatogram_type: "SRM".into(),
        detector: "SCIEX".into(),
        channel: transition.product_mz.to_string(),
        units: "counts".into(),
        polarity: 0,
        start_time: Some(transition.start_time * 60.0),
        end_time: Some(transition.end_time * 60.0),
        precursor_mz: Some(transition.precursor_mz),
        product_mz: Some(transition.product_mz),
        activation_ce: Some(transition.collision_energy),
        ..Default::default()
    }));
    output
}

fn load_sciex_chromatograms(path: &Path, analysis: &Analysis) -> Result<Vec<Chromatogram>> {
    let sample = analysis
        .source_analysis_number
        .ok_or_else(|| ReaderError::Invalid("SCIEX analysis has no source sample number".into()))?;
    let series = crate::reader_sciex::read_native_mrm_series(path, sample)?;
    render_sciex_mrm_chromatograms(series)
}

fn render_sciex_mrm_chromatograms(
    series_list: Vec<crate::reader_sciex::MrmExperimentSeries>,
) -> Result<Vec<Chromatogram>> {
    let mut traces = Vec::new();
    let mut tic = Vec::new();
    let mut bpc = Vec::new();
    let mut tic_time = Vec::new();
    for series in series_list {
        let point_count = series.intensities.first().map_or(0, Vec::len);
        if point_count == 0
            || series.transitions.len() != series.intensities.len()
            || series.retention_times.len() != series.intensities.len()
        {
            return Err(ReaderError::Invalid(
                "SCIEX MRM series has inconsistent transition arrays".into(),
            ));
        }
        if series
            .intensities
            .iter()
            .any(|values| values.len() != point_count)
        {
            return Err(ReaderError::Invalid(
                "SCIEX MRM transition arrays have unequal lengths".into(),
            ));
        }
        let time = &series.retention_times[0];
        if time.len() != point_count {
            return Err(ReaderError::Invalid(
                "SCIEX MRM time and intensity arrays have unequal lengths".into(),
            ));
        }
        tic_time.extend(time.iter().map(|value| *value * 60.0));
        for point in 0..point_count {
            let values = series.intensities.iter().map(|trace| trace[point]);
            tic.push(values.clone().sum());
            bpc.push(values.fold(0.0_f32, f32::max));
        }
        for ((transition, time), intensity) in series
            .transitions
            .into_iter()
            .zip(series.retention_times)
            .zip(series.intensities)
        {
            let (mut time, intensity): (Vec<_>, Vec<_>) = time
                .into_iter()
                .zip(intensity)
                .filter(|(rt, _)| {
                    transition.start_time >= transition.end_time
                        || (*rt >= transition.start_time && *rt <= transition.end_time)
                })
                .unzip();
            for value in &mut time {
                *value *= 60.0;
            }
            let interval_ms = if time.len() > 1 {
                (time[1] - time[0]) * 1_000.0
            } else {
                0.0
            };
            traces.push(Chromatogram {
                id: transition.name,
                array_length: time.len() as i32,
                signal_type: "MS".into(),
                chromatogram_type: "SRM".into(),
                detector: "SCIEX".into(),
                channel: transition.product_mz.to_string(),
                units: "counts".into(),
                wavelength_nm: 0.0,

                polarity: 0,
                interval_ms,
                start_time: time.first().copied(),
                end_time: time.last().copied(),
                precursor_mz: Some(transition.precursor_mz),
                product_mz: Some(transition.product_mz),
                activation_ce: Some(transition.collision_energy),
                time,
                intensity,
            });
        }
    }
    let tic_start = tic_time.first().copied();
    let tic_end = tic_time.last().copied();
    let mut output = Vec::with_capacity(traces.len() + 2);
    output.push(Chromatogram {
        id: "TIC".into(),
        signal_type: "MS".into(),
        chromatogram_type: "TIC".into(),
        detector: "SCIEX".into(),
        units: "counts".into(),
        time: tic_time.clone(),
        intensity: tic,
        start_time: tic_start,
        end_time: tic_end,
        ..Default::default()
    });
    output.push(Chromatogram {
        id: "BPC".into(),
        signal_type: "MS".into(),
        chromatogram_type: "BPC".into(),
        detector: "SCIEX".into(),
        units: "counts".into(),
        time: tic_time,
        intensity: bpc,
        start_time: tic_start,
        end_time: tic_end,
        ..Default::default()
    });
    output.extend(traces);
    Ok(output)
}

fn detect_format(path: &Path, bytes: &[u8]) -> Result<Format> {
    match path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase()
        .as_str()
    {
        "mzml" => Ok(Format::MzMl),
        "mzxml" => Ok(Format::MzXml),
        "asc" => Ok(Format::Asc),
        "lcd" => Ok(Format::ShimadzuLcd),
        "wiff" if path.with_extension("wiff.scan").exists() && cfb::open(path).is_ok() => {
            Ok(Format::SciexWiff)
        }
        "d" => Err(ReaderError::Unsupported(
            "Shimadzu .d directories are not LCD compound files".into(),
        )),
        ext => {
            let head = String::from_utf8_lossy(&bytes[..bytes.len().min(4096)]);
            if head.contains("<mzML") || head.contains("<indexedmzML") {
                Ok(Format::MzMl)
            } else if head.contains("<mzXML") {
                Ok(Format::MzXml)
            } else {
                Err(ReaderError::Unsupported(ext.into()))
            }
        }
    }
}

fn attr<'a>(e: &quick_xml::events::BytesStart<'a>, name: &[u8]) -> Option<String> {
    e.attributes()
        .flatten()
        .find(|a| a.key.as_ref() == name)
        .and_then(|a| a.unescape_value().ok())
        .map(|v| v.into_owned())
}
fn f32_attr(e: &quick_xml::events::BytesStart<'_>, name: &[u8]) -> f32 {
    attr(e, name).and_then(|v| v.parse().ok()).unwrap_or(0.0)
}
fn i32_attr(e: &quick_xml::events::BytesStart<'_>, name: &[u8]) -> i32 {
    attr(e, name).and_then(|v| v.parse().ok()).unwrap_or(0)
}
fn local(name: &[u8]) -> &[u8] {
    name.rsplit(|b| *b == b':').next().unwrap_or(name)
}

fn id_value(id: &str, key: &str) -> Option<f32> {
    id.split_whitespace()
        .find_map(|part| part.strip_prefix(&format!("{key}="))?.parse().ok())
}
#[derive(Default)]
struct BinaryArray {
    mz: bool,
    intensity: bool,
    compressed: bool,
    precision: usize,
    encoded: String,
}

fn decode_array(a: &BinaryArray) -> Result<Vec<f32>> {
    let compact: String = a.encoded.chars().filter(|c| !c.is_whitespace()).collect();
    if compact.is_empty() {
        return Ok(Vec::new());
    }
    let mut bytes = STANDARD
        .decode(compact)
        .map_err(|e| ReaderError::Invalid(format!("invalid base64 array: {e}")))?;
    if a.compressed {
        let mut out = Vec::new();
        ZlibDecoder::new(bytes.as_slice()).read_to_end(&mut out)?;
        bytes = out;
    }
    let width = if a.precision == 64 { 8 } else { 4 };
    if bytes.len() % width != 0 {
        return Err(ReaderError::Invalid(format!(
                "binary array length is not aligned to its precision: len={} width={} precision={} mz={} intensity={} compressed={}",
                bytes.len(),
                width,
                a.precision,
                a.mz,
                a.intensity,
                a.compressed
            )));
    }
    Ok(bytes
        .chunks_exact(width)
        .map(|b| {
            if width == 8 {
                f64::from_le_bytes(b.try_into().unwrap()) as f32
            } else {
                f32::from_le_bytes(b.try_into().unwrap())
            }
        })
        .collect())
}

fn parse_mzml_with_arrays(bytes: &[u8], decode_arrays: bool) -> Result<Vec<Spectrum>> {
    let mut xml = XmlReader::from_reader(bytes);
    xml.config_mut().trim_text(false);
    let mut buf = Vec::new();
    let mut out = Vec::new();
    let mut current: Option<Spectrum> = None;
    let mut array: Option<BinaryArray> = None;
    let mut in_binary = false;
    loop {
        match xml.read_event_into(&mut buf)? {
            Event::Start(e) if local(e.name().as_ref()) == b"spectrum" => {
                current = Some(Spectrum {
                    index: i32_attr(&e, b"index"),
                    scan: i32_attr(&e, b"index"),
                    array_length: i32_attr(&e, b"defaultArrayLength"),
                    level: 1,
                    ..Default::default()
                })
            }
            Event::Start(e) if local(e.name().as_ref()) == b"binaryDataArray" => {
                array = Some(BinaryArray {
                    precision: 32,
                    ..Default::default()
                })
            }
            Event::Start(e) if local(e.name().as_ref()) == b"binary" => in_binary = true,
            Event::Text(e) if in_binary => {
                if let Some(a) = array.as_mut() {
                    a.encoded.push_str(&String::from_utf8_lossy(e.as_ref()));
                }
            }
            Event::Empty(e) | Event::Start(e) if local(e.name().as_ref()) == b"cvParam" => {
                let accession = attr(&e, b"accession").unwrap_or_default();
                let name = attr(&e, b"name").unwrap_or_default().to_ascii_lowercase();
                let value = attr(&e, b"value").unwrap_or_default();
                if let Some(a) = array.as_mut() {
                    if accession == "MS:1000574" || name.contains("zlib") {
                        a.compressed = true;
                    }
                    if accession == "MS:1000523" || name.contains("64-bit") {
                        a.precision = 64;
                    }
                    if accession == "MS:1000514" || name.contains("m/z array") {
                        a.mz = true;
                    }
                    if accession == "MS:1000515" || name.contains("intensity array") {
                        a.intensity = true;
                    }
                } else if let Some(s) = current.as_mut() {
                    match accession.as_str() {
                        "MS:1000511" => s.level = value.parse().unwrap_or(1),
                        "MS:1000130" => s.polarity = 1,
                        "MS:1000129" => s.polarity = -1,
                        "MS:1000528" => s.low_mz = value.parse().unwrap_or(0.0),
                        "MS:1000527" => s.high_mz = value.parse().unwrap_or(0.0),
                        "MS:1000504" => s.base_peak_mz = value.parse().unwrap_or(0.0),
                        "MS:1000505" => s.base_peak_intensity = value.parse().unwrap_or(0.0),
                        "MS:1000285" => s.tic = value.parse().unwrap_or(0.0),
                        "MS:1000744" => s.precursor_mz = value.parse().unwrap_or(0.0),
                        "MS:1000042" => s.precursor_intensity = value.parse().unwrap_or(0.0),
                        "MS:1000041" => s.precursor_charge = value.parse().unwrap_or(0),
                        "MS:1000045" => s.collision_energy = value.parse().unwrap_or(0.0),
                        _ if accession == "MS:1000016" || name.contains("scan start time") => {
                            s.retention_time = value.parse().unwrap_or(0.0)
                                * if name.contains("minute") { 60.0 } else { 1.0 }
                        }
                        _ if name.contains("mobility") => s.mobility = value.parse().unwrap_or(0.0),
                        _ => {}
                    }
                }
            }
            Event::End(e) if local(e.name().as_ref()) == b"binary" => in_binary = false,
            Event::End(e) if local(e.name().as_ref()) == b"binaryDataArray" => {
                if decode_arrays {
                    if let Some(a) = array.take() {
                        if let Some(s) = current.as_mut() {
                            let values = decode_array(&a).map_err(|error| {
                                        ReaderError::Invalid(format!(
                                            "spectrum index={} scan={} array(len={}, mz={}, intensity={}, compressed={}, precision={}): {error}",
                                            s.index,
                                            s.scan,
                                            a.encoded.len(),
                                            a.mz,
                                            a.intensity,
                                            a.compressed,
                                            a.precision
                                        ))
                                    })?;
                            if a.mz {
                                s.mz = values;
                            } else if a.intensity {
                                s.intensity = values;
                            }
                        }
                    }
                } else {
                    array.take();
                }
            }
            Event::End(e) if local(e.name().as_ref()) == b"spectrum" => {
                if let Some(mut s) = current.take() {
                    finish_spectrum(&mut s);
                    out.push(s);
                }
            }
            Event::Eof => break,
            _ => {}
        }
        buf.clear();
    }
    Ok(out)
}

fn mzml_spectrum_offsets(bytes: &[u8]) -> Result<Vec<(usize, usize)>> {
    let mut xml = XmlReader::from_reader(bytes);
    let mut buf = Vec::new();
    let mut start = None;
    let mut offsets = Vec::new();
    loop {
        let position = xml.buffer_position();
        match xml.read_event_into(&mut buf)? {
            Event::Start(e) if local(e.name().as_ref()) == b"spectrum" => {
                start = Some(position);
            }
            Event::End(e) if local(e.name().as_ref()) == b"spectrum" => {
                if let Some(begin) = start.take() {
                    offsets.push((begin as usize, xml.buffer_position() as usize));
                }
            }
            Event::Eof => break,
            _ => {}
        }
        buf.clear();
    }
    Ok(offsets)
}

fn parse_mzml_chromatograms(
    bytes: &[u8],
    decode_indices: Option<&[usize]>,
) -> Result<Vec<Chromatogram>> {
    let mut xml = XmlReader::from_reader(bytes);
    xml.config_mut().trim_text(false);
    let mut buf = Vec::new();
    let mut out = Vec::new();
    let mut current: Option<Chromatogram> = None;
    let mut array: Option<BinaryArray> = None;
    let mut in_binary = false;
    let mut decode_current = decode_indices.is_none();
    loop {
        match xml.read_event_into(&mut buf)? {
            Event::Start(e) if local(e.name().as_ref()) == b"chromatogram" => {
                let index = out.len();
                decode_current = decode_indices
                    .map(|indices| indices.contains(&index))
                    .unwrap_or(true);
                let id = attr(&e, b"id").unwrap_or_default();
                current = Some(Chromatogram {
                    array_length: i32_attr(&e, b"defaultArrayLength"),
                    id: id.clone(),
                    signal_type: "MS".into(),
                    chromatogram_type: if id.contains("TIC") {
                        "TIC".into()
                    } else {
                        "MS Chromatogram".into()
                    },
                    detector: "MS".into(),
                    channel: id.clone(),
                    units: "counts".into(),
                    precursor_mz: Some(0.0),
                    product_mz: Some(0.0),
                    activation_ce: id_value(&id, "ce"),
                    start_time: id_value(&id, "start"),
                    end_time: id_value(&id, "end"),
                    ..Default::default()
                });
            }
            Event::Start(e) if local(e.name().as_ref()) == b"binaryDataArray" => {
                array = Some(BinaryArray {
                    precision: 32,
                    ..Default::default()
                });
            }
            Event::Start(e) if local(e.name().as_ref()) == b"binary" => in_binary = true,
            Event::Text(e) if in_binary => {
                if let Some(a) = array.as_mut() {
                    a.encoded.push_str(&String::from_utf8_lossy(e.as_ref()));
                }
            }
            Event::Empty(e) | Event::Start(e) if local(e.name().as_ref()) == b"cvParam" => {
                let accession = attr(&e, b"accession").unwrap_or_default();
                let name = attr(&e, b"name").unwrap_or_default().to_ascii_lowercase();
                if let Some(a) = array.as_mut() {
                    if accession == "MS:1000574" || name.contains("zlib") {
                        a.compressed = true;
                    }
                    if accession == "MS:1000523" || name.contains("64-bit") {
                        a.precision = 64;
                    }
                    if accession == "MS:1000595" || name.contains("time array") {
                        a.mz = true;
                    }
                    if accession == "MS:1000515" || name.contains("intensity array") {
                        a.intensity = true;
                    }
                }
            }
            Event::End(e) if local(e.name().as_ref()) == b"binary" => in_binary = false,
            Event::End(e) if local(e.name().as_ref()) == b"binaryDataArray" => {
                if let Some(a) = array.take() {
                    if let Some(c) = current.as_mut() {
                        if decode_current {
                            let values = decode_array(&a)?;
                            if a.mz {
                                c.time = values;
                            } else if a.intensity {
                                c.intensity = values;
                            }
                        }
                    }
                }
            }
            Event::End(e) if local(e.name().as_ref()) == b"chromatogram" => {
                if let Some(c) = current.take() {
                    if decode_current {
                        trace_payload_decode("chromatogram", out.len());
                    }
                    out.push(c);
                }
            }
            Event::Eof => break,
            _ => {}
        }
        buf.clear();
    }
    Ok(out)
}

fn parse_mzxml(bytes: &[u8]) -> Result<Vec<Spectrum>> {
    let mut xml = XmlReader::from_reader(bytes);
    let mut buf = Vec::new();
    let mut out = Vec::new();
    let mut current: Option<Spectrum> = None;
    let mut peaks = false;
    let mut encoded = String::new();
    let mut precision = 32;
    let mut compressed = false;
    let mut big_endian = true;
    loop {
        match xml.read_event_into(&mut buf)? {
            Event::Start(e) if local(e.name().as_ref()) == b"scan" => {
                current = Some(Spectrum {
                    index: i32_attr(&e, b"num"),
                    scan: i32_attr(&e, b"num"),
                    array_length: i32_attr(&e, b"peaksCount"),
                    level: i32_attr(&e, b"msLevel").max(1),
                    polarity: match attr(&e, b"polarity").as_deref() {
                        Some("+") | Some("positive") => 1,
                        Some("-") | Some("negative") => -1,
                        _ => 0,
                    },
                    low_mz: f32_attr(&e, b"lowMz"),
                    high_mz: f32_attr(&e, b"highMz"),
                    base_peak_mz: f32_attr(&e, b"basePeakMz"),
                    base_peak_intensity: f32_attr(&e, b"basePeakIntensity"),
                    tic: f32_attr(&e, b"totIonCurrent"),
                    retention_time: parse_rt(&attr(&e, b"retentionTime").unwrap_or_default()),
                    ..Default::default()
                });
            }
            Event::Start(e) if local(e.name().as_ref()) == b"peaks" => {
                peaks = true;
                encoded.clear();
                precision = i32_attr(&e, b"precision").max(32) as usize;
                compressed = attr(&e, b"compressionType")
                    .unwrap_or_default()
                    .contains("zlib");
                big_endian = matches!(
                    attr(&e, b"byteOrder").as_deref(),
                    Some("network") | Some("big") | Some("big endian")
                );
            }
            Event::Text(e) if peaks => encoded.push_str(&String::from_utf8_lossy(e.as_ref())),
            Event::End(e) if local(e.name().as_ref()) == b"peaks" => {
                peaks = false;
                if let Some(s) = current.as_mut() {
                    let a = BinaryArray {
                        precision,
                        compressed,
                        encoded: encoded.clone(),
                        ..Default::default()
                    };
                    let values = decode_array_endian(&a, big_endian)?;
                    s.mz = values.iter().step_by(2).copied().collect();
                    s.intensity = values.iter().skip(1).step_by(2).copied().collect();
                }
            }
            Event::End(e) if local(e.name().as_ref()) == b"scan" => {
                if let Some(mut s) = current.take() {
                    finish_spectrum(&mut s);
                    out.push(s);
                }
            }
            Event::Eof => break,
            _ => {}
        }
        buf.clear();
    }
    Ok(out)
}

fn decode_array_endian(a: &BinaryArray, big: bool) -> Result<Vec<f32>> {
    let mut values = decode_array(a)?;
    if big {
        let compact: String = a.encoded.chars().filter(|c| !c.is_whitespace()).collect();
        let mut bytes = STANDARD
            .decode(compact)
            .map_err(|e| ReaderError::Invalid(e.to_string()))?;
        if a.compressed {
            let mut out = Vec::new();
            ZlibDecoder::new(bytes.as_slice()).read_to_end(&mut out)?;
            bytes = out;
        }
        let width = if a.precision == 64 { 8 } else { 4 };
        values = bytes
            .chunks_exact(width)
            .map(|b| {
                if width == 8 {
                    f64::from_be_bytes(b.try_into().unwrap()) as f32
                } else {
                    f32::from_be_bytes(b.try_into().unwrap())
                }
            })
            .collect();
    }
    Ok(values)
}
fn parse_rt(value: &str) -> f32 {
    value
        .strip_prefix("PT")
        .and_then(|v| v.strip_suffix('S'))
        .and_then(|v| v.parse().ok())
        .unwrap_or(0.0)
}
fn finish_spectrum(s: &mut Spectrum) {
    s.array_length = if s.array_length == 0 {
        s.mz.len() as i32
    } else {
        s.array_length
    };
    if s.low_mz == 0.0 {
        s.low_mz = s.mz.iter().copied().reduce(f32::min).unwrap_or(0.0);
    }
    if s.high_mz == 0.0 {
        s.high_mz = s.mz.iter().copied().reduce(f32::max).unwrap_or(0.0);
    }
    if s.tic == 0.0 {
        s.tic = s.intensity.iter().sum();
    }
    if s.base_peak_intensity == 0.0 {
        if let Some((i, v)) = s
            .intensity
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
        {
            s.base_peak_intensity = *v;
            s.base_peak_mz = s.mz.get(i).copied().unwrap_or(0.0);
        }
    }
}
fn parse_asc(bytes: &[u8]) -> Vec<Chromatogram> {
    let mut mz = Vec::new();
    let mut intensity = Vec::new();
    for line in String::from_utf8_lossy(bytes).lines() {
        let normalized = line.replace(',', " ").replace(';', " ");
        let mut it = normalized
            .split_whitespace()
            .filter_map(|v| v.parse::<f32>().ok());
        if let (Some(x), Some(y)) = (it.next(), it.next()) {
            mz.push(x);
            intensity.push(y);
        }
    }
    if mz.is_empty() {
        return Vec::new();
    }
    vec![Chromatogram {
        time: mz,
        intensity,
        ..Default::default()
    }]
}

fn lcd_path(path: &str) -> String {
    format!("/{}", path.trim_start_matches('/').replace('\\', "/"))
}

fn read_u16_le(bytes: &[u8], pos: usize) -> u16 {
    bytes
        .get(pos..pos + 2)
        .and_then(|v| v.try_into().ok())
        .map(u16::from_le_bytes)
        .unwrap_or(0)
}

fn read_u32_le(bytes: &[u8], pos: usize) -> u32 {
    bytes
        .get(pos..pos + 4)
        .and_then(|v| v.try_into().ok())
        .map(u32::from_le_bytes)
        .unwrap_or(0)
}

fn decode_rc_delta(bytes: &[u8], pos: usize, width: usize) -> f64 {
    let value_bits = width * 8 - 4;
    let packed = bytes[pos..pos + width]
        .iter()
        .fold(0.0, |v, b| v * 256.0 + f64::from(*b));
    let scale = 2_f64.powi(value_bits as i32);
    let sign = (packed / scale).floor() as i32;
    let value = packed - f64::from(sign) * scale;
    if sign % 2 == 1 {
        -(scale - value)
    } else {
        value
    }
}

fn decode_rc_stream(bytes: &[u8]) -> Vec<f64> {
    if bytes.len() < 24 || &bytes[..2] != b"RC" {
        return Vec::new();
    }
    let expected = read_u32_le(bytes, 8) as usize;
    let mut signal = Vec::with_capacity(expected);
    let mut pos = 24;
    while signal.len() < expected && pos + 1 < bytes.len() {
        let payload_len = read_u16_le(bytes, pos) as usize;
        pos += 2;
        if payload_len == 0 || pos + payload_len > bytes.len() {
            break;
        }
        let end = pos + payload_len;
        let mut accumulator = 0.0;
        while pos < end && signal.len() < expected {
            let current = bytes[pos];
            let delta = match current {
                0x82 => {
                    pos += 1;
                    continue;
                }
                0x00 => {
                    pos += 1;
                    0.0
                }
                value if value >> 4 == 0 => {
                    pos += 1;
                    f64::from(value)
                }
                value => {
                    let extra = if value >> 4 == 1 {
                        0
                    } else {
                        (value >> 4) as usize / 2
                    };
                    let width = 1 + extra;
                    if pos + width > end {
                        break;
                    }
                    let delta = decode_rc_delta(bytes, pos, width);
                    pos += width;
                    delta
                }
            };
            accumulator += delta;
            signal.push(accumulator);
        }
        if pos + 1 < bytes.len() {
            pos += 2;
        }
    }
    signal
}

fn with_initial_point(values: &[f64]) -> Vec<f64> {
    values
        .first()
        .map(|first| {
            std::iter::once(*first)
                .chain(values.iter().copied())
                .collect()
        })
        .unwrap_or_default()
}

fn status_definition(path: &str, channel: i32) -> Option<(&'static str, &'static str, f64)> {
    let lc = path.starts_with("LC Raw Data/");
    let values: &[(i32, &str, &str, f64)] = if lc {
        &[
            (1, "Pump A Pressure", "kgf/cm2", 0.1),
            (2, "Pump B Pressure", "kgf/cm2", 0.1),
            (4, "Oven Temp.", "C", 0.01),
            (5, "Room Temp.", "C", 0.01),
            (6, "Sample Cooler Temp.", "C", 0.01),
            (7, "UV Cell Temp.", "C", 0.01),
        ]
    } else {
        &[
            (1, "Pump A Pressure", "bar", 0.1 * 0.980665),
            (2, "Pump A Degasser Pressure", "bar", 0.1 * 0.980665),
            (3, "Pump B Pressure", "bar", 0.1 * 0.980665),
            (4, "Pump B Degasser Pressure", "bar", 1.0),
            (5, "Pump C Pressure", "bar", 0.1 * 0.980665),
            (6, "Pump C Degasser Pressure", "bar", 1.0),
            (7, "Sample Cooler Temp.", "C", 0.01),
            (8, "Oven Temp.", "C", 0.01),
            (9, "Room Temp.", "C", 0.01),
            (10, "Oven B Temp.", "C", 0.01),
        ]
    };
    values
        .iter()
        .find(|(number, _, _, _)| *number == channel)
        .map(|(_, id, units, factor)| (*id, *units, *factor))
}

fn trailing_channel(path: &str) -> Option<i32> {
    let marker = path.rfind("Ch")?;
    path[marker + 2..].parse().ok()
}

fn add_lcd_chromatogram(out: &mut Vec<Chromatogram>, path: &str, bytes: &[u8], values: &[f64]) {
    if values.is_empty() {
        return;
    }
    let interval_ms = read_u32_le(bytes, 4) as f32;
    let (id, signal_type, chromatogram_type, detector, channel, units, factor) =
        if path == "LC Raw Data/Chromatogram Ch1" {
            (
                "Detector A-Ch1",
                "LC Chromatogram",
                "UV Trace",
                "Detector A",
                "Ch1",
                "mV",
                0.00476837158203125 * 0.001,
            )
        } else if path == "LC Raw Data/Chromatogram Ch5" {
            (
                "AD1",
                "LC Chromatogram",
                "Analog Trace",
                "AD",
                "1",
                "mV",
                0.2 * 0.001,
            )
        } else if path == "LSS Raw Data/Chromatogram Ch1" {
            (
                "Detector A-Ch1",
                "LC Chromatogram",
                "UV Trace",
                "Detector A",
                "Ch1",
                "mV",
                0.00476837158203125 * 0.001,
            )
        } else if path == "LSS Raw Data/Chromatogram Ch5" {
            (
                "AD1",
                "LC Chromatogram",
                "Analog Trace",
                "AD",
                "1",
                "mV",
                0.2 * 0.001,
            )
        } else {
            return;
        };
    let values = with_initial_point(values);
    let time = (0..values.len())
        .map(|i| i as f32 * interval_ms / 60000.0 * 60.0)
        .collect();
    let intensity = values.into_iter().map(|v| (v * factor) as f32).collect();
    out.push(Chromatogram {
        id: id.into(),
        signal_type: signal_type.into(),
        chromatogram_type: chromatogram_type.into(),
        detector: detector.into(),
        channel: channel.into(),
        units: units.into(),
        wavelength_nm: if id == "Detector A-Ch1" { 280.0 } else { 0.0 },
        interval_ms,
        time,
        intensity,
        ..Default::default()
    });
}

#[derive(Default)]
struct TlmTransitionSet {
    group_id: i32,
    transition_id: i32,
    label: String,
    window_start: f32,
    window_end: f32,
    polarity: i32,
    precursor_mz: f32,
    activation_ce: f32,
    product_mz: Vec<f32>,
    product_ce: Vec<f32>,
}

fn read_lcd_stream(path: &Path, wanted: &str) -> Result<Option<Vec<u8>>> {
    let mut file = cfb::open(path)?;
    let wanted = wanted.to_ascii_lowercase();
    let stream_path = file
        .walk()
        .find(|entry| {
            entry.is_stream()
                && entry.len() > 0
                && entry
                    .path()
                    .to_string_lossy()
                    .trim_start_matches('/')
                    .replace('\\', "/")
                    .to_ascii_lowercase()
                    == wanted
        })
        .map(|entry| {
            entry
                .path()
                .to_string_lossy()
                .trim_start_matches('/')
                .replace('\\', "/")
        });
    let Some(stream_path) = stream_path else {
        return Ok(None);
    };
    let mut stream = file.open_stream(lcd_path(&stream_path))?;
    let mut bytes = Vec::new();
    stream.read_to_end(&mut bytes)?;
    Ok(Some(bytes))
}

fn read_sciex_analysis_catalog(path: &Path) -> Result<Vec<Analysis>> {
    let file = cfb::open(path)?;
    let mut metadata_numbers = BTreeSet::new();
    let mut indexed_numbers = BTreeSet::new();
    for entry in file.walk() {
        if !entry.is_stream() {
            continue;
        }
        let value = entry
            .path()
            .to_string_lossy()
            .trim_start_matches('/')
            .replace('\\', "/");
        let Some(rest) = value.strip_prefix("SampleSubtree/Sample") else {
            continue;
        };
        let Some((number, suffix)) = rest.split_once('/') else {
            continue;
        };
        if suffix == "SampleDABE/DATA" {
            if let Ok(number) = number.parse::<usize>() {
                metadata_numbers.insert(number);
            }
        } else if suffix == "Idx" {
            if let Ok(number) = number.parse::<usize>() {
                indexed_numbers.insert(number);
            }
        }
    }
    drop(file);
    let source_numbers = metadata_numbers
        .intersection(&indexed_numbers)
        .copied()
        .collect::<Vec<_>>();
    if source_numbers.is_empty() {
        return Err(ReaderError::Invalid(
            "Sciex WIFF contains no sample analysis metadata".into(),
        ));
    }
    let count = source_numbers.len();
    let mut catalog = Vec::with_capacity(count);
    let mut names = BTreeSet::new();
    for (index, source_number) in source_numbers.into_iter().enumerate() {
        let wanted = format!("SampleSubtree/Sample{source_number}/SampleDABE/DATA");
        let mut name = read_lcd_stream(path, &wanted)?
            .map(|bytes| first_utf16_string(&bytes))
            .filter(|name| !name.is_empty() && name != "none")
            .unwrap_or_else(|| format!("sample_{source_number}"));
        if !names.insert(name.clone()) {
            let base_name = name.clone();
            name = format!("{base_name}_sample_{source_number}");
            if !names.insert(name.clone()) {
                name = format!("{base_name}_analysis_{index}");
                names.insert(name.clone());
            }
        }
        catalog.push(Analysis {
            analysis_index: index,
            source_analysis_number: Some(source_number),
            name,
            analysis_count: count,
        });
    }
    Ok(catalog)
}

fn first_utf16_string(bytes: &[u8]) -> String {
    let mut candidate = String::new();
    let mut offset = 0;
    while offset + 1 < bytes.len() {
        let c = bytes[offset];
        let high = bytes[offset + 1];
        if c == 0 && high == 0 {
            if candidate.len() >= 2 {
                break;
            }
        } else if high == 0 && (32..=126).contains(&c) {
            candidate.push(c as char);
        } else if !candidate.is_empty() {
            break;
        }
        offset += 2;
    }
    candidate
}

fn read_ascii_z(bytes: &[u8], start: usize, max_len: usize) -> String {
    let mut out = String::new();
    for &byte in bytes
        .get(start..start.saturating_add(max_len))
        .unwrap_or_default()
    {
        if byte == 0 {
            break;
        }
        if (32..=126).contains(&byte) {
            out.push(byte as char);
        }
    }
    out
}

fn parse_tlm_method(path: &Path) -> Result<Vec<TlmTransitionSet>> {
    let Some(bytes) = read_lcd_stream(path, "TLM Raw Data/Mass Parameters")? else {
        return Ok(Vec::new());
    };
    if bytes.len() < 256 + 760 || !read_ascii_z(&bytes, 0, 32).starts_with("CTLM3030Parameters") {
        return Ok(Vec::new());
    }
    let mut sets = Vec::new();
    for compound in 0..(bytes.len() - 256) / 760 {
        let base = 256 + compound * 760;
        let count = read_u32_le(&bytes, base + 156).min(2) as usize;
        let mut set = TlmTransitionSet {
            transition_id: compound as i32 + 1,
            label: read_ascii_z(&bytes, base + 16, 128),
            window_start: read_u32_le(&bytes, base + 144) as f32 / 1000.0,
            window_end: read_u32_le(&bytes, base + 148) as f32 / 1000.0,
            precursor_mz: 0.0,
            ..Default::default()
        };
        let mut polarity_score = 0;
        for slot in 0..count {
            let pos = base + 512 + slot * 110;
            if read_u32_le(&bytes, pos) != 110 || read_u16_le(&bytes, pos + 4) != 11 {
                continue;
            }
            let precursor = read_u32_le(&bytes, pos + 6) as f32 / 10000.0;
            let product = read_u32_le(&bytes, pos + 10) as f32 / 10000.0;
            if precursor <= 0.0 || product <= 0.0 {
                continue;
            }
            set.precursor_mz = if set.precursor_mz == 0.0 {
                precursor
            } else {
                set.precursor_mz
            };
            set.product_mz.push(product);
            set.product_ce.push(read_u32_le(&bytes, pos + 14) as f32);
            set.activation_ce = if set.activation_ce == 0.0 {
                read_u32_le(&bytes, pos + 14) as f32
            } else {
                set.activation_ce
            };
            polarity_score += read_u32_le(&bytes, pos + 30) as i32;
        }
        if !set.product_mz.is_empty() {
            set.polarity = if polarity_score > 0 { -1 } else { 1 };
            set.group_id = sets
                .iter()
                .find(|value: &&TlmTransitionSet| {
                    (value.window_start - set.window_start).abs() < 0.0001
                        && (value.window_end - set.window_end).abs() < 0.0001
                })
                .map(|value| value.group_id)
                .unwrap_or_else(|| sets.iter().map(|value| value.group_id).max().unwrap_or(0) + 1);
            sets.push(set);
        }
    }
    Ok(sets)
}

fn decompress_zlib(bytes: &[u8]) -> Result<Vec<u8>> {
    let mut output = Vec::new();
    ZlibDecoder::new(bytes).read_to_end(&mut output)?;
    Ok(output)
}

fn parse_tlm_spectra(path: &Path, methods: &[TlmTransitionSet]) -> Result<Vec<Spectrum>> {
    let (Some(index), Some(raw), Some(rt)) = (
        read_lcd_stream(path, "TLM Raw Data/Spectrum Index")?,
        read_lcd_stream(path, "TLM Raw Data/MS Raw Data")?,
        read_lcd_stream(path, "TLM Raw Data/Retention Time")?,
    ) else {
        return Ok(Vec::new());
    };
    if index.len() % 24 != 0 || rt.len() / 4 != index.len() / 24 {
        return Ok(Vec::new());
    }
    let mut spectra = Vec::new();
    for i in 0..index.len() / 24 {
        let record = i * 24;
        let chunk_size = read_u32_le(&index, record) as usize;
        let chunk_offset = read_u32_le(&index, record + 8) as usize;
        if chunk_size <= 12
            || chunk_offset
                .checked_add(chunk_size)
                .is_none_or(|end| end > raw.len())
        {
            continue;
        }
        let payload = decompress_zlib(&raw[chunk_offset + 12..chunk_offset + chunk_size])?;
        let point_count = read_u32_le(&payload, 40) as usize;
        if point_count == 0 || payload.len() < 44 + point_count * 12 {
            continue;
        }
        let mut spectrum = Spectrum {
            index: spectra.len() as i32,
            scan: i as i32,
            array_length: point_count as i32,
            level: 2,
            polarity: 1,
            retention_time: read_u32_le(&rt, i * 4) as f32 / 1000.0,
            precursor_mz: read_u32_le(&payload, 44) as f32 / 100.0,
            mz: Vec::with_capacity(point_count),
            intensity: Vec::with_capacity(point_count),
            ..Default::default()
        };
        for j in 0..point_count {
            let point = 48 + j * 12;
            spectrum
                .mz
                .push(read_u32_le(&payload, point) as f32 / 100.0);
            spectrum
                .intensity
                .push(read_u32_le(&payload, point + 4) as f32);
        }
        finish_spectrum(&mut spectrum);
        if let Some(method) = methods.iter().find(|method| {
            (method.precursor_mz - spectrum.precursor_mz).abs() <= 0.05
                && method.product_mz.len() == spectrum.mz.len()
                && method
                    .product_mz
                    .iter()
                    .zip(&spectrum.mz)
                    .all(|(a, b)| (a - b).abs() < 0.0001)
        }) {
            spectrum.precursor_mz = method.precursor_mz;
            spectrum.polarity = method.polarity;
            spectrum.collision_energy = method.activation_ce;
        }
        spectra.push(spectrum);
    }
    Ok(spectra)
}

fn build_tlm_chromatograms(
    spectra: &[Spectrum],
    methods: &[TlmTransitionSet],
) -> Vec<Chromatogram> {
    let mut output = Vec::new();
    let mut ordered_methods = methods.iter().collect::<Vec<_>>();
    ordered_methods.sort_by_key(|method| {
        spectra
            .iter()
            .position(|spectrum| {
                (method.precursor_mz - spectrum.precursor_mz).abs() <= 0.05
                    && method.product_mz.len() == spectrum.mz.len()
                    && method
                        .product_mz
                        .iter()
                        .zip(&spectrum.mz)
                        .all(|(a, b)| (a - b).abs() < 0.0001)
            })
            .unwrap_or(usize::MAX)
    });
    for method in ordered_methods {
        let items = spectra
            .iter()
            .filter(|spectrum| {
                (method.precursor_mz - spectrum.precursor_mz).abs() <= 0.05
                    && method.product_mz.len() == spectrum.mz.len()
                    && method
                        .product_mz
                        .iter()
                        .zip(&spectrum.mz)
                        .all(|(a, b)| (a - b).abs() < 0.0001)
            })
            .collect::<Vec<_>>();
        if items.is_empty() {
            continue;
        }
        let prefix = format!(
            "{}-{}MS({})",
            method.group_id,
            method.transition_id,
            if method.polarity < 0 { "E-" } else { "E+" }
        );
        let label = if method.label.is_empty() {
            prefix.clone()
        } else {
            method.label.clone()
        };
        let time = items
            .iter()
            .map(|spectrum| spectrum.retention_time)
            .collect::<Vec<_>>();
        let interval_ms = time
            .windows(2)
            .next()
            .map(|pair| (pair[1] - pair[0]) * 1000.0)
            .unwrap_or(0.0);
        let mut add = |id: String, kind: &str, product: Option<usize>| {
            let intensity = items
                .iter()
                .map(|spectrum| {
                    if kind == "BPC" {
                        spectrum.base_peak_intensity
                    } else {
                        product.map_or(spectrum.tic, |index| {
                            spectrum.intensity.get(index).copied().unwrap_or(0.0)
                        })
                    }
                })
                .collect();
            output.push(Chromatogram {
                id,
                signal_type: "MS".into(),
                chromatogram_type: kind.into(),
                detector: "MS".into(),
                channel: label.clone(),
                units: "counts".into(),
                polarity: method.polarity,
                interval_ms,
                time: time.clone(),
                intensity,
                precursor_mz: Some(method.precursor_mz),
                product_mz: product.map(|index| method.product_mz[index]),
                activation_ce: product
                    .and_then(|index| method.product_ce.get(index).copied())
                    .or(Some(method.activation_ce)),
                start_time: time.first().copied(),
                end_time: time.last().copied(),
                ..Default::default()
            });
        };
        add(format!("{prefix} TIC"), "TIC", None);
        add(format!("{prefix} BPC"), "BPC", None);

        for (index, product) in method.product_mz.iter().enumerate() {
            add(
                format!("{prefix}m/z {:.4}>{:.4}", method.precursor_mz, product),
                "MRM",
                Some(index),
            );
        }
    }
    output
}

fn parse_lcd(path: &Path) -> Result<(Vec<Spectrum>, Vec<Chromatogram>)> {
    let mut file = cfb::open(path)?;
    let mut chromatograms = Vec::new();
    let stream_paths: Vec<String> = file
        .walk()
        .filter(|entry| entry.is_stream() && !entry.is_empty())
        .map(|entry| {
            entry
                .path()
                .to_string_lossy()
                .trim_start_matches('/')
                .replace('\\', "/")
        })
        .filter(|path| {
            path.starts_with("LC Raw Data/Chromatogram Ch")
                || path.starts_with("LSS Raw Data/Chromatogram Ch")
                || path.starts_with("LC Raw Data/StatusLog Ch")
                || path.starts_with("LSS Raw Data/StatusLog Ch")
        })
        .collect::<Vec<_>>();
    let stream_rank = |path: &str| {
        if path.contains("/Chromatogram Ch") {
            0
        } else {
            1
        }
    };
    let mut stream_paths = stream_paths;
    stream_paths.sort_by_key(|path| {
        (
            stream_rank(path),
            trailing_channel(path).unwrap_or(i32::MAX),
            path.clone(),
        )
    });
    for stream_path in stream_paths {
        let is_chrom = stream_path.starts_with("LC Raw Data/Chromatogram Ch")
            || stream_path.starts_with("LSS Raw Data/Chromatogram Ch");
        let is_status = stream_path.starts_with("LC Raw Data/StatusLog Ch")
            || stream_path.starts_with("LSS Raw Data/StatusLog Ch");
        if !is_chrom && !is_status {
            continue;
        }
        let mut stream = file.open_stream(lcd_path(&stream_path))?;
        let mut bytes = Vec::new();
        stream.read_to_end(&mut bytes)?;
        let decoded = decode_rc_stream(&bytes);
        if is_chrom {
            add_lcd_chromatogram(&mut chromatograms, &stream_path, &bytes, &decoded);
        } else if let Some(channel) = trailing_channel(&stream_path) {
            if let Some((id, units, factor)) = status_definition(&stream_path, channel) {
                let interval_ms = read_u32_le(&bytes, 4) as f32;
                let values = with_initial_point(&decoded);
                chromatograms.push(Chromatogram {
                    id: id.into(),
                    signal_type: "LC Status".into(),
                    chromatogram_type: "Status Trace".into(),
                    detector: "LC Status".into(),
                    channel: id.into(),
                    units: units.into(),
                    interval_ms,
                    time: (0..values.len())
                        .map(|i| i as f32 * interval_ms / 60000.0 * 60.0)
                        .collect(),
                    intensity: values.into_iter().map(|v| (v * factor) as f32).collect(),
                    ..Default::default()
                });
            }
        }
    }
    let methods = parse_tlm_method(path)?;
    let spectra = parse_tlm_spectra(path, &methods)?;
    chromatograms.extend(build_tlm_chromatograms(&spectra, &methods));
    Ok((spectra, chromatograms))
}

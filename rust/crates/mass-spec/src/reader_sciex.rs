use std::{
    collections::BTreeSet,
    fs::{self, File},
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
};

use crate::reader::{ReaderError, Result};

#[derive(Debug, Clone)]
pub struct ScanBlock {
    pub sample_number: u32,
    pub offset: usize,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct IdxRecord {
    pub sample_number: u32,
    pub scan_offset: u32,
    pub scan_size: u32,
    pub retention_time_minutes: f32,
    pub ms_level_flag: u8,
    pub tic: f64,
    pub grid_field: f64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ScanPoint {
    pub raw_mz_bin: u32,
    pub raw_intensity: u32,
}

#[derive(Debug, Clone)]
pub struct IndexedFloatRecord {
    pub index: IdxRecord,
    pub fields: Vec<f32>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CompactMrmPair {
    pub first_intensity: f32,
    pub second_intensity: f32,
}

#[derive(Debug, Clone)]
pub struct TofMetadata {
    pub source_analysis_number: usize,
    pub records: Vec<IdxRecord>,
    pub public_indices: Vec<usize>,
    pub sample_base: usize,
    pub slope: f64,
    pub intercept: f64,
    pub dde_precursors: Vec<f32>,
    pub dde_precursor_intensities: Vec<f32>,
    pub experiment_count: usize,
}

pub fn tof_precursor_for_index(metadata: &TofMetadata, index: usize) -> f32 {
    let Some(&source_index) = metadata.public_indices.get(index) else {
        return 0.0;
    };
    if metadata.experiment_count > 1 && source_index % metadata.experiment_count == 0 {
        return 0.0;
    }
    let ms2_index = metadata.public_indices[..=index]
        .iter()
        .filter(|source_index| {
            let source_index = **source_index;
            !(metadata.experiment_count > 1 && source_index % metadata.experiment_count == 0)
        })
        .count()
        .saturating_sub(1);
    metadata
        .dde_precursors
        .get(ms2_index)
        .copied()
        .unwrap_or(0.0)
}

pub fn tof_precursor_intensity_for_index(metadata: &TofMetadata, index: usize) -> f32 {
    let Some(&source_index) = metadata.public_indices.get(index) else {
        return 0.0;
    };
    if metadata.experiment_count > 1 && source_index % metadata.experiment_count == 0 {
        return 0.0;
    }
    let ms2_index = metadata.public_indices[..=index]
        .iter()
        .filter(|source_index| {
            let source_index = **source_index;
            !(metadata.experiment_count > 1 && source_index % metadata.experiment_count == 0)
        })
        .count()
        .saturating_sub(1);
    metadata
        .dde_precursor_intensities
        .get(ms2_index)
        .copied()
        .unwrap_or(0.0)
}

fn read_tof_dde_metadata(path: &Path, source_analysis_number: usize) -> (Vec<f32>, Vec<f32>) {
    let stream = read_stream(
        path,
        &format!("SampleSubtree/Sample{source_analysis_number}/DDERealTimeDataEx"),
    )
    .or_else(|_| {
        read_stream(
            path,
            &format!("SampleSubtree/Sample{source_analysis_number}/DDERealTimeData"),
        )
    });
    let Ok(bytes) = stream else {
        return (Vec::new(), Vec::new());
    };
    let stride = if bytes.len() >= 32 && (bytes.len() - 32) % 32 == 0 {
        32
    } else {
        76
    };
    let value_offset = if stride == 32 { 0 } else { 4 };
    let mut precursors = Vec::new();
    let mut intensities = Vec::new();
    for offset in (32..=bytes.len().saturating_sub(stride)).step_by(stride) {
        let Ok(value) = read_f64(&bytes, offset + value_offset) else {
            continue;
        };
        if value.is_finite() && value > 0.0 && value < 5000.0 {
            precursors.push(value as f32);
            intensities.push(if stride == 32 {
                read_f64(&bytes, offset + 16).unwrap_or(0.0) as f32
            } else {
                0.0
            });
        }
    }
    (precursors, intensities)
}

fn read_tof_dde_precursors(path: &Path, source_analysis_number: usize) -> Vec<f32> {
    read_tof_dde_metadata(path, source_analysis_number).0
}

pub fn read_tof_metadata(path: &Path, source_analysis_number: usize) -> Result<TofMetadata> {
    let calibration = read_stream(
        path,
        &format!("SampleSubtree/Sample{source_analysis_number}/TOFCalibrationData"),
    )?;
    if calibration.len() < 48 {
        return Err(ReaderError::Unsupported(
            "SCIEX TOF calibration stream is incomplete".into(),
        ));
    }
    let slope = read_f64(&calibration, 32)?;
    let intercept = read_f64(&calibration, 40)?;
    if !slope.is_finite() || !intercept.is_finite() || slope == 0.0 {
        return Err(ReaderError::Invalid(
            "SCIEX TOF calibration contains an invalid slope or intercept".into(),
        ));
    }
    let index_bytes = read_stream(
        path,
        &format!("SampleSubtree/Sample{source_analysis_number}/Idx"),
    )?;
    if index_bytes.len() < 32 {
        return Err(ReaderError::Invalid(
            "Sciex WIFF Idx stream is too short".into(),
        ));
    }
    let mut records = Vec::new();
    for offset in (32..=index_bytes.len().saturating_sub(54)).step_by(54) {
        records.push(IdxRecord {
            sample_number: source_analysis_number as u32,
            scan_offset: read_u32(&index_bytes, offset)?,
            scan_size: read_u32(&index_bytes, offset + 4)?,
            retention_time_minutes: (read_f64(&index_bytes, offset + 8)? / 60000.0) as f32,
            ms_level_flag: *index_bytes.get(offset + 16).unwrap_or(&0),
            tic: read_f64(&index_bytes, offset + 18)?,
            grid_field: read_f64(&index_bytes, offset + 26)?,
        });
    }
    let experiment_count = records
        .iter()
        .enumerate()
        .skip(1)
        .find_map(|(index, record)| (record.scan_size > 0 && index <= 128).then_some(index))
        .unwrap_or(1);
    let (dde_precursors, dde_precursor_intensities) =
        read_tof_dde_metadata(path, source_analysis_number);
    let public_indices = records
        .iter()
        .enumerate()
        .filter_map(|(index, record)| {
            (index > 0 && index + 1 < records.len() && record.scan_size > 0).then_some(index)
        })
        .collect();
    Ok(TofMetadata {
        source_analysis_number,
        records,
        public_indices,
        sample_base: sample_block_offset(path, source_analysis_number as u32)?,
        slope,
        intercept,
        dde_precursors,
        dde_precursor_intensities,
        experiment_count,
    })
}

pub fn read_tof_spectrum(
    path: &Path,
    metadata: &TofMetadata,
    index: usize,
) -> Result<crate::reader::Spectrum> {
    let source_index = *metadata.public_indices.get(index).ok_or_else(|| {
        ReaderError::Invalid(format!("SCIEX TOF spectrum index is out of range: {index}"))
    })?;
    let record = metadata.records.get(source_index).ok_or_else(|| {
        ReaderError::Invalid(format!(
            "SCIEX TOF source scan index is out of range: {source_index}"
        ))
    })?;
    if record.scan_size == 0 {
        return Err(ReaderError::Invalid(format!(
            "SCIEX TOF spectrum index has no scan payload: {index}"
        )));
    }
    let mut scan_file = File::open(scan_path_for_wiff(path))?;
    let scan_size = scan_file.metadata()?.len() as usize;
    let payload_start = metadata.sample_base + record.scan_offset as usize + 56;
    let next_end = metadata
        .records
        .get(source_index + 1)
        .map_or(scan_size, |next| {
            metadata.sample_base + next.scan_offset as usize + 64
        });
    let own_end =
        metadata.sample_base + record.scan_offset as usize + record.scan_size as usize + 64;
    let end = next_end.min(own_end).min(scan_size);
    let points = if end > payload_start {
        let mut payload = vec![0u8; end - payload_start];
        scan_file.seek(SeekFrom::Start(payload_start as u64))?;
        scan_file.read_exact(&mut payload)?;
        decode_scan_payload(&payload)?
    } else {
        Vec::new()
    };
    let is_ms1 = metadata.experiment_count > 1 && source_index % metadata.experiment_count == 0;
    let mut spectrum = crate::reader::Spectrum {
        index: index as i32,
        scan: source_index as i32,
        array_length: points.len() as i32,
        level: if is_ms1 { 1 } else { 2 },
        polarity: 1,
        retention_time: record.retention_time_minutes * 60.0,
        ..Default::default()
    };
    if !is_ms1 {
        spectrum.precursor_mz = tof_precursor_for_index(metadata, index);
        spectrum.precursor_intensity = tof_precursor_intensity_for_index(metadata, index);
    }
    for point in points {
        spectrum.mz.push(
            (metadata.slope * (point.raw_mz_bin as f64 * 0.025 - metadata.intercept)).powi(2)
                as f32,
        );
        spectrum.intensity.push(point.raw_intensity as f32);
    }
    spectrum.tic = spectrum.intensity.iter().sum();
    if let Some((point, intensity)) = spectrum
        .intensity
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.total_cmp(b.1))
    {
        spectrum.base_peak_mz = spectrum.mz[point];
        spectrum.base_peak_intensity = *intensity;
    }
    spectrum.low_mz = spectrum.mz.first().copied().unwrap_or(0.0);
    spectrum.high_mz = spectrum.mz.last().copied().unwrap_or(0.0);
    Ok(spectrum)
}

pub fn read_tof_spectra(
    path: &Path,
    source_analysis_number: usize,
) -> Result<Vec<crate::reader::Spectrum>> {
    let calibration = read_stream(
        path,
        &format!("SampleSubtree/Sample{source_analysis_number}/TOFCalibrationData"),
    )?;
    if calibration.len() < 48 {
        return Err(ReaderError::Unsupported(
            "SCIEX TOF calibration stream is incomplete".into(),
        ));
    }
    let slope = read_f64(&calibration, 32)?;
    let intercept = read_f64(&calibration, 40)?;
    if !slope.is_finite() || !intercept.is_finite() || slope == 0.0 {
        return Err(ReaderError::Invalid(
            "SCIEX TOF calibration contains an invalid slope or intercept".into(),
        ));
    }
    let scan_bytes = fs::read(scan_path_for_wiff(path))?;
    let sample_base = sample_block_offset(path, source_analysis_number as u32)?;
    let index_bytes = read_stream(
        path,
        &format!("SampleSubtree/Sample{source_analysis_number}/Idx"),
    )?;
    let mut records = Vec::new();
    for offset in (32..=index_bytes.len().saturating_sub(54)).step_by(54) {
        records.push(IdxRecord {
            sample_number: source_analysis_number as u32,
            scan_offset: read_u32(&index_bytes, offset)?,
            scan_size: read_u32(&index_bytes, offset + 4)?,
            retention_time_minutes: (read_f64(&index_bytes, offset + 8)? / 60_000.0) as f32,
            ms_level_flag: *index_bytes.get(offset + 16).unwrap_or(&0),
            tic: read_f64(&index_bytes, offset + 18)?,
            grid_field: read_f64(&index_bytes, offset + 26)?,
        });
    }
    let experiment_count = records
        .iter()
        .enumerate()
        .skip(1)
        .find_map(|(index, record)| (record.scan_size > 0 && index <= 64).then_some(index))
        .unwrap_or(1);
    let dde_precursors = read_tof_dde_precursors(path, source_analysis_number);
    let mut ms2_count = 0usize;
    let mut spectra = Vec::new();
    for (i, record) in records.iter().enumerate() {
        if i == 0 || i + 1 == records.len() {
            continue;
        }
        if record.scan_size == 0 {
            continue;
        }
        let payload_start = sample_base + record.scan_offset as usize + 56;
        let next_end = records.get(i + 1).map_or(scan_bytes.len(), |next| {
            sample_base + next.scan_offset as usize + 64
        });
        let own_end = sample_base + record.scan_offset as usize + record.scan_size as usize + 64;
        let end = next_end.min(own_end).min(scan_bytes.len());
        let points = if end > payload_start {
            decode_scan_payload(&scan_bytes[payload_start..end])?
        } else {
            Vec::new()
        };
        let is_ms1 = experiment_count > 1 && i % experiment_count == 0;
        let mut spectrum = crate::reader::Spectrum {
            index: spectra.len() as i32,
            scan: i as i32,
            array_length: points.len() as i32,
            level: if is_ms1 { 1 } else { 2 },
            polarity: 1,
            retention_time: record.retention_time_minutes * 60.0,
            ..Default::default()
        };
        if !is_ms1 {
            if let Some(precursor) = dde_precursors.get(ms2_count) {
                spectrum.precursor_mz = *precursor;
            }
            ms2_count += 1;
        }
        for point in points {
            spectrum
                .mz
                .push((slope * (point.raw_mz_bin as f64 * 0.025 - intercept)).powi(2) as f32);
            spectrum.intensity.push(point.raw_intensity as f32);
        }
        spectrum.tic = spectrum.intensity.iter().sum();
        if let Some((index, intensity)) = spectrum
            .intensity
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
        {
            spectrum.base_peak_intensity = *intensity;
            spectrum.base_peak_mz = spectrum.mz[index];
        }
        spectrum.low_mz = spectrum.mz.first().copied().unwrap_or(0.0);
        spectrum.high_mz = spectrum.mz.last().copied().unwrap_or(0.0);
        spectra.push(spectrum);
    }
    Ok(spectra)
}

#[derive(Debug, Clone)]
pub struct MrmExperimentSeries {
    pub experiment_index: usize,
    pub transitions: Vec<Transition>,
    pub retention_times: Vec<Vec<f32>>,
    pub intensities: Vec<Vec<f32>>,
}

#[derive(Debug, Clone)]
pub struct EventRecord {
    pub ordinal: usize,
    pub retention_time_minutes: f32,
    pub fields: Vec<f32>,
}

#[derive(Debug, Clone)]
pub struct IntensityGroup {
    pub field_code: i32,
    pub intensities: Vec<f32>,
}

#[derive(Debug, Clone)]
pub struct Transition {
    pub name: String,
    pub precursor_mz: f32,
    pub product_mz: f32,
    pub start_time: f32,
    pub end_time: f32,
    pub collision_energy: f32,
}

#[derive(Debug, Clone)]
pub struct MrmMetadata {
    pub source_analysis_number: usize,
    pub transitions: Vec<Transition>,
}

pub fn read_mrm_metadata(path: &Path, source_analysis_number: usize) -> Result<MrmMetadata> {
    let _times = read_stream(
        path,
        &format!(
            "SampleSubtree/Sample{source_analysis_number}/SampleDAM/sMRMPro_adw1/sMRMPro_adw_Times"
        ),
    )?;
    Ok(MrmMetadata {
        source_analysis_number,
        transitions: read_transitions(path, source_analysis_number)?,
    })
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    let slice = bytes
        .get(offset..offset + 4)
        .ok_or_else(|| ReaderError::Invalid("Sciex stream contains a truncated uint32".into()))?;
    Ok(u32::from_le_bytes(slice.try_into().unwrap()))
}

fn read_f32(bytes: &[u8], offset: usize) -> Result<f32> {
    Ok(f32::from_bits(read_u32(bytes, offset)?))
}

fn read_f64(bytes: &[u8], offset: usize) -> Result<f64> {
    let slice = bytes
        .get(offset..offset + 8)
        .ok_or_else(|| ReaderError::Invalid("Sciex stream contains a truncated float64".into()))?;
    Ok(f64::from_le_bytes(slice.try_into().unwrap()))
}

fn normalize_stream_path(path: &str) -> String {
    path.trim_start_matches('/')
        .replace('\\', "/")
        .to_ascii_lowercase()
}

fn read_stream(path: &Path, wanted: &str) -> Result<Vec<u8>> {
    let mut file = cfb::open(path)?;
    let wanted = normalize_stream_path(wanted);
    let stream_path = file
        .walk()
        .find(|entry| {
            entry.is_stream() && normalize_stream_path(&entry.path().to_string_lossy()) == wanted
        })
        .map(|entry| entry.path().to_string_lossy().to_string())
        .ok_or_else(|| ReaderError::Invalid(format!("Sciex stream not found: {wanted}")))?;
    let mut stream = file.open_stream(format!("/{}", stream_path.trim_start_matches('/')))?;
    let mut bytes = Vec::new();
    stream.read_to_end(&mut bytes)?;
    Ok(bytes)
}

pub fn scan_path_for_wiff(path: &Path) -> PathBuf {
    path.with_extension("wiff.scan")
}

fn sample_block_offset(path: &Path, sample_number: u32) -> Result<usize> {
    let bytes = fs::read(scan_path_for_wiff(path))?;
    sample_block_offset_in_bytes(&bytes, sample_number)
}

fn sample_block_offset_in_bytes(bytes: &[u8], sample_number: u32) -> Result<usize> {
    for offset in (0..=bytes.len().saturating_sub(12)).step_by(4) {
        if read_u32(&bytes, offset)? == 0x11111111 && read_u32(&bytes, offset + 8)? == sample_number
        {
            return Ok(offset);
        }
    }
    Err(ReaderError::Invalid(
        "Sciex WIFF sample block is missing".into(),
    ))
}

pub fn read_idx_records(path: &Path, source_analysis_number: usize) -> Result<Vec<IdxRecord>> {
    let bytes = read_stream(
        path,
        &format!("SampleSubtree/Sample{source_analysis_number}/Idx"),
    )?;
    const HEADER: usize = 32;
    const RECORD: usize = 54;
    if bytes.len() < HEADER {
        return Err(ReaderError::Invalid(
            "Sciex WIFF Idx stream is too short".into(),
        ));
    }
    let mut records = Vec::new();
    for offset in (HEADER..=bytes.len().saturating_sub(RECORD)).step_by(RECORD) {
        let scan_size = read_u32(&bytes, offset + 4)?;
        if scan_size == 0 {
            continue;
        }
        records.push(IdxRecord {
            sample_number: source_analysis_number as u32,
            scan_offset: read_u32(&bytes, offset)?,
            scan_size,
            retention_time_minutes: (read_f64(&bytes, offset + 8)? / 60_000.0) as f32,
            ms_level_flag: bytes[offset + 16],
            tic: read_f64(&bytes, offset + 18)?,
            grid_field: read_f64(&bytes, offset + 26)?,
        });
    }
    if records.is_empty() {
        return Err(ReaderError::Invalid(
            "Sciex WIFF Idx contains no valid records".into(),
        ));
    }
    Ok(records)
}

pub fn decode_scan_payload(payload: &[u8]) -> Result<Vec<ScanPoint>> {
    if payload.len() < 8 || payload[..4] != [0xff, 0xff, 0xff, 0xff] {
        return Err(ReaderError::Invalid(
            "Sciex WIFF TOF payload has an invalid profile header".into(),
        ));
    }
    let mut points = Vec::new();
    let mut mz_bin = u32::from_le_bytes(payload[4..8].try_into().unwrap());
    let mut offset = 8usize;
    while offset < payload.len() {
        let token = payload[offset];
        if token == 0xff {
            break;
        }
        let value = token & 0x7f;
        let (consumed, value) = match value {
            0..=123 => (1, value as u32),
            124 => (
                2,
                *payload.get(offset + 1).ok_or_else(|| {
                    ReaderError::Invalid("Sciex WIFF TOF payload has a truncated value".into())
                })? as u32,
            ),
            125 => (
                3,
                u16::from_le_bytes(
                    payload
                        .get(offset + 1..offset + 3)
                        .ok_or_else(|| {
                            ReaderError::Invalid(
                                "Sciex WIFF TOF payload has a truncated value".into(),
                            )
                        })?
                        .try_into()
                        .unwrap(),
                ) as u32,
            ),
            126 => (
                5,
                u32::from_le_bytes(
                    payload
                        .get(offset + 1..offset + 5)
                        .ok_or_else(|| {
                            ReaderError::Invalid(
                                "Sciex WIFF TOF payload has a truncated value".into(),
                            )
                        })?
                        .try_into()
                        .unwrap(),
                ),
            ),
            _ => {
                return Err(ReaderError::Invalid(
                    "Sciex WIFF TOF payload has an unsupported value marker".into(),
                ))
            }
        };
        if consumed > payload.len() - offset {
            return Err(ReaderError::Invalid(
                "Sciex WIFF TOF payload has a truncated value".into(),
            ));
        }
        let advance = value.checked_mul(4).ok_or_else(|| {
            ReaderError::Invalid("Sciex WIFF TOF time-bin increment overflows uint32".into())
        })?;
        if token & 0x80 != 0 {
            mz_bin = mz_bin.checked_add(advance).ok_or_else(|| {
                ReaderError::Invalid("Sciex WIFF TOF time-bin overflows uint32".into())
            })?;
        } else {
            if value != 0 {
                points.push(ScanPoint {
                    raw_mz_bin: mz_bin,
                    raw_intensity: value,
                });
            }
            mz_bin = mz_bin.checked_add(4).ok_or_else(|| {
                ReaderError::Invalid("Sciex WIFF TOF time-bin overflows uint32".into())
            })?;
        }
        offset += consumed;
    }
    Ok(points)
}

pub fn read_scan_points(
    path: &Path,
    record: &IdxRecord,
    _next: Option<&IdxRecord>,
) -> Result<Vec<ScanPoint>> {
    let bytes = fs::read(scan_path_for_wiff(path))?;
    let sample_base = sample_block_offset(path, record.sample_number)?;
    let payload_start = sample_base
        .checked_add(record.scan_offset as usize)
        .and_then(|value| value.checked_add(24))
        .ok_or_else(|| ReaderError::Invalid("Sciex WIFF payload offset overflows".into()))?;
    let own_end = sample_base
        .checked_add(record.scan_offset as usize)
        .and_then(|value| value.checked_add(record.scan_size as usize))
        .ok_or_else(|| ReaderError::Invalid("Sciex WIFF scan extent overflows".into()))?;
    let end = own_end.min(bytes.len());
    if end <= payload_start {
        return Ok(Vec::new());
    }
    decode_scan_payload(&bytes[payload_start..end])
}

pub fn read_idx_float_records(
    path: &Path,
    source_analysis_number: usize,
) -> Result<Vec<IndexedFloatRecord>> {
    let index_bytes = read_stream(
        path,
        &format!("SampleSubtree/Sample{source_analysis_number}/Idx"),
    )?;
    let scan_bytes = fs::read(scan_path_for_wiff(path))?;
    let sample_base = sample_block_offset_in_bytes(&scan_bytes, source_analysis_number as u32)?;
    const HEADER: usize = 32;
    const RECORD: usize = 54;
    if index_bytes.len() < HEADER {
        return Err(ReaderError::Invalid(
            "Sciex WIFF Idx stream is too short".into(),
        ));
    }
    let mut records = Vec::new();
    for offset in (HEADER..=index_bytes.len().saturating_sub(RECORD)).step_by(RECORD) {
        let scan_offset = read_u32(&index_bytes, offset)?;
        let scan_size = read_u32(&index_bytes, offset + 4)?;
        let global_offset = sample_base + scan_offset as usize;
        let end = global_offset + scan_size as usize;
        if end > scan_bytes.len() || scan_size % 4 != 0 {
            continue;
        }
        let index = IdxRecord {
            sample_number: source_analysis_number as u32,
            scan_offset,
            scan_size,
            retention_time_minutes: (read_f64(&index_bytes, offset + 8)? / 60_000.0) as f32,
            ms_level_flag: index_bytes[offset + 16],
            tic: read_f64(&index_bytes, offset + 18)?,
            grid_field: read_f64(&index_bytes, offset + 26)?,
        };
        let fields = (global_offset..end)
            .step_by(4)
            .map(|pos| read_f32(&scan_bytes, pos))
            .collect::<Result<Vec<_>>>()?;
        records.push(IndexedFloatRecord { index, fields });
    }
    if records.is_empty() {
        return Err(ReaderError::Invalid(
            "Sciex WIFF contains no indexed float records".into(),
        ));
    }
    Ok(records)
}

pub fn read_idx_event_records(
    path: &Path,
    source_analysis_number: usize,
) -> Result<Vec<EventRecord>> {
    let fragments = read_idx_float_records(path, source_analysis_number)?;
    let mut records = Vec::new();
    for fragment in fragments {
        for value in fragment.fields {
            if (value + 59.01).abs() < 0.001 {
                records.push(EventRecord {
                    ordinal: records.len(),
                    retention_time_minutes: fragment.index.retention_time_minutes,
                    fields: Vec::new(),
                });
            } else if let Some(current) = records.last_mut() {
                current.fields.push(value);
            }
        }
    }
    Ok(records)
}

pub fn read_compact_mrm_pairs(
    path: &Path,
    source_analysis_number: usize,
) -> Result<Vec<CompactMrmPair>> {
    let fragments = read_idx_float_records(path, source_analysis_number)?;
    if fragments.len() < 4 || fragments.iter().any(|fragment| fragment.fields.len() != 2) {
        return Err(ReaderError::Invalid(
            "Sciex compact MRM payload is not a two-channel stream".into(),
        ));
    }
    Ok(fragments[3..]
        .iter()
        .map(|fragment| CompactMrmPair {
            first_intensity: fragment.fields[0],
            second_intensity: fragment.fields[1],
        })
        .collect())
}

pub fn build_compact_mrm_series(
    experiment_index: usize,
    transitions: Vec<Transition>,
    pairs: &[CompactMrmPair],
) -> Result<MrmExperimentSeries> {
    if transitions.len() != 2 {
        return Err(ReaderError::Invalid(
            "Compact MRM series requires exactly two transitions".into(),
        ));
    }
    let mut first = vec![0.0; 3];
    let mut second = vec![0.0; 3];
    first.extend(pairs.iter().map(|pair| pair.first_intensity));
    second.extend(pairs.iter().map(|pair| pair.second_intensity));
    let intensities = vec![first, second];
    let retention_times = transitions
        .iter()
        .enumerate()
        .map(|(index, transition)| {
            let count = intensities[index].len();
            (0..count)
                .map(|point| {
                    if transition.start_time == transition.end_time {
                        point as f32 * (0.110 / 60.0)
                    } else {
                        let fraction = if count <= 1 {
                            0.0
                        } else {
                            point as f32 / (count - 1) as f32
                        };
                        transition.start_time
                            + fraction * (transition.end_time - transition.start_time)
                    }
                })
                .collect()
        })
        .collect();
    Ok(MrmExperimentSeries {
        experiment_index,
        transitions,
        retention_times,
        intensities,
    })
}

pub fn read_compact_mrm_experiments(
    path: &Path,
    source_analysis_number: usize,
) -> Result<Vec<MrmExperimentSeries>> {
    let fragments = read_idx_float_records(path, source_analysis_number)?;
    let mut groups: Vec<Vec<IndexedFloatRecord>> = Vec::new();
    for fragment in fragments {
        if let Some(group) = groups.last_mut() {
            if group[0].fields.len() == fragment.fields.len() {
                group.push(fragment);
                continue;
            }
        }
        groups.push(vec![fragment]);
    }
    let mut out = Vec::new();
    for (experiment_index, group) in groups.into_iter().enumerate() {
        let width = group[0].fields.len();
        if width == 0 {
            continue;
        }
        let transitions =
            read_transitions_for_experiment(path, source_analysis_number, experiment_index, 0)?;
        if transitions.len() < width {
            return Err(ReaderError::Invalid(format!("SCIEX experiment {experiment_index} has {width} payload channels but {} transitions", transitions.len())));
        }
        let transitions = transitions.into_iter().take(width).collect::<Vec<_>>();
        let intensities = (0..width)
            .map(|column| group.iter().map(|record| record.fields[column]).collect())
            .collect::<Vec<Vec<f32>>>();
        let retention_times = transitions
            .iter()
            .enumerate()
            .map(|(column, transition)| {
                let count = intensities[column].len();
                (0..count)
                    .map(|point| {
                        let fraction = if count <= 1 {
                            0.0
                        } else {
                            point as f32 / (count - 1) as f32
                        };
                        transition.start_time
                            + fraction * (transition.end_time - transition.start_time)
                    })
                    .collect()
            })
            .collect();
        out.push(MrmExperimentSeries {
            experiment_index,
            transitions,
            retention_times,
            intensities,
        });
    }
    Ok(out)
}

pub fn read_tagged_mrm_series(
    path: &Path,
    source_analysis_number: usize,
) -> Result<MrmExperimentSeries> {
    let fragments = read_idx_float_records(path, source_analysis_number)?;
    let transitions = read_transitions(path, source_analysis_number)?;
    decode_tagged_mrm_series(&fragments, transitions)
}

fn decode_tagged_mrm_series(
    fragments: &[IndexedFloatRecord],
    transitions: Vec<Transition>,
) -> Result<MrmExperimentSeries> {
    let channel_count = transitions.len();
    if channel_count == 0 {
        return Err(ReaderError::Unsupported(
            "SCIEX MRM metadata has no transitions".into(),
        ));
    }
    let marker = -(channel_count as f32) - 0.01;
    let marker_count = fragments
        .iter()
        .flat_map(|fragment| fragment.fields.iter())
        .filter(|value| (**value - marker).abs() < 0.001)
        .count();
    if marker_count * 10 < fragments.len() * 9 {
        return Err(ReaderError::Unsupported(
            "SCIEX MRM tagged marker is not record-aligned".into(),
        ));
    }
    let transitions = transitions
        .into_iter()
        .take(channel_count)
        .collect::<Vec<_>>();
    let record_marker = marker;
    let flat = fragments
        .iter()
        .flat_map(|fragment| fragment.fields.iter().copied())
        .collect::<Vec<_>>();
    let starts = flat
        .iter()
        .enumerate()
        .filter_map(|(index, value)| ((value - record_marker).abs() < 0.001).then_some(index))
        .collect::<Vec<_>>();
    if starts.is_empty() {
        return Err(ReaderError::Unsupported(
            "SCIEX MRM payload has no tagged record marker".into(),
        ));
    }
    let mut events = Vec::with_capacity(starts.len());
    for (event_index, start) in starts.iter().enumerate() {
        let end = starts.get(event_index + 1).copied().unwrap_or(flat.len());
        let mut values = vec![0.0; channel_count];
        let mut position = 0usize;
        let mut valid = true;
        for value in &flat[start + 1..end] {
            if *value < 0.0 {
                position = position.saturating_add((-*value).round() as usize);
                if position > values.len() {
                    valid = false;
                }
            } else if position < values.len() {
                values[position] = *value;
                position += 1;
            } else {
                valid = false;
            }
        }
        let complete = valid && position == values.len();
        events.push((values, complete, valid));
    }
    let has_signal = |event: &(Vec<f32>, bool, bool)| event.0.iter().any(|value| *value != 0.0);
    let is_active = |channel: usize, rt: f32| {
        transitions[channel].start_time >= transitions[channel].end_time
            || (rt >= transitions[channel].start_time && rt <= transitions[channel].end_time)
    };
    let min_delta = -16i32;
    let max_delta = 16.max((events.len().abs_diff(fragments.len()) + 16) as i32);
    let width = (max_delta - min_delta + 1) as usize;
    let offset = |delta: i32| (delta - min_delta) as usize;
    let impossible = i64::MIN / 4;
    let mut parent = vec![vec![None; width]; events.len() + 1];
    let mut row = vec![impossible; width];
    row[offset(0)] = 0;
    for event_index in 0..=events.len() {
        for delta in ((min_delta + 1)..=max_delta).rev() {
            let cycle = event_index as i32 - delta;
            if row[offset(delta)] != impossible
                && cycle >= 0
                && (cycle as usize) < fragments.len()
                && !(0..channel_count).any(|channel| {
                    is_active(
                        channel,
                        fragments[cycle as usize].index.retention_time_minutes,
                    )
                })
                && row[offset(delta)] > row[offset(delta - 1)]
            {
                row[offset(delta - 1)] = row[offset(delta)];
                parent[event_index][offset(delta - 1)] = Some((event_index, delta, b'c'));
            }
        }
        if event_index == events.len() {
            break;
        }
        let mut next = vec![impossible; width];
        for delta in min_delta..=max_delta {
            let cycle = event_index as i32 - delta;
            if row[offset(delta)] == impossible {
                continue;
            }
            if events[event_index].2 && cycle >= 0 && (cycle as usize) < fragments.len() {
                let inactive = events[event_index]
                    .0
                    .iter()
                    .enumerate()
                    .filter(|(channel, value)| {
                        **value != 0.0
                            && !is_active(
                                *channel,
                                fragments[cycle as usize].index.retention_time_minutes,
                            )
                    })
                    .count() as i64;
                let nonzero = events[event_index]
                    .0
                    .iter()
                    .filter(|value| **value != 0.0)
                    .count() as i64;
                let score = row[offset(delta)] + nonzero * 10 - inactive * 10_000;
                if score > next[offset(delta)] {
                    next[offset(delta)] = score;
                    parent[event_index + 1][offset(delta)] = Some((event_index, delta, b'm'));
                }
            }
            if !events[event_index].1
                && !has_signal(&events[event_index])
                && event_index > 0
                && !has_signal(&events[event_index - 1])
                && delta < max_delta
                && row[offset(delta)] > next[offset(delta + 1)]
            {
                next[offset(delta + 1)] = row[offset(delta)];
                parent[event_index + 1][offset(delta + 1)] = Some((event_index, delta, b'e'));
            }
        }
        row = next;
    }
    let final_delta = events.len() as i32 - fragments.len() as i32;
    if final_delta < min_delta || final_delta > max_delta || row[offset(final_delta)] == impossible
    {
        return Err(ReaderError::Unsupported(
            "SCIEX tagged MRM payload cannot be reconciled to native acquisition cycles".into(),
        ));
    }
    let mut intensities = vec![vec![0.0; fragments.len()]; channel_count];
    let mut event_index = events.len();
    let mut delta = final_delta;
    while event_index > 0 || delta != 0 {
        let Some((prior_event, prior_delta, action)) = parent[event_index][offset(delta)] else {
            return Err(ReaderError::Unsupported(
                "SCIEX tagged MRM cycle reconciliation has no monotonic predecessor".into(),
            ));
        };
        if action == b'm' {
            let cycle = prior_event as i32 - prior_delta;
            if cycle < 0 || cycle as usize >= fragments.len() {
                return Err(ReaderError::Invalid(
                    "SCIEX tagged MRM cycle reconciliation produced an invalid cycle".into(),
                ));
            }
            for (channel, value) in events[prior_event].0.iter().enumerate() {
                intensities[channel][cycle as usize] = *value;
            }
        }
        event_index = prior_event;
        delta = prior_delta;
    }
    let time = fragments
        .iter()
        .map(|fragment| fragment.index.retention_time_minutes)
        .collect::<Vec<_>>();
    Ok(MrmExperimentSeries {
        experiment_index: 0,
        transitions,
        retention_times: (0..channel_count).map(|_| time.clone()).collect(),
        intensities,
    })
}

pub fn read_native_mrm_series(
    path: &Path,
    source_analysis_number: usize,
) -> Result<Vec<MrmExperimentSeries>> {
    let fragments = read_idx_float_records(path, source_analysis_number)?;
    let transitions = read_transitions(path, source_analysis_number)?;
    if fragments.iter().all(|fragment| fragment.fields.len() == 2) && transitions.len() == 2 {
        let mut first = vec![0.0; 3];
        let mut second = vec![0.0; 3];
        first.extend(fragments.iter().skip(3).map(|fragment| fragment.fields[0]));
        second.extend(fragments.iter().skip(3).map(|fragment| fragment.fields[1]));
        let time = fragments
            .iter()
            .map(|fragment| fragment.index.retention_time_minutes)
            .collect::<Vec<_>>();
        return Ok(vec![MrmExperimentSeries {
            experiment_index: 0,
            transitions,
            retention_times: vec![time.clone(), time],
            intensities: vec![first, second],
        }]);
    }
    if let Ok(tagged) = decode_tagged_mrm_series(&fragments, transitions.clone()) {
        return Ok(vec![tagged]);
    }
    let mut groups: Vec<Vec<IndexedFloatRecord>> = Vec::new();
    for fragment in fragments {
        if let Some(group) = groups.last_mut() {
            if group[0].fields.len() == fragment.fields.len() {
                group.push(fragment);
                continue;
            }
        }
        groups.push(vec![fragment]);
    }
    let mut series = Vec::new();
    for (experiment_index, group) in groups.into_iter().enumerate() {
        let width = group[0].fields.len();
        let transitions =
            read_transitions_for_experiment(path, source_analysis_number, experiment_index, 0)?;
        if width == 0 || transitions.len() < width {
            return Err(ReaderError::Unsupported(
                "SCIEX MRM payload width does not match method transitions".into(),
            ));
        }
        let transitions = transitions.into_iter().take(width).collect::<Vec<_>>();
        let intensities = (0..width)
            .map(|column| {
                group
                    .iter()
                    .map(|record| record.fields[column])
                    .collect::<Vec<_>>()
            })
            .collect::<Vec<_>>();
        let time = group
            .iter()
            .map(|record| record.index.retention_time_minutes)
            .collect::<Vec<_>>();
        let retention_times = (0..width).map(|_| time.clone()).collect();
        series.push(MrmExperimentSeries {
            experiment_index,
            transitions,
            retention_times,
            intensities,
        });
    }
    Ok(series)
}

pub fn read_scan_blocks(path: &Path) -> Result<Vec<ScanBlock>> {
    let bytes = fs::read(scan_path_for_wiff(path))?;
    let mut offsets = Vec::new();
    for offset in (0..bytes.len().saturating_sub(3)).step_by(4) {
        if bytes[offset..offset + 4] == 0x11111111u32.to_le_bytes() {
            offsets.push(offset);
        }
    }
    if offsets.is_empty() {
        return Err(ReaderError::Invalid(
            "Sciex WIFF scan contains no sample blocks".into(),
        ));
    }
    offsets
        .iter()
        .enumerate()
        .map(|(index, &offset)| {
            let end = offsets.get(index + 1).copied().unwrap_or(bytes.len());
            if end <= offset + 12 {
                return Err(ReaderError::Invalid(
                    "Sciex WIFF scan sample block is truncated".into(),
                ));
            }
            Ok(ScanBlock {
                sample_number: read_u32(&bytes, offset + 8)?,
                offset,
                bytes: bytes[offset..end].to_vec(),
            })
        })
        .collect()
}

fn first_utf16_string(bytes: &[u8]) -> String {
    for start in (0..bytes.len().saturating_sub(1)).step_by(2) {
        let mut candidate = String::new();
        for pair in bytes[start..].chunks_exact(2) {
            let c = pair[0];
            let high = pair[1];
            if c == 0 && high == 0 {
                break;
            }
            if high == 0 && (32..=126).contains(&c) {
                candidate.push(c as char);
            } else {
                break;
            }
        }
        if candidate.len() >= 2 {
            return candidate;
        }
    }
    String::new()
}

pub fn read_analysis_catalog(path: &Path) -> Result<Vec<crate::reader::Analysis>> {
    let file = cfb::open(path)?;
    let mut metadata_numbers = BTreeSet::new();
    let mut indexed_numbers = BTreeSet::new();
    for entry in file.walk() {
        if !entry.is_stream() {
            continue;
        }
        let normalized = normalize_stream_path(&entry.path().to_string_lossy());
        let Some(rest) = normalized.strip_prefix("samplesubtree/sample") else {
            continue;
        };
        let Some((number, suffix)) = rest.split_once('/') else {
            continue;
        };
        if suffix == "sampledabe/data" {
            if let Ok(number) = number.parse::<usize>() {
                metadata_numbers.insert(number);
            }
        } else if suffix == "idx" {
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
    let mut names = BTreeSet::new();
    source_numbers
        .into_iter()
        .enumerate()
        .map(|(analysis_index, source_number)| {
            let stream = read_stream(
                path,
                &format!("SampleSubtree/Sample{source_number}/SampleDABE/DATA"),
            )?;
            let mut name = first_utf16_string(&stream);
            if name.is_empty() || name == "none" {
                name = format!("sample_{source_number}");
            }
            if !names.insert(name.clone()) {
                let base_name = name.clone();
                name = format!("{base_name}_sample_{source_number}");
                if !names.insert(name.clone()) {
                    name = format!("{base_name}_analysis_{analysis_index}");
                    names.insert(name.clone());
                }
            }
            Ok(crate::reader::Analysis {
                analysis_index,
                source_analysis_number: Some(source_number),
                name,
                analysis_count: count,
            })
        })
        .collect()
}

pub fn read_event_records(block: &ScanBlock) -> Result<Vec<EventRecord>> {
    let mut markers = Vec::new();
    for offset in (24..block.bytes.len().saturating_sub(3)).step_by(4) {
        if (read_f32(&block.bytes, offset)? + 59.01).abs() < 0.001 {
            markers.push(offset);
        }
    }
    markers
        .iter()
        .enumerate()
        .map(|(ordinal, &offset)| {
            let end = markers
                .get(ordinal + 1)
                .copied()
                .unwrap_or(block.bytes.len());
            let mut fields = Vec::new();
            for pos in (offset + 4..end).step_by(4) {
                fields.push(read_f32(&block.bytes, pos)?);
            }
            Ok(EventRecord {
                ordinal,
                retention_time_minutes: 0.0,
                fields,
            })
        })
        .collect()
}

pub fn decode_intensity_groups(record: &EventRecord) -> Vec<IntensityGroup> {
    let mut groups = Vec::new();
    for &value in &record.fields {
        if value < 0.0 {
            groups.push(IntensityGroup {
                field_code: value.round() as i32,
                intensities: Vec::new(),
            });
        } else if let Some(group) = groups.last_mut() {
            group.intensities.push(value);
        }
    }
    groups.retain(|group| !group.intensities.is_empty());
    groups
}

fn utf16_name_start(bytes: &[u8], offset: usize) -> bool {
    offset >= 2 && bytes.get(offset + 1) == Some(&0) && bytes[offset - 2] < 32
}

fn read_transition_name(bytes: &[u8], offset: usize) -> (String, usize, bool, bool) {
    let mut name = String::new();
    let mut cursor = offset;
    while cursor + 1 < bytes.len() {
        let c = bytes[cursor];
        let high = bytes[cursor + 1];
        if c == 0 && high == 0 {
            break;
        }
        if high != 0 || !(32..=126).contains(&c) {
            break;
        }
        name.push(c as char);
        cursor += 2;
    }
    let quoted = name.starts_with('"');
    let prefixed = name.starts_with('*') || name.starts_with('&');
    while name.starts_with(['"', '*', '&', ' ']) {
        name.remove(0);
    }
    (name, cursor, quoted, prefixed)
}

pub fn read_transitions_for_experiment(
    path: &Path,
    source_analysis_number: usize,
    period: usize,
    experiment: usize,
) -> Result<Vec<Transition>> {
    let bytes = read_stream(
        path,
        &format!("MethodSubtree/Method1/DeviceMethod0/Period{period}/Experiment{experiment}/MassRangeEx/MassRangeEx"),
    )?;
    let mut transitions = Vec::new();
    let mut seen = BTreeSet::new();
    for offset in (0..bytes.len().saturating_sub(4)).step_by(2) {
        if !utf16_name_start(&bytes, offset) {
            continue;
        }
        let (name, cursor, quoted, prefixed) = read_transition_name(&bytes, offset);
        if name.len() < 3 || name == "CXP" || !seen.insert(name.clone()) {
            continue;
        }
        let field_offset = if quoted || prefixed { 20 } else { 22 };
        if offset < field_offset {
            continue;
        }
        let precursor = read_f32(&bytes, offset - field_offset)?.max(0.0);
        let product = read_f32(&bytes, offset - field_offset + 8)?.max(0.0);
        let ce_marker = [b'C', 0, b'E', 0, 0, 0];
        let end = (cursor + 80).min(bytes.len());
        let ce_start = bytes[cursor..end]
            .windows(ce_marker.len())
            .position(|window| window == ce_marker.as_slice())
            .map(|position| cursor + position);
        let Some(ce_start) = ce_start else {
            continue;
        };
        let collision_energy = read_f32(&bytes, ce_start + 8).unwrap_or(0.0);
        transitions.push(Transition {
            name,
            precursor_mz: precursor,
            product_mz: product,
            start_time: 0.0,
            end_time: 0.0,
            collision_energy,
        });
    }
    let times = match read_stream(
        path,
        &format!(
            "SampleSubtree/Sample{source_analysis_number}/SampleDAM/sMRMPro_adw1/sMRMPro_adw_Times"
        ),
    ) {
        Ok(times) => times,
        Err(_) => return Ok(transitions),
    };
    let available_pairs = times.len().saturating_sub(16) / 8;
    let pair_offset = if available_pairs == transitions.len() {
        0
    } else {
        2
    };
    if times.len() >= 16 && available_pairs >= transitions.len() + pair_offset {
        for (index, transition) in transitions.iter_mut().enumerate() {
            let offset = 16 + (index + pair_offset) * 8;
            transition.start_time = read_u32(&times, offset)? as f32 / 60000.0;
            transition.end_time = read_u32(&times, offset + 4)? as f32 / 60000.0;
        }
    }
    Ok(transitions)
}

pub fn read_transitions(path: &Path, source_analysis_number: usize) -> Result<Vec<Transition>> {
    read_transitions_for_experiment(path, source_analysis_number, 0, 0)
}

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::reader::{Chromatogram, Spectrum};

#[derive(Debug, Clone)]
pub struct ScanMetadata {
    pub scan: i32,
    pub packet_start: u64,
    pub centroid_offset: u64,
    pub centroid_count: usize,
    pub level: i32,
    pub mode: i32,
    pub polarity: i32,
    pub precursor_mz: f64,
    pub precursor_charge: i32,
    pub collision_energy: f32,
    pub isolation_width: f32,
    pub profile_bins: i32,
    pub profile_first: f64,
    pub profile_step: f64,
    pub profile_coefficients: Vec<f64>,
    pub low_mz: f32,
    pub high_mz: f32,
    pub base_peak_mz: f32,
    pub base_peak_intensity: f32,
    pub tic: f32,
    pub retention_time: f32,
}

#[derive(Debug, Clone)]
pub struct ThermoMetadata {
    pub scans: Vec<ScanMetadata>,
    pub chromatograms: Vec<Chromatogram>,
    pub time_stamp: String,
}

struct ThermoLayout {
    data_addr: usize,
    scan_index_addr: usize,
    first_scan: u32,
    last_scan: u32,
    error_log_addr: usize,
    scan_trailer_addr: usize,
    scan_params_addr: usize,
    event_count: usize,
}

fn u32_at(bytes: &[u8], offset: usize) -> Option<u32> {
    bytes
        .get(offset..offset.checked_add(4)?)
        .map(|value| u32::from_le_bytes(value.try_into().unwrap()))
}

fn u64_at(bytes: &[u8], offset: usize) -> Option<u64> {
    bytes
        .get(offset..offset.checked_add(8)?)
        .map(|value| u64::from_le_bytes(value.try_into().unwrap()))
}

fn f32_at(bytes: &[u8], offset: usize) -> Option<f32> {
    Some(f32::from_bits(u32_at(bytes, offset)?))
}

fn f64_at(bytes: &[u8], offset: usize) -> Option<f64> {
    Some(f64::from_bits(u64_at(bytes, offset)?))
}

fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
    (if month <= 2 { y + 1 } else { y }, month, day)
}

fn audit_timestamp(bytes: &[u8]) -> String {
    let Some(filetime) = u64_at(bytes, 0x28) else {
        return String::new();
    };
    const WINDOWS_EPOCH_100NS: u64 = 116_444_736_000_000_000;
    if filetime < WINDOWS_EPOCH_100NS {
        return String::new();
    }
    let unix_ms = (filetime - WINDOWS_EPOCH_100NS) / 10_000;
    let seconds = (unix_ms / 1_000) as i64;
    let millis = unix_ms % 1_000;
    let days = seconds.div_euclid(86_400);
    let day_seconds = seconds.rem_euclid(86_400);
    let (year, month, day) = civil_from_days(days);
    let hour = day_seconds / 3_600;
    let minute = (day_seconds % 3_600) / 60;
    let second = day_seconds % 60;
    format!("{year:04}-{month:02}-{day:02}T{hour:02}:{minute:02}:{second:02}.{millis:03}Z")
}

fn i32_at(bytes: &[u8], offset: usize) -> Option<i32> {
    bytes
        .get(offset..offset.checked_add(4)?)
        .map(|value| i32::from_le_bytes(value.try_into().unwrap()))
}

fn packet_header(bytes: &[u8], offset: usize) -> Option<(usize, usize, f32, f32)> {
    let profile_tokens = u32_at(bytes, offset + 4)? as usize;
    let profile_groups = u32_at(bytes, offset + 8)?;
    let encoding = u32_at(bytes, offset + 12)?;
    let centroid_count = u32_at(bytes, offset + 16)? as usize;
    let centroid_count_plus_one = u32_at(bytes, offset + 20)? as usize;
    let low_mz = f32_at(bytes, offset + 32)?;
    let high_mz = f32_at(bytes, offset + 36)?;
    if encoding != 128
        || profile_tokens < 7
        || profile_groups as usize != centroid_count.saturating_mul(2).saturating_add(1)
        || centroid_count_plus_one != centroid_count.saturating_add(1)
        || !low_mz.is_finite()
        || !high_mz.is_finite()
        || low_mz <= 0.0
        || high_mz <= low_mz
        || high_mz > 100_000.0
    {
        return None;
    }
    let centroid_offset = offset.checked_add((profile_tokens + 11).checked_mul(4)?)?;
    if centroid_count == 0 {
        return Some((profile_tokens, centroid_count, low_mz, high_mz));
    }
    let first_mz = f32_at(bytes, centroid_offset)?;
    let first_intensity = f32_at(bytes, centroid_offset + 4)?;
    if !first_mz.is_finite()
        || !first_intensity.is_finite()
        || first_mz < low_mz - 5.0
        || first_mz > high_mz + 5.0
        || first_intensity < 0.0
    {
        return None;
    }
    Some((profile_tokens, centroid_count, low_mz, high_mz))
}

fn pascal_end(bytes: &[u8], offset: usize) -> Option<usize> {
    let chars = u32_at(bytes, offset)? as usize;
    offset
        .checked_add(4)?
        .checked_add(chars.checked_mul(2)?)
        .filter(|end| *end <= bytes.len())
}

fn utf16_at(bytes: &[u8], offset: usize, chars: usize) -> Option<String> {
    let end = offset.checked_add(chars.checked_mul(2)?)?;
    let units: Vec<u16> = bytes
        .get(offset..end)?
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect();
    Some(
        String::from_utf16_lossy(&units)
            .trim_end_matches('\0')
            .to_string(),
    )
}

fn ascii_at(bytes: &[u8], offset: usize, length: usize) -> Option<String> {
    let end = offset.checked_add(length)?;
    let raw = bytes.get(offset..end)?;
    let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
    Some(String::from_utf8_lossy(&raw[..end]).into_owned())
}

fn generic_field_size(type_code: u32, length: u32) -> usize {
    match type_code {
        0 => 0,
        1..=5 => 1,
        6 | 7 => 2,
        8..=10 => 4,
        11 => 8,
        12 => length as usize,
        13 => length as usize * 2,
        _ => 0,
    }
}

struct ScanParamsHeader {
    fields: Vec<(String, u32, u32)>,
    record_size: usize,
}

fn generic_header_at(bytes: &[u8], offset: usize) -> Option<(Vec<(String, u32, u32)>, usize)> {
    let field_count = u32_at(bytes, offset)? as usize;
    if !(2..=500).contains(&field_count) {
        return None;
    }
    let mut cursor = offset.checked_add(4)?;
    let mut fields = Vec::with_capacity(field_count);
    let mut size = 0usize;
    for _ in 0..field_count {
        let type_code = u32_at(bytes, cursor)?;
        let length = u32_at(bytes, cursor + 4)?;
        cursor = cursor.checked_add(8)?;
        if type_code > 13 || length > 4096 {
            return None;
        }
        let chars = u32_at(bytes, cursor)? as usize;
        cursor = cursor.checked_add(4)?;
        if chars > 200 || cursor.checked_add(chars.checked_mul(2)?)? > bytes.len() {
            return None;
        }
        let label = utf16_at(bytes, cursor, chars)?;
        cursor = cursor.checked_add(chars.checked_mul(2)?)?;
        size = size.checked_add(generic_field_size(type_code, length))?;
        fields.push((label, type_code, length));
    }
    if fields.iter().filter(|field| !field.0.is_empty()).count() < 2 || size == 0 {
        return None;
    }
    Some((fields, size))
}

fn find_scan_params(bytes: &[u8], layout: &ThermoLayout) -> Option<ScanParamsHeader> {
    let scan_count = layout
        .last_scan
        .saturating_sub(layout.first_scan)
        .saturating_add(1) as usize;
    let tail = bytes.len().saturating_sub(layout.scan_params_addr);
    let expected = if scan_count > 0 && tail >= 4 {
        Some(tail / scan_count)
    } else {
        None
    };
    let start = layout.error_log_addr.min(bytes.len().saturating_sub(4));
    let end = layout.scan_trailer_addr.min(bytes.len().saturating_sub(4));
    let probe_end = end.saturating_sub(start).min(4 * 1024 * 1024);
    for pass in 0..2 {
        let mut offset = start;
        while offset + 4 <= start.saturating_add(probe_end) {
            if let Some((fields, size)) = generic_header_at(bytes, offset) {
                let size_ok = match (pass, expected) {
                    (0, Some(wanted)) => size == wanted,
                    _ => true,
                };
                if size_ok
                    && layout
                        .scan_params_addr
                        .checked_add(scan_count.saturating_mul(size.max(1)))
                        .map_or(false, |span_end| span_end <= bytes.len())
                {
                    return Some(ScanParamsHeader {
                        fields,
                        record_size: size,
                    });
                }
            }
            offset = offset.checked_add(2)?;
        }
        if expected.is_none() {
            break;
        }
    }
    None
}

struct ScanParamsValues {
    precursor_mz: f64,
    precursor_charge: i32,
    hcd_energy: String,
    hcd_energy_ev: f64,
    isolation_width: f64,
}

struct EventInfo {
    level: i32,
    polarity: i32,
    mode: i32,
    precursor_mz: f64,
    isolation_width: f64,
    collision_energy: f64,
    coefficients: Vec<f64>,
}

fn scan_params_values(
    bytes: &[u8],
    layout: &ThermoLayout,
    header: &ScanParamsHeader,
    index: usize,
) -> ScanParamsValues {
    let mut values = ScanParamsValues {
        precursor_mz: 0.0,
        precursor_charge: 0,
        hcd_energy: String::new(),
        hcd_energy_ev: 0.0,
        isolation_width: 0.0,
    };
    let mut cursor = match layout
        .scan_params_addr
        .checked_add(index.saturating_mul(header.record_size.max(1)))
    {
        Some(cursor) => cursor,
        None => return values,
    };
    let mut valid = true;
    for (label, type_code, length) in &header.fields {
        match *type_code {
            0 => {}
            1..=5 => {
                if label == "Charge State:" && *type_code == 4 && values.precursor_charge == 0 {
                    values.precursor_charge = bytes.get(cursor).copied().unwrap_or(0) as i32;
                }
                cursor = match cursor.checked_add(1) {
                    Some(next) => next,
                    None => {
                        valid = false;
                        break;
                    }
                };
            }
            6 | 7 => {
                cursor = match cursor.checked_add(2) {
                    Some(next) => next,
                    None => {
                        valid = false;
                        break;
                    }
                }
            }
            8..=10 => {
                if label == "Charge State:" && *type_code == 8 {
                    values.precursor_charge = i32_at(bytes, cursor).unwrap_or(0);
                }
                cursor = match cursor.checked_add(4) {
                    Some(next) => next,
                    None => {
                        valid = false;
                        break;
                    }
                };
            }
            11 => {
                let value = f64_at(bytes, cursor).unwrap_or(0.0);
                match label.as_str() {
                    "Monoisotopic M/Z:"
                    | "MS2 Isolation M/Z:"
                    | "Isolation Center M/Z:"
                    | "Precursor M/Z:" => {
                        if value > 0.0 {
                            values.precursor_mz = value;
                        }
                    }
                    "HCD Energy (eV):" | "HCD Energy eV:" => {
                        if value > 0.0 {
                            values.hcd_energy_ev = value;
                        }
                    }
                    "MS2 Isolation Width:" | "MSn Isolation Width:" => {
                        if value > 0.0 {
                            values.isolation_width = value;
                        }
                    }
                    _ => {}
                }
                cursor = match cursor.checked_add(8) {
                    Some(next) => next,
                    None => {
                        valid = false;
                        break;
                    }
                };
            }
            12 => {
                if matches!(label.as_str(), "HCD Energy:" | "HCD Energy V:" | "CE:") {
                    values.hcd_energy =
                        ascii_at(bytes, cursor, *length as usize).unwrap_or_default();
                }
                cursor = match cursor.checked_add(*length as usize) {
                    Some(next) => next,
                    None => {
                        valid = false;
                        break;
                    }
                };
            }
            13 => {
                cursor = match cursor.checked_add(*length as usize * 2) {
                    Some(next) => next,
                    None => {
                        valid = false;
                        break;
                    }
                }
            }
            _ => {
                valid = false;
                break;
            }
        }
    }
    if !valid {
        values.precursor_mz = 0.0;
        values.precursor_charge = 0;
        values.hcd_energy.clear();
        values.hcd_energy_ev = 0.0;
        values.isolation_width = 0.0;
    }
    values
}

fn collision_energy_from(values: &ScanParamsValues) -> f32 {
    let trimmed = values.hcd_energy.trim().trim_end_matches('%');
    if let Ok(energy) = trimmed.parse::<f64>() {
        if energy > 0.0 {
            return energy as f32;
        }
    }
    if values.hcd_energy_ev > 0.0 {
        return values.hcd_energy_ev as f32;
    }
    0.0
}

fn read_layout(bytes: &[u8]) -> Result<ThermoLayout, String> {
    if u32_at(bytes, 0x24).is_none() || u32_at(bytes, 0).unwrap_or(0) & 0xffff != 0xa101 {
        return Err("invalid Thermo RAW file header".into());
    }
    let version = u32_at(bytes, 0x24).unwrap();
    if version < 64 {
        return Err(format!("unsupported Thermo RAW format version: {version}"));
    }
    let mut cursor = 1356usize
        .checked_add(64)
        .ok_or("Thermo RAW header overflow")?;
    for _ in 0..14 {
        cursor = pascal_end(bytes, cursor).ok_or("invalid Thermo RAW sequence row")?;
    }
    for _ in 0..2 {
        cursor = pascal_end(bytes, cursor).ok_or("invalid Thermo RAW sequence row")?;
    }
    cursor = cursor
        .checked_add(4)
        .ok_or("Thermo RAW sequence row overflow")?;
    for _ in 0..15 {
        cursor = pascal_end(bytes, cursor).ok_or("invalid Thermo RAW sequence row")?;
    }
    cursor = cursor
        .checked_add(24)
        .ok_or("Thermo RAW autosampler overflow")?;
    cursor = pascal_end(bytes, cursor).ok_or("invalid Thermo RAW autosampler record")?;
    let raw_info = cursor;
    let controller_count =
        u32_at(bytes, raw_info + 28).ok_or("Thermo RAW controller count is truncated")? as usize;
    let data_addr = usize::try_from(
        u64_at(bytes, raw_info + 808).ok_or("Thermo RAW data address is truncated")?,
    )
    .map_err(|_| "Thermo RAW data address exceeds host size")?;
    let run_header_addr = usize::try_from(
        u64_at(bytes, raw_info + 824).ok_or("Thermo RAW run-header address is truncated")?,
    )
    .map_err(|_| "Thermo RAW run-header address exceeds host size")?;
    if data_addr >= bytes.len() || run_header_addr >= bytes.len() {
        return Err("Thermo RAW pointer is outside the file".into());
    }
    let first_scan =
        u32_at(bytes, run_header_addr + 8).ok_or("Thermo RAW first scan is truncated")?;
    let last_scan =
        u32_at(bytes, run_header_addr + 12).ok_or("Thermo RAW last scan is truncated")?;
    if last_scan < first_scan {
        return Err("Thermo RAW scan range is invalid".into());
    }
    let address_base = run_header_addr
        .checked_add(592 + 13 * 520 + 16 + 40)
        .ok_or("Thermo RAW run-header overflow")?;
    let scan_index_addr = usize::try_from(
        u64_at(bytes, address_base).ok_or("Thermo RAW scan-index address is truncated")?,
    )
    .map_err(|_| "Thermo RAW scan-index address exceeds host size")?;
    let run_data_addr = usize::try_from(
        u64_at(bytes, address_base + 8).ok_or("Thermo RAW run data address is truncated")?,
    )
    .map_err(|_| "Thermo RAW run data address exceeds host size")?;
    let error_log_addr = usize::try_from(
        u64_at(bytes, address_base + 24).ok_or("Thermo RAW error-log address is truncated")?,
    )
    .map_err(|_| "Thermo RAW error-log address exceeds host size")?;
    let scan_trailer_addr = usize::try_from(
        u64_at(bytes, address_base + 40).ok_or("Thermo RAW trailer address is truncated")?,
    )
    .map_err(|_| "Thermo RAW trailer address exceeds host size")?;
    let scan_params_addr = usize::try_from(
        u64_at(bytes, address_base + 48).ok_or("Thermo RAW parameter address is truncated")?,
    )
    .map_err(|_| "Thermo RAW parameter address exceeds host size")?;
    let event_count = u32_at(bytes, run_header_addr + 592 + 13 * 520 + 16 + 8)
        .ok_or("Thermo RAW event count is truncated")? as usize;
    if run_data_addr != data_addr
        || scan_index_addr >= bytes.len()
        || scan_trailer_addr >= bytes.len()
    {
        return Err("Thermo RAW run-header pointers are inconsistent".into());
    }
    if controller_count == 0 || event_count == 0 {
        return Err("Thermo RAW has no mass-spectrometry controller".into());
    }
    Ok(ThermoLayout {
        data_addr,
        scan_index_addr,
        first_scan,
        last_scan,
        error_log_addr,
        scan_trailer_addr,
        scan_params_addr,
        event_count,
    })
}

fn event_info(bytes: &[u8], layout: &ThermoLayout, index: usize) -> EventInfo {
    let stream_end = layout.scan_params_addr;
    let stream_start = layout.scan_trailer_addr.saturating_add(4);
    let stream_size = stream_end.saturating_sub(stream_start);
    let event_size = if layout.event_count > 0 && stream_size % layout.event_count == 0 {
        stream_size / layout.event_count
    } else {
        0
    };
    let offset = stream_start.saturating_add(index.saturating_mul(event_size));
    if event_size < 136 || offset + 136 > bytes.len() {
        return EventInfo {
            level: 0,
            polarity: 0,
            mode: 0,
            precursor_mz: 0.0,
            isolation_width: 0.0,
            collision_energy: 0.0,
            coefficients: Vec::new(),
        };
    }
    let level = match bytes[offset + 6] {
        1 => 1,
        2..=8 => 2,
        _ => 0,
    };
    let polarity = match bytes[offset + 4] {
        0 => -1,
        1 => 1,
        _ => 0,
    };
    let mode = match bytes[offset + 5] {
        0 | 1 => bytes[offset + 5] as i32,
        _ => 0,
    };
    // Q Exactive-family dependent event bodies (uniform v66 layout, 144-byte
    // body) store: precursor m/z as f64 at body offset 4, isolation width as
    // f32 at body offset 12, and collision energy as f64 at body offset 20.
    // All three reproduced the paired mzML values exactly on the validated
    // 210705 and 220909 corpora and are used as fallbacks when the scan
    // parameters stream lacks the corresponding field.
    let body = offset + 136;
    let dependent = level >= 2 && event_size >= 144 && body + 32 <= bytes.len();
    // Q Exactive uniform events store the frequency-to-m/z calibration block
    // after the scan-window pair: nparam u32 at body offset 80, then nparam
    // f64 coefficients. Orbitrap nparam 5/7: m/z = A + B/f^2 + C/f^4 with
    // coefficients [2..5]; LTQ-FT nparam 4: A + B/f + C/f^2 with [1..4].
    let mut coefficients = Vec::new();
    if event_size >= 136 && body + 88 <= bytes.len() {
        let nparam = u32_at(bytes, body + 80).unwrap_or(0) as usize;
        let nparam = nparam.min((bytes.len().saturating_sub(body + 84)) / 8);
        for i in 0..nparam {
            coefficients.push(f64_at(bytes, body + 84 + i * 8).unwrap_or(0.0));
        }
    }
    EventInfo {
        level,
        polarity,
        mode,
        precursor_mz: if dependent {
            f64_at(bytes, body + 4).unwrap_or(0.0)
        } else {
            0.0
        },
        isolation_width: if dependent {
            f32_at(bytes, body + 12).unwrap_or(0.0) as f64
        } else {
            0.0
        },
        collision_energy: if dependent {
            f64_at(bytes, body + 20).unwrap_or(0.0)
        } else {
            0.0
        },
        coefficients,
    }
}

fn profile_mz(frequency: f64, coefficients: &[f64]) -> f64 {
    if frequency == 0.0 {
        return 0.0;
    }
    match coefficients.len() {
        4 => {
            let (a, b, c) = (coefficients[1], coefficients[2], coefficients[3]);
            a + b / frequency + c / (frequency * frequency)
        }
        5 | 7 => {
            let (a, b, c) = (coefficients[2], coefficients[3], coefficients[4]);
            let f2 = frequency * frequency;
            a + b / f2 + c / (f2 * f2)
        }
        _ => frequency,
    }
}

fn polarity_for_path(path: &Path) -> i32 {
    let name = path.to_string_lossy().to_ascii_lowercase();
    if name.contains("_neg") {
        -1
    } else if name.contains("_pos") {
        1
    } else {
        0
    }
}

fn scan_level(header: &[u8], offset: usize) -> i32 {
    let value = f32_at(header, offset + 52).unwrap_or(0.0);
    if (value + 0.8125).abs() < 0.01 {
        1
    } else if (value + 0.9375).abs() < 0.01 {
        2
    } else {
        0
    }
}

pub fn is_thermo_raw(path: &Path, bytes: &[u8]) -> bool {
    path.extension()
        .and_then(|value| value.to_str())
        .map(|value| value.eq_ignore_ascii_case("raw"))
        .unwrap_or(false)
        && bytes.windows(16).any(|window| {
            window == b"F\0i\0n\0n\0i\0g\0a\0n\0"
                || window == b"X\0c\0a\0l\0i\0b\0u\0r\0_\0S\0y\0s\0t\0e\0m\0"
        })
}

pub fn read_metadata(path: &Path, bytes: &[u8]) -> Result<ThermoMetadata, String> {
    let layout = read_layout(bytes)?;
    let fallback_polarity = polarity_for_path(path);
    let scan_params = find_scan_params(bytes, &layout);
    let record_size = 88;
    let mut scans = Vec::new();
    let mut previous_packet_offset = None;
    let scan_count =
        usize::try_from(u64::from(layout.last_scan) - u64::from(layout.first_scan) + 1)
            .map_err(|_| "Thermo RAW scan count exceeds host size".to_string())?;
    for index in 0..scan_count {
        let record = layout.scan_index_addr + index * record_size;
        if record + record_size > bytes.len() {
            return Err(format!(
                "Thermo RAW scan index is truncated at scan {}",
                index + 1
            ));
        }
        let packet_offset = u64_at(bytes, record + 72)
            .ok_or_else(|| "Thermo RAW scan packet offset is truncated".to_string())?;
        if previous_packet_offset.is_some_and(|previous| packet_offset <= previous) {
            return Err(format!(
                "Thermo RAW scan-index offsets are not increasing at scan {}",
                index + 1
            ));
        }
        let packet_offset = usize::try_from(packet_offset)
            .map_err(|_| "Thermo RAW packet offset exceeds host size".to_string())?;
        let packet_start = layout
            .data_addr
            .checked_add(packet_offset)
            .ok_or_else(|| "Thermo RAW packet offset overflow".to_string())?;
        let (_, centroid_count, _, _) = packet_header(bytes, packet_start)
            .ok_or_else(|| format!("Thermo RAW packet header is invalid at scan {}", index + 1))?;
        let centroid_offset = packet_start
            .checked_add((u32_at(bytes, packet_start + 4).unwrap() as usize + 11) * 4)
            .ok_or_else(|| "Thermo RAW centroid offset overflow".to_string())?;
        let centroid_end = centroid_offset
            .checked_add(centroid_count * 8)
            .ok_or_else(|| "Thermo RAW centroid section overflow".to_string())?;
        if centroid_end > bytes.len() {
            return Err(format!(
                "Thermo RAW centroid section is truncated at scan {}",
                index + 1
            ));
        }
        let event = event_info(bytes, &layout, index);
        let params = scan_params
            .as_ref()
            .map(|header| scan_params_values(bytes, &layout, header, index))
            .unwrap_or(ScanParamsValues {
                precursor_mz: 0.0,
                precursor_charge: 0,
                hcd_energy: String::new(),
                hcd_energy_ev: 0.0,
                isolation_width: 0.0,
            });
        let level = if event.level == 0 {
            scan_level(bytes, packet_start)
        } else {
            event.level
        };
        let params_energy = collision_energy_from(&params);
        // The scan-data packet carries a dense native profile grid when
        // profile_words > 0: a 24-byte preamble (first frequency f64, step
        // f64, chunk count u32, total bins u32) followed by sampled chunks.
        // Validated against paired profile mzML: the converted grid is
        // decimated, but the non-zero intensity sequence matches the native
        // samples 1:1 in order and the m/z endpoints match the converted
        // grid exactly.
        let mut profile_bins = 0i32;
        let mut profile_first = 0.0f64;
        let mut profile_step = 0.0f64;
        let profile_offset = packet_start.saturating_add(40);
        let profile_words = u32_at(bytes, packet_start + 4).unwrap_or(0) as usize;
        if profile_words > 5 && profile_offset + 24 <= bytes.len() {
            let first = f64_at(bytes, profile_offset).unwrap_or(0.0);
            let step = f64_at(bytes, profile_offset + 8).unwrap_or(0.0);
            let bins = u32_at(bytes, profile_offset + 20).unwrap_or(0);
            if first.is_finite()
                && step.is_finite()
                && step != 0.0
                && bins > 0
                && bins <= 8_000_000
                && profile_offset + 24 + 8 <= bytes.len()
            {
                profile_first = first;
                profile_step = step;
                profile_bins = bins as i32;
            }
        }
        scans.push(ScanMetadata {
            scan: u32_at(bytes, record + 4).unwrap_or(layout.first_scan + index as u32) as i32 + 1,
            packet_start: packet_start as u64,
            centroid_offset: centroid_offset as u64,
            centroid_count,
            mode: event.mode,
            level,
            polarity: if event.polarity == 0 {
                fallback_polarity
            } else {
                event.polarity
            },
            precursor_mz: if params.precursor_mz > 0.0 {
                params.precursor_mz
            } else if level >= 2 && event.precursor_mz > 0.0 {
                event.precursor_mz
            } else {
                0.0
            },
            precursor_charge: if level >= 2 {
                params.precursor_charge
            } else {
                0
            },
            collision_energy: if level >= 2 {
                if params_energy > 0.0 {
                    params_energy
                } else {
                    event.collision_energy as f32
                }
            } else {
                0.0
            },
            isolation_width: if level >= 2 {
                if params.isolation_width > 0.0 {
                    params.isolation_width as f32
                } else {
                    event.isolation_width as f32
                }
            } else {
                0.0
            },
            profile_bins,
            profile_first,
            profile_step,
            profile_coefficients: event.coefficients,
            low_mz: f64_at(bytes, record + 56).unwrap() as f32,
            high_mz: f64_at(bytes, record + 64).unwrap() as f32,
            base_peak_mz: f64_at(bytes, record + 48).unwrap() as f32,
            base_peak_intensity: f64_at(bytes, record + 40).unwrap() as f32,
            tic: f64_at(bytes, record + 32).unwrap() as f32,
            retention_time: (f64_at(bytes, record + 24).unwrap() * 60.0) as f32,
        });
        previous_packet_offset = Some(packet_offset as u64);
    }
    if scans.is_empty() {
        return Err("Thermo RAW scan metadata table is empty".to_string());
    }
    let time: Vec<f32> = scans.iter().map(|scan| scan.retention_time).collect();
    let tic = scans.iter().map(|scan| scan.tic).collect();
    let bpc = scans.iter().map(|scan| scan.base_peak_intensity).collect();
    let start_time = scans.first().map(|scan| scan.retention_time);
    let end_time = scans.last().map(|scan| scan.retention_time);
    let chromatograms = vec![
        Chromatogram {
            id: "TIC".into(),
            signal_type: "MS".into(),
            chromatogram_type: "TIC".into(),
            detector: "Thermo".into(),
            units: "counts".into(),
            time: time.clone(),
            intensity: tic,
            start_time,
            end_time,
            ..Default::default()
        },
        Chromatogram {
            id: "BPC".into(),
            signal_type: "MS".into(),
            chromatogram_type: "BPC".into(),
            detector: "Thermo".into(),
            units: "counts".into(),
            time,
            intensity: bpc,
            start_time,
            end_time,
            ..Default::default()
        },
    ];
    Ok(ThermoMetadata {
        scans,
        chromatograms,
        time_stamp: audit_timestamp(bytes),
    })
}

pub fn read_spectrum(
    path: &Path,
    metadata: &ScanMetadata,
    index: usize,
) -> Result<Spectrum, String> {
    let mut file = File::open(path).map_err(|error| error.to_string())?;
    let bytes = metadata
        .centroid_count
        .checked_mul(8)
        .ok_or_else(|| "Thermo RAW centroid size overflow".to_string())?;
    let mut payload = vec![0_u8; bytes];
    file.seek(SeekFrom::Start(metadata.centroid_offset))
        .map_err(|error| error.to_string())?;
    file.read_exact(&mut payload)
        .map_err(|error| error.to_string())?;
    let mut mz = Vec::with_capacity(metadata.centroid_count);
    let mut intensity = Vec::with_capacity(metadata.centroid_count);
    for point in 0..metadata.centroid_count {
        let offset = point * 8;
        mz.push(f32::from_le_bytes(
            payload[offset..offset + 4].try_into().unwrap(),
        ));
        intensity.push(f32::from_le_bytes(
            payload[offset + 4..offset + 8].try_into().unwrap(),
        ));
    }
    Ok(Spectrum {
        index: index as i32,
        scan: metadata.scan,
        array_length: metadata.centroid_count as i32,
        level: metadata.level,
        mode: metadata.mode,
        polarity: metadata.polarity,
        window_mz: if metadata.level >= 2 {
            metadata.precursor_mz as f32
        } else {
            0.0
        },
        window_mzlow: if metadata.level >= 2 && metadata.precursor_mz > 0.0 {
            (metadata.precursor_mz - metadata.isolation_width as f64 / 2.0) as f32
        } else {
            0.0
        },
        window_mzhigh: if metadata.level >= 2 && metadata.precursor_mz > 0.0 {
            (metadata.precursor_mz + metadata.isolation_width as f64 / 2.0) as f32
        } else {
            0.0
        },
        precursor_mz: metadata.precursor_mz as f32,
        precursor_charge: metadata.precursor_charge,
        collision_energy: metadata.collision_energy,
        low_mz: metadata.low_mz,
        high_mz: metadata.high_mz,
        base_peak_mz: metadata.base_peak_mz,
        base_peak_intensity: metadata.base_peak_intensity,
        tic: metadata.tic,
        retention_time: metadata.retention_time,
        mz,
        intensity,
        ..Default::default()
    })
}

/// Decode the native dense profile grid (frequency-domain samples converted
/// through the scan-event calibration) into the public m/z/intensity arrays.
/// Used only when a scan carries no centroid list. The grid is zero-filled;
/// the non-zero intensity sequence matches the paired profile mzML 1:1 in
/// order, and the grid m/z endpoints match the converted grid exactly.
pub fn read_profile_spectrum(
    path: &Path,
    metadata: &ScanMetadata,
    index: usize,
) -> Result<Spectrum, String> {
    if metadata.profile_bins <= 0 || metadata.profile_first == 0.0 || metadata.profile_step == 0.0 {
        return Err(format!(
            "Thermo RAW scan {} has no validated profile grid",
            index + 1
        ));
    }
    let file = File::open(path).map_err(|error| error.to_string())?;
    let size = file.metadata().map_err(|error| error.to_string())?.len() as usize;
    let profile_offset = (metadata.packet_start as usize)
        .checked_add(40)
        .ok_or_else(|| "Thermo RAW profile offset overflow".to_string())?;
    if profile_offset + 24 > size {
        return Err(format!(
            "Thermo RAW profile section is truncated at scan {}",
            index + 1
        ));
    }
    let file = File::open(path).map_err(|error| error.to_string())?;
    let mut reader = std::io::BufReader::new(file);
    use std::io::{Read, Seek, SeekFrom};
    reader
        .seek(SeekFrom::Start(profile_offset as u64))
        .map_err(|error| error.to_string())?;
    let mut preamble = [0_u8; 24];
    reader
        .read_exact(&mut preamble)
        .map_err(|error| error.to_string())?;
    let first = f64::from_le_bytes(preamble[0..8].try_into().unwrap());
    let step = f64::from_le_bytes(preamble[8..16].try_into().unwrap());
    let chunk_count = u32::from_le_bytes(preamble[16..20].try_into().unwrap()) as usize;
    let bins = u32::from_le_bytes(preamble[20..24].try_into().unwrap()) as usize;
    if bins != metadata.profile_bins as usize {
        return Err(format!(
            "Thermo RAW profile grid changed at scan {}",
            index + 1
        ));
    }
    let mut packet_header = [0_u8; 16];
    {
        let mut header_source =
            std::io::BufReader::new(File::open(path).map_err(|e| e.to_string())?);
        header_source
            .seek(SeekFrom::Start(metadata.packet_start))
            .map_err(|error| error.to_string())?;
        header_source
            .read_exact(&mut packet_header)
            .map_err(|error| error.to_string())?;
    }
    let layout = u32::from_le_bytes(packet_header[12..16].try_into().unwrap());
    let mut mz = vec![0.0_f32; bins];
    let mut intensity = vec![0.0_f32; bins];
    let coefficients = &metadata.profile_coefficients;
    for bin in 0..bins {
        let frequency = first + (bin as f64) * step;
        mz[bin] = profile_mz(frequency, coefficients) as f32;
    }
    if chunk_count == 0 || chunk_count > 65_536 {
        return Err(format!(
            "Thermo RAW profile chunk count is invalid at scan {}",
            index + 1
        ));
    }
    for _chunk in 0..chunk_count {
        let mut header = [0_u8; 8];
        reader
            .read_exact(&mut header)
            .map_err(|error| error.to_string())?;
        let first_bin = u32::from_le_bytes(header[0..4].try_into().unwrap()) as usize;
        let count = u32::from_le_bytes(header[4..8].try_into().unwrap()) as usize;
        let has_fudge = layout != 0;
        let fudge = if has_fudge {
            let mut fudge_bytes = [0_u8; 4];
            reader
                .read_exact(&mut fudge_bytes)
                .map_err(|error| error.to_string())?;
            f32::from_le_bytes(fudge_bytes) as f64
        } else {
            0.0
        };
        if count == 0 || count > bins || first_bin >= bins {
            continue;
        }
        let mut signal = vec![0_u8; count * 4];
        reader
            .read_exact(&mut signal)
            .map_err(|error| error.to_string())?;
        for i in 0..count {
            let bin = first_bin.saturating_add(i);
            if bin >= bins {
                break;
            }
            let value = f32::from_le_bytes(signal[i * 4..i * 4 + 4].try_into().unwrap());
            if value <= 0.0 {
                continue;
            }
            let frequency = first + (bin as f64) * step + fudge;
            mz[bin] = profile_mz(frequency, coefficients) as f32;
            intensity[bin] = value;
        }
    }
    Ok(Spectrum {
        index: index as i32,
        scan: metadata.scan,
        array_length: bins as i32,
        level: metadata.level,
        mode: metadata.mode,
        polarity: metadata.polarity,
        low_mz: metadata.low_mz,
        high_mz: metadata.high_mz,
        base_peak_mz: metadata.base_peak_mz,
        base_peak_intensity: metadata.base_peak_intensity,
        tic: metadata.tic,
        retention_time: metadata.retention_time,
        mz,
        intensity,
        ..Default::default()
    })
}

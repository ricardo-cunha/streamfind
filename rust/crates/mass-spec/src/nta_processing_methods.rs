use base64::{engine::general_purpose::STANDARD, Engine as _};
use serde_json::{json, Value};
use std::time::Instant;
use streamfind_rust_core::{Error, ErrorCode, Project, Result};

use crate::nta::{get_spectra_targets, TargetSpec};
use crate::reader::Reader;

fn invalid(s: impl Into<String>) -> Error {
    Error::new(ErrorCode::InvalidArgument, s)
}
fn sql(s: &str) -> String {
    format!("'{}'", s.replace('\'', "''"))
}
fn q(v: &[f32], p: f32) -> f32 {
    if v.is_empty() {
        return 0.0;
    }
    let mut x = v.to_vec();
    let index = ((x.len() - 1) as f32 * p.clamp(0., 1.)) as usize;
    *x.select_nth_unstable_by(index, f32::total_cmp).1
}
fn noise_levels(intensities: &[f32], noise_threshold: f32, base_quantile: f32) -> Vec<f32> {
    let nonzero: Vec<_> = intensities.iter().copied().filter(|x| *x > 0.).collect();
    let values = if nonzero.is_empty() {
        intensities
    } else {
        &nonzero
    };
    let m = mean(values);
    let cv = if m != 0. { sd(values, m) / m } else { 0. };
    let snr = if q(values, 0.25) > 0. {
        q(values, 0.9) / q(values, 0.25)
    } else {
        0.
    };
    let mut quantile = if cv > 2. {
        base_quantile * 0.5
    } else if cv > 1. {
        base_quantile
    } else {
        base_quantile * 2.
    };
    let multiplier = if snr > 100. {
        quantile *= 0.5;
        1.2
    } else if snr < 10. {
        quantile *= 1.5;
        0.8
    } else {
        1.
    };
    quantile = quantile.clamp(0.01, (base_quantile * 1.2).max(0.30));
    let n = intensities.len();
    let mut bins = ((n as f32).sqrt() * 1.5).clamp(10., 200.) as usize;
    let sparsity = n as f32 / bins as f32;
    if sparsity < 5. {
        bins = (n / 5).max(5);
    } else if sparsity > 50. {
        bins = (n / 20).min(200);
    }
    let mut data = vec![Vec::new(); bins];
    for (i, value) in intensities.iter().enumerate() {
        data[(i * bins / n.max(1)).min(bins - 1)].push(*value);
    }
    let levels: Vec<_> = data
        .iter()
        .map(|v| (q(v, quantile) * multiplier).max(noise_threshold))
        .collect();
    intensities
        .iter()
        .enumerate()
        .map(|(i, _)| levels[(i * bins / n.max(1)).min(bins - 1)])
        .collect()
}
fn mean(v: &[f32]) -> f32 {
    if v.is_empty() {
        0.
    } else {
        v.iter().sum::<f32>() / v.len() as f32
    }
}
fn sd(v: &[f32], m: f32) -> f32 {
    if v.is_empty() {
        0.
    } else {
        (v.iter().map(|x| (x - m).powi(2)).sum::<f32>() / v.len() as f32).sqrt()
    }
}
fn clusters(v: &[f32], ppm: f32) -> Vec<i32> {
    let mut out = vec![0; v.len()];
    for i in 1..v.len() {
        out[i] = out[i - 1] + (v[i] - v[i - 1] > v[i] * ppm / 1e6) as i32;
    }
    out
}
fn encode(v: &[f32]) -> String {
    let mut bytes = Vec::with_capacity(v.len() * 4);
    for x in v {
        bytes.extend(x.to_le_bytes());
    }
    STANDARD.encode(bytes)
}

#[allow(dead_code)]
#[derive(Clone)]
struct Point {
    rt: f32,
    mz: f32,
    intensity: f32,
    noise: f32,
    cluster: i32,
}
#[derive(Default)]
struct Feature {
    analysis: String,
    feature: String,
    adduct: String,
    rt: f32,
    mz: f32,
    mass: f32,
    intensity: f32,
    noise: f32,
    sn: f32,
    area: f32,
    rtmin: f32,
    rtmax: f32,
    width: f32,
    mzmin: f32,
    mzmax: f32,
    ppm: f32,
    fwhm_rt: f32,
    fwhm_mz: f32,
    ga: f32,
    gmu: f32,
    gs: f32,
    r2: f32,
    jag: f32,
    sharp: f32,
    asym: f32,
    modality: i32,
    plates: f32,
    polarity: i32,
    eic: Vec<(String, String)>,
}

fn baseline(y: &[f32], w: usize) -> Vec<f32> {
    let mut b = vec![0.; y.len()];
    for i in 0..y.len() {
        let a = i.saturating_sub(w);
        let z = (i + w).min(y.len() - 1);
        b[i] = y[a..=z].iter().copied().fold(f32::INFINITY, f32::min);
    }
    if y.len() > 2 {
        for i in 1..y.len() - 1 {
            b[i] = (b[i - 1] + b[i] + b[i + 1]) / 3.;
        }
    }
    b
}
fn smooth(y: &[f32]) -> Vec<f32> {
    let mut z = vec![0.; y.len()];
    for i in 0..y.len() {
        let a = i.saturating_sub(2);
        let b = (i + 2).min(y.len() - 1);
        z[i] = mean(&y[a..=b]);
    }
    z
}
fn fit(
    x: &[f32],
    y: &[f32],
    mut a: f32,
    mut mu: f32,
    mut sig: f32,
    mut base: f32,
) -> (f32, f32, f32, f32) {
    let (mut ma, mut va, mut mm, mut vm, mut ms, mut vs, mut mb, mut vb) =
        (0., 0., 0., 0., 0., 0., 0., 0.);
    for it in 1..=500 {
        let (mut ga, mut gm, mut gs, mut gb) = (0., 0., 0., 0.);
        for (&xx, &yy) in x.iter().zip(y) {
            let e = (-(xx - mu).powi(2) / (2. * sig * sig)).exp();
            let er = yy - (base + a * e);
            ga += -2. * er * e;
            gm += -2. * er * a * e * (xx - mu) / (sig * sig);
            gs += -2. * er * a * e * (xx - mu).powi(2) / (sig.powi(3));
            gb += -2. * er;
        }
        fn u(g: &mut f32, m: &mut f32, v: &mut f32, p: &mut f32, it: f32) {
            *m = 0.9 * *m + 0.1 * *g;
            *v = 0.999 * *v + 0.001 * (*g) * (*g);
            *p -= 0.01 * (*m / (1. - 0.9f32.powf(it)))
                / ((*v / (1. - 0.999f32.powf(it))).sqrt() + 1e-8);
        }
        let i = it as f32;
        u(&mut ga, &mut ma, &mut va, &mut a, i);
        a = a.max(0.1);
        u(&mut gm, &mut mm, &mut vm, &mut mu, i);
        u(&mut gs, &mut ms, &mut vs, &mut sig, i);
        sig = sig.clamp(0.1, 100.);
        u(&mut gb, &mut mb, &mut vb, &mut base, i);
        base = base.max(0.);
    }
    (a, mu, sig, base)
}
fn r2(x: &[f32], y: &[f32], a: f32, mu: f32, s: f32, b: f32) -> f32 {
    let m = mean(y);
    let (mut t, mut r) = (0., 0.);
    for (&xx, &yy) in x.iter().zip(y) {
        let p = b + a * (-(xx - mu).powi(2) / (2. * s * s)).exp();
        t += (yy - m).powi(2);
        r += (yy - p).powi(2);
    }
    if t > 0. {
        1. - r / t
    } else {
        0.
    }
}

fn make_feature(
    analysis: &str,
    polarity: i32,
    cluster: i32,
    apex: usize,
    left: usize,
    right: usize,
    rt: &[f32],
    mz: &[f32],
    raw: &[f32],
    base: &[f32],
    sm: &[f32],
    adduct: &str,
    correction: f32,
) -> Feature {
    let x = &rt[left..=right];
    let mm = &mz[left..=right];
    let y = &raw[left..=right];
    let sy = &sm[left..=right];
    let by = &base[left..=right];
    let ai = apex - left;

    // Max intensity within the allowed window around the candidate apex
    // (mirrors core process_polarity_clusters: allowed_shift = width * 0.5).
    let center_rt = x[ai];
    let allowed_shift = (x[x.len() - 1] - x[0]) * 0.5;
    let mut max_position = 0usize;
    let mut peak_max = f32::NEG_INFINITY;
    for (j, &rj) in x.iter().enumerate() {
        if (rj - center_rt).abs() <= allowed_shift && y[j] > peak_max {
            peak_max = y[j];
            max_position = j;
        }
    }
    if !peak_max.is_finite() {
        peak_max = y[ai];
        max_position = ai;
    }
    let rt_at_max = x[max_position];
    let mz_at_max = mm[max_position];

    let noise = if sy.len() >= 4 {
        (sy[0].min(sy[1]) + sy[sy.len() - 2].min(*sy.last().unwrap())) / 2.
    } else {
        sy.iter().copied().fold(f32::INFINITY, f32::min)
    };
    let sn = if noise > 0. { peak_max / noise } else { 0. };

    // FWHM (RT + m/z) and mean m/z over the FWHM RT region (core uses
    // calculate_fwhm_combined for the feature's m/z and FWHM columns).
    let (fwhm_rt, fwhm_mz, mean_mz_fwhm) = crate::nta_utils::calculate_fwhm_combined(x, mm, y);

    // Slope-check outlier interpolation before the gaussian fit (core lines
    // 1545-1667): mark non-rising (left of apex) / non-falling (right of apex)
    // smoothed points and linearly interpolate each consecutive run.
    let mut needs = vec![false; sy.len()];
    let mut interp_left = 0usize;
    let mut interp_right = 0usize;
    let si = sy
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.total_cmp(b.1))
        .map(|(i, _)| i)
        .unwrap_or(ai);
    if si > 1 {
        for i in 0..si - 1 {
            if needs[i] {
                continue;
            }
            let cur = sy[i];
            for j in i + 1..si {
                if sy[j] <= cur {
                    needs[j] = true;
                    interp_left += 1;
                } else {
                    break;
                }
            }
        }
    }
    if si < sy.len().saturating_sub(2) {
        for i in si + 1..sy.len() - 1 {
            if needs[i] {
                continue;
            }
            let cur = sy[i];
            for j in i + 1..sy.len() {
                if sy[j] >= cur {
                    needs[j] = true;
                    interp_right += 1;
                } else {
                    break;
                }
            }
        }
    }
    let mut fit_sy = sy.to_vec();
    if interp_left + interp_right > 0 {
        let mut k = 0usize;
        while k < needs.len() {
            if needs[k] {
                let start = k;
                let mut end = k;
                while end < needs.len() && needs[end] {
                    end += 1;
                }
                end -= 1;
                if start >= 1 && end + 1 < sy.len() {
                    let ib = sy[start - 1];
                    let ia = sy[end + 1];
                    let rb = x[start - 1];
                    let ra = x[end + 1];
                    for j in start..=end {
                        let t = (x[j] - rb) / (ra - rb);
                        fit_sy[j] = ib + t * (ia - ib);
                    }
                }
                k = end + 1;
            } else {
                k += 1;
            }
        }
    }

    // Gaussian fit on the corrected profile, initialised from the corrected
    // apex (mirrors core lines 1669-1715).
    let fwhm_peak_rt = crate::nta_utils::calculate_fwhm_rt(x, &fit_sy);
    let fit_max_position = fit_sy
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.total_cmp(b.1))
        .map(|(i, _)| i)
        .unwrap_or(0);
    let fit_max = fit_sy[fit_max_position];
    let fit_rt_at_max = x[fit_max_position];
    let gbin = fit_sy[0].min(*fit_sy.last().unwrap());
    let mut gsig = fwhm_peak_rt / 2.355;
    if gsig <= 0. {
        gsig = (x[x.len() - 1] - x[0]) / 4.;
    }
    let (ga, gmu, gs, gb) = fit(x, &fit_sy, fit_max - gbin, fit_rt_at_max, gsig, gbin);
    let g_r2 = r2(x, &fit_sy, ga, gmu, gs, gb);

    // Quality metrics on the same sources as core: area/jaggedness/sharpness/
    // asymmetry on raw intensity, modality on smoothed, plates from rt_at_max
    // and the FWHM RT value.
    let ar = crate::nta_utils::calculate_area(x, y);
    let jag = crate::nta_utils::calculate_jaggedness(y);
    let sharp = crate::nta_utils::calculate_sharpness(x, y, ar);
    let asym = crate::nta_utils::calculate_asymmetry(x, y);
    let modality = crate::nta_utils::calculate_modality(sy, 0.1);
    let plates = crate::nta_utils::calculate_theoretical_plates(rt_at_max, fwhm_rt);

    Feature {
        analysis: analysis.into(),
        feature: format!(
            "CL{}_PK{}_MZ{}_RT{}_{}",
            cluster,
            apex,
            mz_at_max.round() as i32,
            rt_at_max.round() as i32,
            if polarity > 0 { "POS" } else { "NEG" }
        ),
        adduct: adduct.into(),
        rt: rt_at_max,
        mz: mean_mz_fwhm,
        mass: mean_mz_fwhm + correction,
        intensity: peak_max,
        noise,
        sn,
        area: ar,
        rtmin: x[0],
        rtmax: *x.last().unwrap(),
        width: x[x.len() - 1] - x[0],
        mzmin: mm.iter().copied().fold(f32::INFINITY, f32::min),
        mzmax: mm.iter().copied().fold(f32::NEG_INFINITY, f32::max),
        ppm: (mm.iter().copied().fold(f32::NEG_INFINITY, f32::max)
            - mm.iter().copied().fold(f32::INFINITY, f32::min))
            / mean_mz_fwhm
            * 1e6,
        fwhm_rt,
        fwhm_mz,
        ga,
        gmu,
        gs,
        r2: g_r2,
        jag,
        sharp,
        asym,
        modality,
        plates,
        polarity,
        eic: vec![
            ("eic_rt".into(), encode(x)),
            ("eic_mz".into(), encode(mm)),
            ("eic_intensity".into(), encode(y)),
            ("eic_baseline".into(), encode(by)),
            ("eic_smoothed".into(), encode(sy)),
        ],
    }
}

fn valid_peaks(sm: &[f32], d: &[f32]) -> Vec<usize> {
    let mut out = Vec::new();
    for i in 1..d.len() {
        if d[i - 1] <= 0. || d[i] > 0. || i < 2 || i >= sm.len() - 2 {
            continue;
        }
        let half = sm[i] * 0.5;
        let mut left = i - 1;
        let mut rises = 0;
        while sm[left] >= half {
            if left > 0 && sm[left] < sm[left - 1] {
                rises += 1;
                if rises >= 2 {
                    break;
                }
            } else {
                rises = 0;
            }
            if left == 0 {
                break;
            }
            left -= 1;
        }
        let mut right = i + 1;
        rises = 0;
        while right < sm.len() && sm[right] >= half {
            if right + 1 < sm.len() && sm[right] < sm[right + 1] {
                rises += 1;
                if rises >= 2 {
                    break;
                }
            } else {
                rises = 0;
            }
            right += 1;
        }
        let pre = mean(&d[left..i]);
        let post = mean(&d[i..=right.min(d.len() - 1)]);
        let pre_apex = (left..i).any(|j| sm[j] > sm[i]);
        let post_apex = (i + 1..=right.min(sm.len() - 1)).any(|j| sm[j] > sm[i]);
        if pre > 0. && post < 0. && !pre_apex && !post_apex {
            out.push(i);
        }
    }
    out
}

fn boundaries(
    peak: usize,
    rt: &[f32],
    sm: &[f32],
    base: &[f32],
    half_width: f32,
) -> (usize, usize) {
    let apex = sm[peak];
    let minimum = apex * 0.01;
    let mut left = peak;
    for i in (0..peak).rev() {
        if (i + 1 < peak && rt[i + 1] - rt[i] > half_width) || rt[peak] - rt[i] > half_width {
            left = i + 1;
            break;
        }
        if sm[i] <= base[i] * 1.1 || sm[i] > apex * 1.2 || sm[i] <= minimum {
            left = i;
            break;
        }
        if i >= 2 && sm[i - 1] > sm[i] && sm[i - 2] > sm[i - 1] {
            left = i;
            break;
        }
        left = i;
    }
    let mut right = peak;
    for i in peak + 1..rt.len() {
        if (i > peak + 1 && rt[i] - rt[i - 1] > half_width) || rt[i] - rt[peak] > half_width {
            right = i - 1;
            break;
        }
        if sm[i] <= base[i] * 1.1 || sm[i] > apex * 1.2 || sm[i] <= minimum {
            right = i;
            break;
        }
        if i + 2 < rt.len() && sm[i + 1] > sm[i] && sm[i + 2] > sm[i + 1] {
            right = i;
            break;
        }
        right = i;
    }
    (left, right)
}

fn detect(
    analysis: &str,
    points: &mut Vec<Point>,
    min_traces: usize,
    min_snr: f32,
    baseline_window: f32,
    max_width: f32,
    polarity: i32,
    ppm: f32,
) -> Vec<Feature> {
    points.sort_by(|a, b| a.mz.total_cmp(&b.mz));
    let cs = clusters(&points.iter().map(|p| p.mz).collect::<Vec<_>>(), ppm);
    for (i, p) in points.iter_mut().enumerate() {
        p.cluster = cs[i];
    }
    let mut groups = std::collections::BTreeMap::<i32, Vec<Point>>::new();
    for p in points.drain(..) {
        groups.entry(p.cluster).or_default().push(p)
    }
    let mut out = Vec::new();
    for (c, g) in groups {
        let snr = g.iter().map(|p| p.intensity).fold(0., f32::max)
            / g.iter()
                .map(|p| p.intensity)
                .fold(f32::INFINITY, f32::min)
                .max(1e-8);
        if g.len() <= min_traces || snr <= min_snr {
            continue;
        }
        let mut g = g;
        g.sort_by(|a, b| a.rt.total_cmp(&b.rt));
        let (rt, mz, y): (Vec<_>, Vec<_>, Vec<_>) =
            g.iter().map(|p| (p.rt, p.mz, p.intensity)).unzip3();
        let cycle_time = if rt.len() > 1 {
            let mut diffs: Vec<f32> = rt.windows(2).map(|w| w[1] - w[0]).collect();
            diffs.sort_by(f32::total_cmp);
            diffs[diffs.len() / 2]
        } else {
            1.
        };
        let b = baseline(
            &y,
            (baseline_window / cycle_time).max(min_traces as f32) as usize / 2,
        );
        let sm = smooth(&y);
        let d: Vec<_> = sm.windows(2).map(|w| w[1] - w[0]).collect();
        let mut peaks = Vec::new();
        for i in valid_peaks(&sm, &d) {
            if i < min_traces / 2 || i >= rt.len() - min_traces / 2 {
                continue;
            }
            let (left, right) = boundaries(i, &rt, &sm, &b, max_width / 2.);
            if left < right {
                peaks.push((i, left, right));
            }
        }
        let mut merged = true;
        while merged {
            merged = false;
            'outer: for i in 0..peaks.len() {
                for j in i + 1..peaks.len() {
                    let (_, left_i, right_i) = peaks[i];
                    let (_, left_j, right_j) = peaks[j];
                    let overlaps = !(rt[right_i] < rt[left_j] || rt[right_j] < rt[left_i]);
                    let width_i = rt[right_i] - rt[left_i];
                    let width_j = rt[right_j] - rt[left_j];
                    let apex_distance = (rt[peaks[j].0] - rt[peaks[i].0]).abs();
                    let close = apex_distance < (width_i + width_j) * 0.15;
                    let start = peaks[i].0.min(peaks[j].0);
                    let end = peaks[i].0.max(peaks[j].0);
                    let valley = (start..=end).map(|k| sm[k]).fold(f32::INFINITY, f32::min)
                        < sm[peaks[i].0].min(sm[peaks[j].0]) * 0.70;
                    if overlaps && close && !valley {
                        let keep = if y[peaks[i].0] > y[peaks[j].0] { i } else { j };
                        let remove = if keep == i { j } else { i };
                        peaks[keep].1 = peaks[keep].1.min(peaks[remove].1);
                        peaks[keep].2 = peaks[keep].2.max(peaks[remove].2);
                        // Post-merge boundary re-shrink (core: recalc against
                        // baseline and 1% of apex on the raw intensity).
                        let kp = peaks[keep].0;
                        let p1 = y[kp] * 0.01;
                        let mut nl = peaks[keep].1;
                        for k in (0..=kp).rev() {
                            if k < nl {
                                break;
                            }
                            if y[k] <= b[k] || y[k] <= p1 {
                                nl = k + 1;
                                break;
                            }
                        }
                        let mut nr = peaks[keep].2;
                        for k in kp..y.len() {
                            if k > nr {
                                break;
                            }
                            if y[k] <= b[k] || y[k] <= p1 {
                                nr = k - 1;
                                break;
                            }
                        }
                        peaks[keep].1 = nl.max(0);
                        peaks[keep].2 = nr.min(y.len() - 1);
                        peaks.remove(remove);
                        merged = true;
                        break 'outer;
                    }
                }
            }
        }
        for (i, left, right) in peaks {
            let center = rt[i];
            let allowed_width = (rt[right] - rt[left]) * 0.5;
            let peak_max = (left..=right)
                .filter(|&j| (rt[j] - center).abs() <= allowed_width)
                .map(|j| y[j])
                .fold(0., f32::max);
            let peak_noise = if right - left + 1 >= 4 {
                (sm[left].min(sm[left + 1]) + sm[right - 1].min(sm[right])) / 2.
            } else {
                (left..=right).map(|j| sm[j]).fold(f32::INFINITY, f32::min)
            };
            if peak_noise > 0. && peak_max / peak_noise < min_snr {
                continue;
            }
            let f = make_feature(
                analysis,
                polarity,
                c,
                i,
                left,
                right,
                &rt,
                &mz,
                &y,
                &b,
                &sm,
                if polarity > 0 { "[M+H]+" } else { "[M-H]-" },
                if polarity > 0 { -1.007276 } else { 1.007276 },
            );
            if f.sn >= min_snr && f.r2 > -1. {
                out.push(f);
            }
        }
    }
    out
}

trait Unzip3<A, B, C> {
    fn unzip3(self) -> (Vec<A>, Vec<B>, Vec<C>);
}
impl<I, A, B, C> Unzip3<A, B, C> for I
where
    I: Iterator<Item = (A, B, C)>,
{
    fn unzip3(self) -> (Vec<A>, Vec<B>, Vec<C>) {
        let mut a = Vec::new();
        let mut b = Vec::new();
        let mut c = Vec::new();
        for (x, y, z) in self {
            a.push(x);
            b.push(y);
            c.push(z)
        }
        (a, b, c)
    }
}

fn row_sql(f: &Feature) -> String {
    let mut vals = vec![
        sql(&f.analysis),
        sql(&f.feature),
        "NULL".into(),
        "NULL".into(),
        sql(&f.adduct),
    ];
    let nums = [
        f.rt,
        f.mz,
        f.mass,
        f.intensity,
        f.noise,
        f.sn,
        f.area,
        STANDARD
            .decode(&f.eic[0].1)
            .map_or(0, |bytes| bytes.len() / 4) as f32,
        f.rtmin,
        f.rtmax,
        f.width,
        f.mzmin,
        f.mzmax,
        f.ppm,
        f.fwhm_rt,
        f.fwhm_mz,
        f.ga,
        f.gmu,
        f.gs,
        f.r2,
        f.jag,
        f.sharp,
        f.asym,
    ];
    vals.extend(nums.iter().map(|x| {
        if x.is_finite() {
            x.to_string()
        } else {
            "0".into()
        }
    }));
    vals.extend([f.modality.to_string(), f.plates.to_string()]);
    vals.extend([
        f.polarity.to_string(),
        "FALSE".into(),
        "NULL".into(),
        "FALSE".into(),
        "1.0".into(),
        STANDARD
            .decode(&f.eic[0].1)
            .map_or(0, |bytes| bytes.len() / 4)
            .to_string(),
    ]);
    for (_, v) in &f.eic {
        vals.push(sql(v));
    }
    vals.extend([
        "0".into(),
        "NULL".into(),
        "NULL".into(),
        "0".into(),
        "NULL".into(),
        "NULL".into(),
        "NULL".into(),
        "NULL".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "NULL".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "FALSE".into(),
        "FALSE".into(),
    ]);
    vals.join(",")
}

pub fn find_features(project: &mut Project, p: &Value) -> Result<Value> {
    let mins = p
        .get("rt_windows_min")
        .and_then(Value::as_array)
        .ok_or_else(|| invalid("rt_windows_min is required"))?;
    let maxs = p
        .get("rt_windows_max")
        .and_then(Value::as_array)
        .ok_or_else(|| invalid("rt_windows_max is required"))?;
    if mins.len() != maxs.len() {
        return Err(invalid(
            "rt_windows_min and rt_windows_max must have equal lengths.",
        ));
    }
    let ppm = p
        .get("ppm_threshold")
        .and_then(Value::as_f64)
        .unwrap_or(15.) as f32;
    let noise = p
        .get("noise_threshold")
        .and_then(Value::as_f64)
        .unwrap_or(15.) as f32;
    let min_snr = p.get("min_snr").and_then(Value::as_f64).unwrap_or(3.) as f32;
    let min_traces = p.get("min_traces").and_then(Value::as_u64).unwrap_or(3) as usize;
    let bw = p
        .get("baseline_window")
        .and_then(Value::as_f64)
        .unwrap_or(30.) as f32;
    let mw = p
        .get("max_feature_width")
        .and_then(Value::as_f64)
        .unwrap_or(30.) as f32;
    let base_quantile = p
        .get("base_quantile")
        .and_then(Value::as_f64)
        .unwrap_or(0.1) as f32;
    let schema="CREATE TABLE IF NOT EXISTS MASS_SPEC_NTA_FEATURES (analysis VARCHAR NOT NULL, feature VARCHAR NOT NULL, feature_component VARCHAR, feature_group VARCHAR, adduct VARCHAR, rt DOUBLE, mz DOUBLE, mass DOUBLE, intensity DOUBLE, noise DOUBLE, sn DOUBLE, area DOUBLE, trace_count INTEGER, rtmin DOUBLE, rtmax DOUBLE, width DOUBLE, mzmin DOUBLE, mzmax DOUBLE, ppm DOUBLE, fwhm_rt DOUBLE, fwhm_mz DOUBLE, gaussian_A DOUBLE, gaussian_mu DOUBLE, gaussian_sigma DOUBLE, gaussian_r2 DOUBLE, jaggedness DOUBLE, sharpness DOUBLE, asymmetry DOUBLE, modality INTEGER, plates DOUBLE, polarity INTEGER, filtered BOOLEAN, filter VARCHAR, filled BOOLEAN, correction DOUBLE, eic_size INTEGER, eic_rt VARCHAR, eic_mz VARCHAR, eic_intensity VARCHAR, eic_baseline VARCHAR, eic_smoothed VARCHAR, ms1_size INTEGER, ms1_mz VARCHAR, ms1_intensity VARCHAR, ms2_size INTEGER, ms2_mz VARCHAR, ms2_intensity VARCHAR, annotation_category VARCHAR, annotation_type VARCHAR, annotation_parent_feature VARCHAR, annotation_element VARCHAR, annotation_mass_error_da DOUBLE, annotation_mass_error_ppm DOUBLE, annotation_rt_error DOUBLE, annotation_rel_intensity DOUBLE, annotation_expected_rel_intensity_min DOUBLE, annotation_expected_rel_intensity_max DOUBLE, annotation_score DOUBLE, component_size INTEGER, component_rt_center DOUBLE, component_rt_spread DOUBLE, component_density DOUBLE, component_mean_correlation DOUBLE, component_best_partner VARCHAR, component_max_correlation DOUBLE, component_mean_correlation_to_component DOUBLE, component_membership_score DOUBLE, component_is_core BOOLEAN, component_bridge_flag BOOLEAN, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(analysis, feature))";
    project.execute_sql(schema)?;
    project.execute_sql(&format!("DELETE FROM MASS_SPEC_NTA_FEATURES"))?;
    let wanted = p
        .get("analysis_names")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let rows = project.query_json(&format!(
        "SELECT analysis,file_path,analysis_index FROM MASS_SPEC_ANALYSES ORDER BY analysis",
    ))?;
    for (analysis_index, row) in rows.as_array().into_iter().flatten().enumerate() {
        let name = row["analysis"].as_str().unwrap_or_default();
        if !wanted.is_empty() && !wanted.iter().any(|x| x.as_str() == Some(name)) {
            continue;
        }
        let mut file = Reader::open(row["file_path"].as_str().unwrap_or_default())
            .map_err(|e| invalid(e.to_string()))?;
        file.select_analysis(row["analysis_index"].as_i64().unwrap_or(0) as usize)
            .map_err(|e| invalid(e.to_string()))?;
        let load_started = Instant::now();
        file.load_mzml_spectrum_data()
            .map_err(|error| invalid(error.to_string()))?;
        eprintln!(
            "[find_features] {} mzML arrays loaded in {:.2}s",
            name,
            load_started.elapsed().as_secs_f64()
        );
        eprintln!(
            "[find_features] {}/{} denoising {} spectra",
            analysis_index + 1,
            rows.as_array().map_or(0, |values| values.len()),
            file.spectra().len()
        );
        let mut negative_points = Vec::new();
        let mut positive_points = Vec::new();
        let mut negative_spectra = 0usize;
        let mut positive_spectra = 0usize;
        for index in 0..file.spectra().len() {
            let s = file
                .spectrum_data(index)
                .map_err(|error| invalid(error.to_string()))?;
            if (index + 1) % 100 == 0 || index + 1 == file.spectra().len() {
                eprintln!(
                    "[find_features] {}/{} decoded {}/{} spectra",
                    analysis_index + 1,
                    rows.as_array().map_or(0, |values| values.len()),
                    index + 1,
                    file.spectra().len()
                );
            }
            if s.level != 1 {
                continue;
            }
            if !mins.is_empty()
                && !mins.iter().zip(maxs).any(|(lo, hi)| {
                    s.retention_time >= lo.as_f64().unwrap_or(f32::NEG_INFINITY as f64) as f32
                        && s.retention_time <= hi.as_f64().unwrap_or(f32::INFINITY as f64) as f32
                })
            {
                continue;
            }
            if s.mz.len() < min_traces {
                continue;
            }
            let n = noise_levels(&s.intensity, noise, base_quantile);
            let mut valid =
                s.mz.iter()
                    .zip(&s.intensity)
                    .zip(&n)
                    .filter(|((_, i), threshold)| **i > **threshold)
                    .map(|((mz, intensity), threshold)| (*mz, *intensity, *threshold))
                    .collect::<Vec<_>>();
            valid.sort_by(|a, b| a.0.total_cmp(&b.0));
            let mut merged: Vec<(f32, f32, f32)> = Vec::new();
            for point in valid {
                if let Some(last) = merged.last_mut() {
                    if point.0 - last.0 <= point.0 * ppm / 1e6 {
                        if point.1 > last.1 {
                            *last = point;
                        }
                        continue;
                    }
                }
                merged.push(point);
            }
            let points = match s.polarity {
                polarity if polarity < 0 => {
                    negative_spectra += 1;
                    &mut negative_points
                }
                polarity if polarity > 0 => {
                    positive_spectra += 1;
                    &mut positive_points
                }
                _ => continue,
            };
            points.extend(
                merged
                    .into_iter()
                    .map(|(mz, intensity, point_noise)| Point {
                        rt: s.retention_time,
                        mz,
                        intensity,
                        noise: point_noise,
                        cluster: 0,
                    }),
            );
        }
        eprintln!(
            "[find_features] {} denoising completed in {:.2}s",
            name,
            load_started.elapsed().as_secs_f64()
        );
        eprintln!(
            "[find_features] {} positive, {} negative spectra; {} positive, {} negative denoised points",
            positive_spectra,
            negative_spectra,
            positive_points.len(),
            negative_points.len()
        );
        for (polarity, label, mut points) in [
            (-1, "negative", negative_points),
            (1, "positive", positive_points),
        ] {
            if points.is_empty() {
                continue;
            }
            eprintln!(
                "[find_features] clustering/detecting {} {} points",
                label,
                points.len()
            );
            let features = detect(
                name,
                &mut points,
                min_traces,
                min_snr,
                bw,
                mw,
                polarity,
                ppm,
            );
            eprintln!("[find_features] {} {} features", label, features.len());
            let values = features
                .iter()
                .map(|f| format!("({}, NULL, NULL, CURRENT_TIMESTAMP)", row_sql(&f)))
                .collect::<Vec<_>>();
            if !values.is_empty() {
                project.execute_sql(&format!(
                    "INSERT INTO MASS_SPEC_NTA_FEATURES VALUES {}",
                    values.join(",")
                ))?;
            }
        }
        eprintln!("[find_features] analysis complete: {}", name);
    }
    Ok(json!({"status":"finished","info":"Features detected."}))
}

/// Port of `merge_NTA_FEATURE_SPECTRA` from bindings/r/src/core/nta/nta.cpp.
/// Each input point is `(mz, intensity, rt, pre_ce)`. Returns clustered
/// `(mz, intensity)` pairs in ascending m/z order.
fn merge_feature_spectra(
    points: &[(f32, f32, f32, Option<f32>)],
    mz_clust: f32,
    presence: f32,
) -> Vec<(f32, f32)> {
    let n = points.len();
    if n == 0 {
        return Vec::new();
    }
    let mut sorted: Vec<(f32, f32, f32, Option<f32>)> = points.to_vec();
    sorted.sort_by(|a, b| a.0.total_cmp(&b.0));

    let mut sorted_rt: Vec<f32> = sorted.iter().map(|p| p.2).collect();
    sorted_rt.sort_by(f32::total_cmp);
    sorted_rt.dedup();
    let total_unique_rt = sorted_rt.len();

    let mut all_finite_ce: Vec<f32> = sorted.iter().filter_map(|p| p.3).collect();
    all_finite_ce.sort_by(f32::total_cmp);
    all_finite_ce.dedup();
    let total_unique_pre_ce = all_finite_ce.len();

    let mz_tol = mz_clust.max(0.0);
    let presence_thresh = presence.clamp(0.0, 1.0);

    let mut out: Vec<(f32, f32)> = Vec::new();
    let mut start = 0;
    while start < n {
        let mut end = start + 1;
        while end < n && (sorted[end].0 - sorted[end - 1].0) <= mz_tol {
            end += 1;
        }

        // Group the m/z cluster by RT (same scan == same RT must not merge).
        let mut rt_groups: std::collections::BTreeMap<u32, Vec<usize>> =
            std::collections::BTreeMap::new();
        for i in start..end {
            rt_groups.entry(sorted[i].2.to_bits()).or_default().push(i);
        }

        if rt_groups.len() <= 1 {
            for i in start..end {
                out.push((sorted[i].0, sorted[i].1));
            }
            start = end;
            continue;
        }

        let mut reps_mz: Vec<f32> = Vec::new();
        let mut reps_int: Vec<f32> = Vec::new();
        let mut reps_pre_ce: Vec<f32> = Vec::new();
        for (_rt_bits, indices) in rt_groups {
            let mut best = indices[0];
            let mut best_int = sorted[best].1;
            for &ji in &indices {
                if sorted[ji].1 > best_int {
                    best = ji;
                    best_int = sorted[ji].1;
                }
            }
            reps_mz.push(sorted[best].0);
            reps_int.push(best_int);
            if let Some(ce) = sorted[best].3 {
                reps_pre_ce.push(ce);
            }
        }

        let mut pass = true;
        if presence_thresh > 0.0 && total_unique_rt > 0 {
            let mut required = presence_thresh * total_unique_rt as f32;
            if total_unique_pre_ce > 0 && !reps_pre_ce.is_empty() {
                reps_pre_ce.sort_by(f32::total_cmp);
                reps_pre_ce.dedup();
                let uniq_ce = reps_pre_ce.len();
                if (uniq_ce as f32) < total_unique_pre_ce as f32 {
                    required *= uniq_ce as f32 / total_unique_pre_ce as f32;
                }
            }
            if (reps_mz.len() as f32) < required {
                pass = false;
            }
        }
        if !pass {
            start = end;
            continue;
        }

        let mut best_rep = 0;
        let mut best_rep_int = reps_int[0];
        for i in 1..reps_mz.len() {
            if reps_int[i] > best_rep_int {
                best_rep = i;
                best_rep_int = reps_int[i];
            }
        }
        out.push((reps_mz[best_rep], reps_int[best_rep]));
        start = end;
    }
    out
}

/// Resolve and cluster MS1 spectra for each persisted feature and store the
/// joined MS1 spectrum (`ms1_size`, `ms1_mz`, `ms1_intensity`) on the row.
///
/// Parameters (see semantic/generated/catalogue.json `mass_spec.load_features_ms1`):
/// `analysis_names` (array, optional — empty means all), `filtered` (bool,
/// default false), `rt_window` (array[2], optional), `mz_window` (array[2],
/// optional), `min_traces_intensity` (real, default 250.0), `mz_clust` (real,
/// default 0.005), `presence` (real, default 0.8).
pub fn load_features_ms1(project: &mut Project, p: &Value) -> Result<Value> {
    let filtered = p.get("filtered").and_then(Value::as_bool).unwrap_or(false);
    let rt_window: Vec<f32> = p
        .get("rt_window")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_f64())
                .map(|x| x as f32)
                .collect()
        })
        .unwrap_or_default();
    let mz_window: Vec<f32> = p
        .get("mz_window")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_f64())
                .map(|x| x as f32)
                .collect()
        })
        .unwrap_or_default();
    let min_traces_intensity = p
        .get("min_traces_intensity")
        .and_then(Value::as_f64)
        .unwrap_or(250.0) as f32;
    let mz_clust = p.get("mz_clust").and_then(Value::as_f64).unwrap_or(0.005) as f32;
    let presence = p.get("presence").and_then(Value::as_f64).unwrap_or(0.8) as f32;
    let has_rt = rt_window.len() >= 2;
    let has_mz = mz_window.len() >= 2;
    let wanted = p
        .get("analysis_names")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let rows = project.query_json(&format!(
        "SELECT analysis, feature, rt, mz, rtmin, rtmax, mzmin, mzmax, polarity, filtered FROM MASS_SPEC_NTA_FEATURES ORDER BY analysis, feature"
    ))?;
    let mut updated = 0usize;
    let mut per_analysis: std::collections::BTreeMap<String, Vec<Value>> =
        std::collections::BTreeMap::new();
    for row in rows.as_array().into_iter().flatten() {
        let analysis = row["analysis"].as_str().unwrap_or_default().to_string();
        if !wanted.is_empty() && !wanted.iter().any(|x| x.as_str() == Some(&analysis)) {
            continue;
        }
        per_analysis.entry(analysis).or_default().push(row.clone());
    }
    for (analysis, frows) in per_analysis {
        let fs = project.query_json(&format!(
            "SELECT file_path, analysis_index FROM MASS_SPEC_ANALYSES WHERE analysis={}",
            sql(&analysis)
        ))?;
        let (file, analysis_index) = match fs.as_array().and_then(|a| a.first()) {
            Some(r) => (
                r["file_path"].as_str().unwrap_or_default().to_string(),
                r["analysis_index"].as_i64().unwrap_or(0) as usize,
            ),
            None => (String::new(), 0),
        };
        if file.is_empty() {
            continue;
        }
        let mut reader = Reader::open(&file).map_err(|e| invalid(e.to_string()))?;
        reader
            .select_analysis(analysis_index)
            .map_err(|e| invalid(e.to_string()))?;
        reader
            .load_mzml_spectrum_data()
            .map_err(|e| invalid(e.to_string()))?;
        let spectra = reader.spectra();
        eprintln!("[load_features_ms1] {} features={}", analysis, frows.len());
        let mut updates = Vec::new();
        for row in &frows {
            let feature = row["feature"].as_str().unwrap_or_default().to_string();
            let row_filtered = row["filtered"].as_bool().unwrap_or(false);
            if row_filtered && !filtered {
                continue;
            }
            if row["ms1_size"].as_i64().unwrap_or(0) > 0
                && !row["ms1_mz"].as_str().unwrap_or("").is_empty()
                && !row["ms1_intensity"].as_str().unwrap_or("").is_empty()
            {
                continue;
            }
            let ft_rtmin = row["rtmin"].as_f64().unwrap_or(0.0) as f32;
            let ft_rtmax = row["rtmax"].as_f64().unwrap_or(0.0) as f32;
            let ft_mzmin = row["mzmin"].as_f64().unwrap_or(0.0) as f32;
            let ft_mzmax = row["mzmax"].as_f64().unwrap_or(0.0) as f32;
            let ft_mz = row["mz"].as_f64().unwrap_or(0.0) as f32;
            let polarity = row["polarity"].as_i64().unwrap_or(0) as i32;

            let (rtmin, rtmax) = if has_rt {
                (ft_rtmin + rt_window[0], ft_rtmax + rt_window[1])
            } else {
                (ft_rtmin, ft_rtmax)
            };
            let (mzmin, mzmax) = if has_mz {
                (ft_mzmin + mz_window[0], ft_mzmax + mz_window[1])
            } else {
                (ft_mzmin, ft_mzmax)
            };
            let mmin = if mzmin == 0.0 && mzmax == 0.0 {
                ft_mz - 0.01
            } else {
                mzmin
            };
            let mmax = if mzmin == 0.0 && mzmax == 0.0 {
                ft_mz + 0.01
            } else {
                mzmax
            };

            let mut points: Vec<(f32, f32, f32, Option<f32>)> = Vec::new();
            for s in spectra {
                if s.level != 1 {
                    continue;
                }
                if polarity != 0 && s.polarity != polarity {
                    continue;
                }
                if rtmin != 0.0 && s.retention_time < rtmin {
                    continue;
                }
                if rtmax != 0.0 && s.retention_time > rtmax {
                    continue;
                }
                if s.mz.len() < 2 {
                    continue;
                }
                for k in 0..s.mz.len() {
                    let mzv = s.mz[k];
                    let inv = s.intensity[k];
                    if inv < min_traces_intensity {
                        continue;
                    }
                    if mzv < mmin || mzv > mmax {
                        continue;
                    }
                    points.push((mzv, inv, s.retention_time, None));
                }
            }
            let clustered = merge_feature_spectra(&points, mz_clust, presence);
            if clustered.is_empty() {
                continue;
            }
            let mzs: Vec<f32> = clustered.iter().map(|x| x.0).collect();
            let ints: Vec<f32> = clustered.iter().map(|x| x.1).collect();
            let n = mzs.len();
            updates.push(format!(
                "UPDATE MASS_SPEC_NTA_FEATURES SET ms1_size={}, ms1_mz={}, ms1_intensity={} WHERE analysis={} AND feature={}",
                n,
                sql(&encode(&mzs)),
                sql(&encode(&ints)),
                sql(&analysis),
                sql(&feature)
            ));
            updated += 1;
        }
        if !updates.is_empty() {
            project.execute_sql(&updates.join(";"))?;
        }
        eprintln!(
            "[load_features_ms1] {} updated {} features",
            analysis,
            updates.len()
        );
    }
    Ok(json!({"status": "finished", "info": format!("MS1 spectra loaded for {updated} features.")}))
}

/// Resolve and cluster MS2 spectra for each persisted feature and store the
/// joined MS2 spectrum (`ms2_size`, `ms2_mz`, `ms2_intensity`) on the row.
///
/// Parameters (see semantic/generated/catalogue.json `mass_spec.load_features_ms2`):
/// `analysis_names` (array, optional — empty means all), `filtered` (bool,
/// default false), `min_traces_intensity` (real, default 10.0),
/// `isolation_window` (real, default 1.3), `mz_clust` (real, default 0.005),
/// `presence` (real, default 0.8).
pub fn load_features_ms2(project: &mut Project, p: &Value) -> Result<Value> {
    let filtered = p.get("filtered").and_then(Value::as_bool).unwrap_or(false);
    let min_traces_intensity = p
        .get("min_traces_intensity")
        .and_then(Value::as_f64)
        .unwrap_or(10.0) as f32;
    let isolation_window = p
        .get("isolation_window")
        .and_then(Value::as_f64)
        .unwrap_or(1.3) as f32;
    let mz_clust = p.get("mz_clust").and_then(Value::as_f64).unwrap_or(0.005) as f32;
    let presence = p.get("presence").and_then(Value::as_f64).unwrap_or(0.8) as f32;
    let wanted = p
        .get("analysis_names")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let rows = project.query_json(&format!(
        "SELECT analysis, feature, rt, mz, rtmin, rtmax, polarity, filtered FROM MASS_SPEC_NTA_FEATURES ORDER BY analysis, feature",
    ))?;
    let mut updated = 0usize;
    let mut per_analysis: std::collections::BTreeMap<String, Vec<Value>> =
        std::collections::BTreeMap::new();
    for row in rows.as_array().into_iter().flatten() {
        let analysis = row["analysis"].as_str().unwrap_or_default().to_string();
        if !wanted.is_empty() && !wanted.iter().any(|x| x.as_str() == Some(&analysis)) {
            continue;
        }
        per_analysis.entry(analysis).or_default().push(row.clone());
    }
    for (analysis, frows) in per_analysis {
        let fs = project.query_json(&format!(
            "SELECT file_path, analysis_index FROM MASS_SPEC_ANALYSES WHERE analysis={}",
            sql(&analysis)
        ))?;
        let (file, analysis_index) = match fs.as_array().and_then(|a| a.first()) {
            Some(r) => (
                r["file_path"].as_str().unwrap_or_default().to_string(),
                r["analysis_index"].as_i64().unwrap_or(0) as usize,
            ),
            None => (String::new(), 0),
        };
        if file.is_empty() {
            continue;
        }
        let mut reader = Reader::open(&file).map_err(|e| invalid(e.to_string()))?;
        reader
            .select_analysis(analysis_index)
            .map_err(|e| invalid(e.to_string()))?;
        reader
            .load_mzml_spectrum_data()
            .map_err(|e| invalid(e.to_string()))?;
        let spectra = reader.spectra();
        let mut targets = Vec::new();
        for row in &frows {
            let row_filtered = row["filtered"].as_bool().unwrap_or(false);
            if row_filtered && !filtered {
                continue;
            }
            if row["ms2_size"].as_i64().unwrap_or(0) > 0
                && !row["ms2_mz"].as_str().unwrap_or("").is_empty()
                && !row["ms2_intensity"].as_str().unwrap_or("").is_empty()
            {
                continue;
            }
            let feature = row["feature"].as_str().unwrap_or_default().to_string();
            let ft_rtmin = row["rtmin"].as_f64().unwrap_or(0.0) as f32;
            let ft_rtmax = row["rtmax"].as_f64().unwrap_or(0.0) as f32;
            let ft_mz = row["mz"].as_f64().unwrap_or(0.0) as f32;
            let polarity = row["polarity"].as_i64().unwrap_or(0) as i32;
            targets.push(TargetSpec {
                id: feature,
                level: 2,
                polarity,
                precursor: true,
                mzmin: ft_mz - isolation_window / 2.0,
                mzmax: ft_mz + isolation_window / 2.0,
                rtmin: ft_rtmin,
                rtmax: ft_rtmax,
                mz: ft_mz,
                rt: row["rt"].as_f64().unwrap_or(0.0) as f32,
                mobility: 0.0,
            });
        }
        if targets.is_empty() {
            continue;
        }
        eprintln!(
            "[load_features_ms2] {} targets={} spectra={}",
            analysis,
            targets.len(),
            spectra.len()
        );
        let extracted = get_spectra_targets(spectra, &targets, 0.0, min_traces_intensity);
        let mut points_by_feature: std::collections::BTreeMap<
            String,
            Vec<(f32, f32, f32, Option<f32>)>,
        > = std::collections::BTreeMap::new();
        for point in extracted {
            let ce = point.pre_ce.is_finite().then_some(point.pre_ce);
            points_by_feature.entry(point.id).or_default().push((
                point.mz,
                point.intensity,
                point.rt,
                ce,
            ));
        }
        eprintln!(
            "[load_features_ms2] {} extracted {} spectrum points",
            analysis,
            points_by_feature.values().map(Vec::len).sum::<usize>()
        );
        let mut updates = Vec::new();
        for row in &frows {
            let feature = row["feature"].as_str().unwrap_or_default().to_string();
            let row_filtered = row["filtered"].as_bool().unwrap_or(false);
            if row_filtered && !filtered {
                continue;
            }
            if row["ms2_size"].as_i64().unwrap_or(0) > 0
                && !row["ms2_mz"].as_str().unwrap_or("").is_empty()
                && !row["ms2_intensity"].as_str().unwrap_or("").is_empty()
            {
                continue;
            }
            let Some(points) = points_by_feature.get(&feature) else {
                continue;
            };
            let clustered = merge_feature_spectra(points, mz_clust, presence);
            if clustered.is_empty() {
                continue;
            }
            let mzs: Vec<f32> = clustered.iter().map(|x| x.0).collect();
            let ints: Vec<f32> = clustered.iter().map(|x| x.1).collect();
            let n = mzs.len();
            updates.push(format!(
                "UPDATE MASS_SPEC_NTA_FEATURES SET ms2_size={}, ms2_mz={}, ms2_intensity={} WHERE analysis={} AND feature={}",
                n,
                sql(&encode(&mzs)),
                sql(&encode(&ints)),
                sql(&analysis),
                sql(&feature)
            ));
            updated += 1;
        }
        if !updates.is_empty() {
            project.execute_sql(&updates.join(";"))?;
        }
        eprintln!(
            "[load_features_ms2] {} updated {} features",
            analysis,
            updates.len()
        );
    }
    Ok(json!({"status": "finished", "info": format!("MS2 spectra loaded for {updated} features.")}))
}

// ---------------------------------------------------------------------------
// NTA processing context (ported from core/domains/mass_spec/src/
// nta_processing_methods.cpp `streamfind::mass_spec::processing_methods::detail`).
// Loads/persists the columnar feature, suspect, and internal-standard state so
// the processing algorithms run over the same in-memory model as core C++.
// ---------------------------------------------------------------------------

#[allow(dead_code)]
pub(crate) const NTA_FEATURES_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_NTA_FEATURES (analysis VARCHAR NOT NULL, feature VARCHAR NOT NULL, feature_component VARCHAR, feature_group VARCHAR, adduct VARCHAR, rt DOUBLE, mz DOUBLE, mass DOUBLE, intensity DOUBLE, noise DOUBLE, sn DOUBLE, area DOUBLE, trace_count INTEGER, rtmin DOUBLE, rtmax DOUBLE, width DOUBLE, mzmin DOUBLE, mzmax DOUBLE, ppm DOUBLE, fwhm_rt DOUBLE, fwhm_mz DOUBLE, gaussian_A DOUBLE, gaussian_mu DOUBLE, gaussian_sigma DOUBLE, gaussian_r2 DOUBLE, jaggedness DOUBLE, sharpness DOUBLE, asymmetry DOUBLE, modality INTEGER, plates DOUBLE, polarity INTEGER, filtered BOOLEAN, filter VARCHAR, filled BOOLEAN, correction DOUBLE, eic_size INTEGER, eic_rt VARCHAR, eic_mz VARCHAR, eic_intensity VARCHAR, eic_baseline VARCHAR, eic_smoothed VARCHAR, ms1_size INTEGER, ms1_mz VARCHAR, ms1_intensity VARCHAR, ms2_size INTEGER, ms2_mz VARCHAR, ms2_intensity VARCHAR, annotation_category VARCHAR, annotation_type VARCHAR, annotation_parent_feature VARCHAR, annotation_element VARCHAR, annotation_mass_error_da DOUBLE, annotation_mass_error_ppm DOUBLE, annotation_rt_error DOUBLE, annotation_rel_intensity DOUBLE, annotation_expected_rel_intensity_min DOUBLE, annotation_expected_rel_intensity_max DOUBLE, annotation_score DOUBLE, component_size INTEGER, component_rt_center DOUBLE, component_rt_spread DOUBLE, component_density DOUBLE, component_mean_correlation DOUBLE, component_best_partner VARCHAR, component_max_correlation DOUBLE, component_mean_correlation_to_component DOUBLE, component_membership_score DOUBLE, component_is_core BOOLEAN, component_bridge_flag BOOLEAN, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(analysis, feature))";

const NTA_SUSPECTS_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_NTA_SUSPECTS (analysis VARCHAR NOT NULL, feature VARCHAR NOT NULL, feature_group VARCHAR, candidate_rank INTEGER, name VARCHAR, polarity INTEGER, db_mass DOUBLE, exp_mass DOUBLE, error_mass DOUBLE, db_rt DOUBLE, exp_rt DOUBLE, error_rt DOUBLE, intensity DOUBLE, area DOUBLE, id_level INTEGER, score DOUBLE, shared_fragments INTEGER, cosine_similarity DOUBLE, formula VARCHAR, SMILES VARCHAR, InChI VARCHAR, InChIKey VARCHAR, xLogP DOUBLE, database_id VARCHAR, db_ms2_size INTEGER, db_ms2_mz VARCHAR, db_ms2_intensity VARCHAR, db_ms2_formula VARCHAR, db_ms2_smiles VARCHAR, exp_ms2_size INTEGER, exp_ms2_mz VARCHAR, exp_ms2_intensity VARCHAR, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(analysis, feature))";

const NTA_INTERNAL_STANDARDS_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_NTA_INTERNAL_STANDARDS (analysis VARCHAR NOT NULL, feature VARCHAR NOT NULL, feature_group VARCHAR, feature_component VARCHAR, adduct VARCHAR, candidate_rank INTEGER, name VARCHAR, polarity INTEGER, db_mass DOUBLE, exp_mass DOUBLE, error_mass DOUBLE, db_rt DOUBLE, exp_rt DOUBLE, error_rt DOUBLE, intensity DOUBLE, area DOUBLE, id_level INTEGER, score DOUBLE, shared_fragments INTEGER, cosine_similarity DOUBLE, formula VARCHAR, SMILES VARCHAR, InChI VARCHAR, InChIKey VARCHAR, xLogP DOUBLE, database_id VARCHAR, db_ms2_size INTEGER, db_ms2_mz VARCHAR, db_ms2_intensity VARCHAR, db_ms2_formula VARCHAR, db_ms2_smiles VARCHAR, exp_ms2_size INTEGER, exp_ms2_mz VARCHAR, exp_ms2_intensity VARCHAR, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(analysis, feature))";

const NTA_TRANSFORMATION_PRODUCTS_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS (analysis VARCHAR NOT NULL, feature_group VARCHAR, precursor_feature_group VARCHAR, main_precursor_feature_group VARCHAR, assignment_rank INTEGER, name VARCHAR NOT NULL, formula VARCHAR, mass DOUBLE, SMILES VARCHAR, InChI VARCHAR, InChIKey VARCHAR, xLogP DOUBLE, transformation VARCHAR, precursor_name VARCHAR, precursor_formula VARCHAR, precursor_mass DOUBLE, precursor_SMILES VARCHAR, precursor_InChI VARCHAR, precursor_InChIKey VARCHAR, precursor_xLogP DOUBLE, main_precursor_name VARCHAR, main_precursor_formula VARCHAR, main_precursor_mass DOUBLE, main_precursor_SMILES VARCHAR, main_precursor_InChI VARCHAR, main_precursor_InChIKey VARCHAR, main_precursor_xLogP DOUBLE, cosine_similarity DOUBLE, main_precursor_cosine_similarity DOUBLE, rt_plausibility DOUBLE, main_precursor_rt_plausibility DOUBLE, assignment_score DOUBLE, network_level INTEGER, assignment_status VARCHAR, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(analysis, feature_group, name))";

#[allow(dead_code)]
pub(crate) fn ensure_nta_schemas(project: &Project) -> Result<()> {
    project.execute_sql(NTA_FEATURES_SCHEMA)?;
    project.execute_sql(NTA_SUSPECTS_SCHEMA)?;
    project.execute_sql(NTA_INTERNAL_STANDARDS_SCHEMA)?;
    project.execute_sql(NTA_TRANSFORMATION_PRODUCTS_SCHEMA)?;
    Ok(())
}

fn row_text(row: &Value, col: &str) -> String {
    row.get(col)
        .filter(|v| !v.is_null())
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string()
}

fn row_num(row: &Value, col: &str) -> f64 {
    row.get(col).and_then(Value::as_f64).unwrap_or(0.0)
}

fn row_int(row: &Value, col: &str) -> i32 {
    row.get(col).and_then(Value::as_i64).unwrap_or(0) as i32
}

fn row_bool(row: &Value, col: &str) -> bool {
    match row.get(col) {
        Some(Value::Bool(b)) => *b,
        Some(Value::String(s)) => s == "true" || s == "TRUE" || s == "1",
        Some(Value::Number(n)) => n.as_i64() == Some(1),
        _ => false,
    }
}

/// Load every persisted feature for the selected analyses into a columnar
/// `ProjectNonTargetAnalysis` (mirrors C++ `detail::load_analysis_features`).
pub(crate) fn load_analysis_features(
    project: &Project,
    parameters: &Value,
) -> Result<crate::nta::ProjectNonTargetAnalysis> {
    let wanted = parameters
        .get("analysis_names")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let mut names = Vec::new();
    let mut paths = Vec::new();
    let mut indices = Vec::new();
    let mut blanks = Vec::new();
    let mut replicates = Vec::new();
    let rows = project.query_json(&format!(
        "SELECT analysis,file_path,analysis_index,blank,replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis",
    ))?;
    for row in rows.as_array().into_iter().flatten() {
        let name = row_text(row, "analysis");
        if !wanted.is_empty() && !wanted.iter().any(|x| x.as_str() == Some(name.as_str())) {
            continue;
        }
        names.push(name);
        paths.push(row_text(row, "file_path"));
        indices.push(
            row.get("analysis_index")
                .and_then(Value::as_i64)
                .unwrap_or(0) as usize,
        );
        blanks.push(row_text(row, "blank"));
        replicates.push(row_text(row, "replicate"));
    }
    let mut data = crate::nta::ProjectNonTargetAnalysis::new(names.clone(), paths, indices);
    data.set_blank_names(blanks);
    data.set_replicate_names(replicates);
    for (i, buffer) in data.feature_buffers.iter_mut().enumerate() {
        buffer.analysis = names[i].clone();
    }
    let features = project.query_json(&format!(
        "SELECT analysis, feature, feature_component, feature_group, adduct, rt, mz, mass, intensity, noise, sn, area, rtmin, rtmax, width, mzmin, mzmax, ppm, fwhm_rt, fwhm_mz, gaussian_A, gaussian_mu, gaussian_sigma, gaussian_r2, jaggedness, sharpness, asymmetry, modality, plates, polarity, filtered, filter, filled, correction, eic_size, eic_rt, eic_mz, eic_intensity, eic_baseline, eic_smoothed, ms1_size, ms1_mz, ms1_intensity, ms2_size, ms2_mz, ms2_intensity, annotation_category, annotation_type, annotation_parent_feature, annotation_element, annotation_mass_error_da, annotation_mass_error_ppm, annotation_rt_error, annotation_rel_intensity, annotation_expected_rel_intensity_min, annotation_expected_rel_intensity_max, annotation_score, component_size, component_rt_center, component_rt_spread, component_density, component_mean_correlation, component_best_partner, component_max_correlation, component_mean_correlation_to_component, component_membership_score, component_is_core, component_bridge_flag FROM MASS_SPEC_NTA_FEATURES ORDER BY analysis",
    ))?;
    for row in features.as_array().into_iter().flatten() {
        let an = row_text(row, "analysis");
        let Some(pos) = names.iter().position(|n| *n == an) else {
            continue;
        };
        let mut r = crate::nta::NtaFeatureRow::default();
        r.analysis = an;
        r.feature = row_text(row, "feature");
        r.feature_component = row_text(row, "feature_component");
        r.feature_group = row_text(row, "feature_group");
        r.adduct = row_text(row, "adduct");
        r.rt = row_num(row, "rt");
        r.mz = row_num(row, "mz");
        r.mass = row_num(row, "mass");
        r.intensity = row_num(row, "intensity");
        r.noise = row_num(row, "noise");
        r.sn = row_num(row, "sn");
        r.area = row_num(row, "area");
        r.rtmin = row_num(row, "rtmin");
        r.rtmax = row_num(row, "rtmax");
        r.width = row_num(row, "width");
        r.mzmin = row_num(row, "mzmin");
        r.mzmax = row_num(row, "mzmax");
        r.ppm = row_num(row, "ppm");
        r.fwhm_rt = row_num(row, "fwhm_rt");
        r.fwhm_mz = row_num(row, "fwhm_mz");
        r.gaussian_A = row_num(row, "gaussian_A");
        r.gaussian_mu = row_num(row, "gaussian_mu");
        r.gaussian_sigma = row_num(row, "gaussian_sigma");
        r.gaussian_r2 = row_num(row, "gaussian_r2");
        r.jaggedness = row_num(row, "jaggedness");
        r.sharpness = row_num(row, "sharpness");
        r.asymmetry = row_num(row, "asymmetry");
        r.modality = row_int(row, "modality");
        r.plates = row_num(row, "plates");
        r.polarity = row_int(row, "polarity");
        r.filtered = row_bool(row, "filtered");
        r.filter = row_text(row, "filter");
        r.filled = row_bool(row, "filled");
        r.correction = row_num(row, "correction");
        r.eic_size = row_int(row, "eic_size");
        r.eic_rt = row_text(row, "eic_rt");
        r.eic_mz = row_text(row, "eic_mz");
        r.eic_intensity = row_text(row, "eic_intensity");
        r.eic_baseline = row_text(row, "eic_baseline");
        r.eic_smoothed = row_text(row, "eic_smoothed");
        r.ms1_size = row_int(row, "ms1_size");
        r.ms1_mz = row_text(row, "ms1_mz");
        r.ms1_intensity = row_text(row, "ms1_intensity");
        r.ms2_size = row_int(row, "ms2_size");
        r.ms2_mz = row_text(row, "ms2_mz");
        r.ms2_intensity = row_text(row, "ms2_intensity");
        r.annotation_category = row_text(row, "annotation_category");
        r.annotation_type = row_text(row, "annotation_type");
        r.annotation_parent_feature = row_text(row, "annotation_parent_feature");
        r.annotation_element = row_text(row, "annotation_element");
        r.annotation_mass_error_da = row_num(row, "annotation_mass_error_da");
        r.annotation_mass_error_ppm = row_num(row, "annotation_mass_error_ppm");
        r.annotation_rt_error = row_num(row, "annotation_rt_error");
        r.annotation_rel_intensity = row_num(row, "annotation_rel_intensity");
        r.annotation_expected_rel_intensity_min =
            row_num(row, "annotation_expected_rel_intensity_min");
        r.annotation_expected_rel_intensity_max =
            row_num(row, "annotation_expected_rel_intensity_max");
        r.annotation_score = row_num(row, "annotation_score");
        r.component_size = row_int(row, "component_size");
        r.component_rt_center = row_num(row, "component_rt_center");
        r.component_rt_spread = row_num(row, "component_rt_spread");
        r.component_density = row_num(row, "component_density");
        r.component_mean_correlation = row_num(row, "component_mean_correlation");
        r.component_best_partner = row_text(row, "component_best_partner");
        r.component_max_correlation = row_num(row, "component_max_correlation");
        r.component_mean_correlation_to_component =
            row_num(row, "component_mean_correlation_to_component");
        r.component_membership_score = row_num(row, "component_membership_score");
        r.component_is_core = row_bool(row, "component_is_core");
        r.component_bridge_flag = row_bool(row, "component_bridge_flag");
        data.feature_buffers[pos].append_feature(&r);
    }
    Ok(data)
}
fn num_cell(v: f64) -> String {
    if v.is_finite() {
        v.to_string()
    } else if v.is_nan() {
        "nan".to_string()
    } else if v > 0.0 {
        "inf".to_string()
    } else {
        "-inf".to_string()
    }
}

fn dnum_cell(v: f64) -> String {
    if v.is_finite() {
        v.to_string()
    } else {
        "NULL".to_string()
    }
}

/// Build one `MASS_SPEC_NTA_FEATURES` value tuple (column order mirrors the
/// C++ `features_columns()`; `created_at` is left to the DEFAULT).
fn feature_values(r: &crate::nta::NtaFeatureRow) -> String {
    let vals = vec![
        sql(&r.analysis),
        sql(&r.feature),
        sql(&r.feature_component),
        sql(&r.feature_group),
        sql(&r.adduct),
        num_cell(r.rt),
        num_cell(r.mz),
        num_cell(r.mass),
        num_cell(r.intensity),
        num_cell(r.noise),
        num_cell(r.sn),
        num_cell(r.area),
        r.eic_size.to_string(),
        num_cell(r.rtmin),
        num_cell(r.rtmax),
        num_cell(r.width),
        num_cell(r.mzmin),
        num_cell(r.mzmax),
        num_cell(r.ppm),
        num_cell(r.fwhm_rt),
        num_cell(r.fwhm_mz),
        num_cell(r.gaussian_A),
        num_cell(r.gaussian_mu),
        num_cell(r.gaussian_sigma),
        num_cell(r.gaussian_r2),
        num_cell(r.jaggedness),
        num_cell(r.sharpness),
        num_cell(r.asymmetry),
        r.modality.to_string(),
        num_cell(r.plates),
        r.polarity.to_string(),
        if r.filtered { "TRUE" } else { "FALSE" }.to_string(),
        sql(&r.filter),
        if r.filled { "TRUE" } else { "FALSE" }.to_string(),
        num_cell(r.correction),
        r.eic_size.to_string(),
        sql(&r.eic_rt),
        sql(&r.eic_mz),
        sql(&r.eic_intensity),
        sql(&r.eic_baseline),
        sql(&r.eic_smoothed),
        r.ms1_size.to_string(),
        sql(&r.ms1_mz),
        sql(&r.ms1_intensity),
        r.ms2_size.to_string(),
        sql(&r.ms2_mz),
        sql(&r.ms2_intensity),
        sql(&r.annotation_category),
        sql(&r.annotation_type),
        sql(&r.annotation_parent_feature),
        sql(&r.annotation_element),
        num_cell(r.annotation_mass_error_da),
        num_cell(r.annotation_mass_error_ppm),
        num_cell(r.annotation_rt_error),
        num_cell(r.annotation_rel_intensity),
        num_cell(r.annotation_expected_rel_intensity_min),
        num_cell(r.annotation_expected_rel_intensity_max),
        num_cell(r.annotation_score),
        r.component_size.to_string(),
        num_cell(r.component_rt_center),
        num_cell(r.component_rt_spread),
        num_cell(r.component_density),
        num_cell(r.component_mean_correlation),
        sql(&r.component_best_partner),
        num_cell(r.component_max_correlation),
        num_cell(r.component_mean_correlation_to_component),
        num_cell(r.component_membership_score),
        if r.component_is_core { "TRUE" } else { "FALSE" }.to_string(),
        if r.component_bridge_flag {
            "TRUE"
        } else {
            "FALSE"
        }
        .to_string(),
    ];
    format!("({})", vals.join(","))
}

const FEATURES_COLUMNS: &str = "analysis,feature,feature_component,feature_group,adduct,rt,mz,mass,intensity,noise,sn,area,trace_count,rtmin,rtmax,width,mzmin,mzmax,ppm,fwhm_rt,fwhm_mz,gaussian_A,gaussian_mu,gaussian_sigma,gaussian_r2,jaggedness,sharpness,asymmetry,modality,plates,polarity,filtered,filter,filled,correction,eic_size,eic_rt,eic_mz,eic_intensity,eic_baseline,eic_smoothed,ms1_size,ms1_mz,ms1_intensity,ms2_size,ms2_mz,ms2_intensity,annotation_category,annotation_type,annotation_parent_feature,annotation_element,annotation_mass_error_da,annotation_mass_error_ppm,annotation_rt_error,annotation_rel_intensity,annotation_expected_rel_intensity_min,annotation_expected_rel_intensity_max,annotation_score,component_size,component_rt_center,component_rt_spread,component_density,component_mean_correlation,component_best_partner,component_max_correlation,component_mean_correlation_to_component,component_membership_score,component_is_core,component_bridge_flag";

/// Batched multi-row INSERT (DuckDB accepts many VALUES tuples in one
/// statement); chunked to keep statement sizes bounded.
fn insert_rows(
    project: &Project,
    table: &str,
    columns: &str,
    value_tuples: Vec<String>,
) -> Result<()> {
    for chunk in value_tuples.chunks(500) {
        let sql_text = format!("INSERT INTO {table} ({columns}) VALUES {}", chunk.join(","));
        project.execute_sql(&sql_text)?;
    }
    Ok(())
}

/// Delete + full re-insert of every feature row (mirrors C++
/// `detail::persist_features`).
pub(crate) fn persist_features(
    project: &Project,
    data: &crate::nta::ProjectNonTargetAnalysis,
) -> Result<()> {
    project.execute_sql(&format!("DELETE FROM MASS_SPEC_NTA_FEATURES"))?;
    let mut tuples = Vec::new();
    for buffer in &data.feature_buffers {
        for i in 0..buffer.size() {
            tuples.push(feature_values(&&buffer.get_feature(i)));
        }
    }
    insert_rows(project, "MASS_SPEC_NTA_FEATURES", FEATURES_COLUMNS, tuples)
}

fn suspect_values(r: &crate::nta::NtaSuspectRow) -> String {
    let vals = vec![
        sql(&r.analysis),
        sql(&r.feature),
        sql(&r.feature_group),
        r.candidate_rank.to_string(),
        sql(&r.name),
        r.polarity.to_string(),
        dnum_cell(r.db_mass),
        dnum_cell(r.exp_mass),
        dnum_cell(r.error_mass),
        dnum_cell(r.db_rt),
        dnum_cell(r.exp_rt),
        dnum_cell(r.error_rt),
        dnum_cell(r.intensity),
        dnum_cell(r.area),
        r.id_level.to_string(),
        dnum_cell(r.score),
        r.shared_fragments.to_string(),
        dnum_cell(r.cosine_similarity),
        sql(&r.formula),
        sql(&r.SMILES),
        sql(&r.InChI),
        sql(&r.InChIKey),
        dnum_cell(r.xLogP),
        sql(&r.database_id),
        r.db_ms2_size.to_string(),
        sql(&r.db_ms2_mz),
        sql(&r.db_ms2_intensity),
        sql(&r.db_ms2_formula),
        sql(&r.db_ms2_smiles),
        r.exp_ms2_size.to_string(),
        sql(&r.exp_ms2_mz),
        sql(&r.exp_ms2_intensity),
    ];
    format!("({})", vals.join(","))
}

const SUSPECTS_COLUMNS: &str = "analysis,feature,feature_group,candidate_rank,name,polarity,db_mass,exp_mass,error_mass,db_rt,exp_rt,error_rt,intensity,area,id_level,score,shared_fragments,cosine_similarity,formula,SMILES,InChI,InChIKey,xLogP,database_id,db_ms2_size,db_ms2_mz,db_ms2_intensity,db_ms2_formula,db_ms2_smiles,exp_ms2_size,exp_ms2_mz,exp_ms2_intensity";

pub(crate) fn persist_suspects(
    project: &Project,
    data: &crate::nta::ProjectNonTargetAnalysis,
) -> Result<()> {
    project.execute_sql(NTA_SUSPECTS_SCHEMA)?;
    project.execute_sql(&format!("DELETE FROM MASS_SPEC_NTA_SUSPECTS"))?;
    let mut tuples = Vec::new();
    for buffer in &data.suspect_buffers {
        for i in 0..buffer.size() {
            tuples.push(suspect_values(&&buffer.get_suspect(i)));
        }
    }
    insert_rows(project, "MASS_SPEC_NTA_SUSPECTS", SUSPECTS_COLUMNS, tuples)
}

fn internal_standard_values(r: &crate::nta::NtaInternalStandardRow) -> String {
    let vals = vec![
        sql(&r.analysis),
        sql(&r.feature),
        sql(&r.feature_group),
        sql(&r.feature_component),
        sql(&r.adduct),
        r.candidate_rank.to_string(),
        sql(&r.name),
        r.polarity.to_string(),
        dnum_cell(r.db_mass),
        dnum_cell(r.exp_mass),
        dnum_cell(r.error_mass),
        dnum_cell(r.db_rt),
        dnum_cell(r.exp_rt),
        dnum_cell(r.error_rt),
        dnum_cell(r.intensity),
        dnum_cell(r.area),
        r.id_level.to_string(),
        dnum_cell(r.score),
        r.shared_fragments.to_string(),
        dnum_cell(r.cosine_similarity),
        sql(&r.formula),
        sql(&r.SMILES),
        sql(&r.InChI),
        sql(&r.InChIKey),
        dnum_cell(r.xLogP),
        sql(&r.database_id),
        r.db_ms2_size.to_string(),
        sql(&r.db_ms2_mz),
        sql(&r.db_ms2_intensity),
        sql(&r.db_ms2_formula),
        sql(&r.db_ms2_smiles),
        r.exp_ms2_size.to_string(),
        sql(&r.exp_ms2_mz),
        sql(&r.exp_ms2_intensity),
    ];
    format!("({})", vals.join(","))
}

const INTERNAL_STANDARDS_COLUMNS: &str = "analysis,feature,feature_group,feature_component,adduct,candidate_rank,name,polarity,db_mass,exp_mass,error_mass,db_rt,exp_rt,error_rt,intensity,area,id_level,score,shared_fragments,cosine_similarity,formula,SMILES,InChI,InChIKey,xLogP,database_id,db_ms2_size,db_ms2_mz,db_ms2_intensity,db_ms2_formula,db_ms2_smiles,exp_ms2_size,exp_ms2_mz,exp_ms2_intensity";

pub(crate) fn persist_internal_standards(
    project: &Project,
    data: &crate::nta::ProjectNonTargetAnalysis,
) -> Result<()> {
    project.execute_sql(NTA_INTERNAL_STANDARDS_SCHEMA)?;
    project.execute_sql(&format!("DELETE FROM MASS_SPEC_NTA_INTERNAL_STANDARDS"))?;
    let mut tuples = Vec::new();
    for buffer in &data.internal_standard_buffers {
        for i in 0..buffer.size() {
            tuples.push(internal_standard_values(&&buffer.get_internal_standard(i)));
        }
    }
    insert_rows(
        project,
        "MASS_SPEC_NTA_INTERNAL_STANDARDS",
        INTERNAL_STANDARDS_COLUMNS,
        tuples,
    )
}
const TRANSFORMATION_PRODUCTS_COLUMNS: &str = "analysis,feature_group,precursor_feature_group,main_precursor_feature_group,assignment_rank,name,formula,mass,SMILES,InChI,InChIKey,xLogP,transformation,precursor_name,precursor_formula,precursor_mass,precursor_SMILES,precursor_InChI,precursor_InChIKey,precursor_xLogP,main_precursor_name,main_precursor_formula,main_precursor_mass,main_precursor_SMILES,main_precursor_InChI,main_precursor_InChIKey,main_precursor_xLogP,cosine_similarity,main_precursor_cosine_similarity,rt_plausibility,main_precursor_rt_plausibility,assignment_score,network_level,assignment_status";

fn transformation_product_values(
    analysis: &str,
    r: &crate::nta_transformation_products::TransformationProductRow,
) -> String {
    let vals = vec![
        sql(analysis),
        sql(&r.feature_group),
        sql(&r.precursor_feature_group),
        sql(&r.main_precursor_feature_group),
        r.assignment_rank.to_string(),
        sql(&r.name),
        sql(&r.formula),
        dnum_cell(r.mass),
        sql(&r.SMILES),
        sql(&r.InChI),
        sql(&r.InChIKey),
        dnum_cell(r.xLogP),
        sql(&r.transformation),
        sql(&r.precursor_name),
        sql(&r.precursor_formula),
        dnum_cell(r.precursor_mass),
        sql(&r.precursor_SMILES),
        sql(&r.precursor_InChI),
        sql(&r.precursor_InChIKey),
        dnum_cell(r.precursor_xLogP),
        sql(&r.main_precursor_name),
        sql(&r.main_precursor_formula),
        dnum_cell(r.main_precursor_mass),
        sql(&r.main_precursor_SMILES),
        sql(&r.main_precursor_InChI),
        sql(&r.main_precursor_InChIKey),
        dnum_cell(r.main_precursor_xLogP),
        dnum_cell(r.cosine_similarity),
        dnum_cell(r.main_precursor_cosine_similarity),
        dnum_cell(r.rt_plausibility),
        dnum_cell(r.main_precursor_rt_plausibility),
        dnum_cell(r.assignment_score),
        r.network_level.to_string(),
        sql(&r.assignment_status),
    ];
    format!("({})", vals.join(","))
}

/// Delete + full re-insert of the transformation-product assignment rows
/// (mirrors `persist_suspects`); `analysis` is resolved per row by the caller.
pub(crate) fn persist_transformation_products(
    project: &Project,
    rows: &[(
        String,
        crate::nta_transformation_products::TransformationProductRow,
    )],
) -> Result<()> {
    project.execute_sql(NTA_TRANSFORMATION_PRODUCTS_SCHEMA)?;
    project.execute_sql(&format!(
        "DELETE FROM MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS"
    ))?;
    let tuples: Vec<String> = rows
        .iter()
        .map(|(analysis, row)| transformation_product_values(&analysis, row))
        .collect();
    insert_rows(
        project,
        "MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS",
        TRANSFORMATION_PRODUCTS_COLUMNS,
        tuples,
    )
}

/// NULL-tolerant numeric load for suspect/IS tables: SQL NULL (persisted
/// NaN) maps back to NaN, matching C++ `detail::col_d`.
fn col_num(row: &Value, col: &str) -> f64 {
    row.get(col).and_then(Value::as_f64).unwrap_or(f64::NAN)
}

pub(crate) fn load_suspects(
    project: &Project,
    data: &mut crate::nta::ProjectNonTargetAnalysis,
) -> Result<()> {
    project.execute_sql(NTA_SUSPECTS_SCHEMA)?;
    for buffer in data.suspect_buffers.iter_mut() {
        *buffer = crate::nta::NtaSuspects::default();
    }
    let names = data.analysis_names().to_vec();
    let rows = project.query_json(&format!(
        "SELECT * FROM MASS_SPEC_NTA_SUSPECTS ORDER BY analysis",
    ))?;
    for row in rows.as_array().into_iter().flatten() {
        let an = row_text(row, "analysis");
        let Some(pos) = names.iter().position(|n| *n == an) else {
            continue;
        };
        let mut r = crate::nta::NtaSuspectRow::default();
        r.analysis = an;
        r.feature = row_text(row, "feature");
        r.feature_group = row_text(row, "feature_group");
        r.candidate_rank = row_int(row, "candidate_rank");
        r.name = row_text(row, "name");
        r.polarity = row_int(row, "polarity");
        r.db_mass = col_num(row, "db_mass");
        r.exp_mass = col_num(row, "exp_mass");
        r.error_mass = col_num(row, "error_mass");
        r.db_rt = col_num(row, "db_rt");
        r.exp_rt = col_num(row, "exp_rt");
        r.error_rt = col_num(row, "error_rt");
        r.intensity = col_num(row, "intensity");
        r.area = col_num(row, "area");
        r.id_level = row_int(row, "id_level");
        r.score = col_num(row, "score");
        r.shared_fragments = row_int(row, "shared_fragments");
        r.cosine_similarity = col_num(row, "cosine_similarity");
        r.formula = row_text(row, "formula");
        r.SMILES = row_text(row, "SMILES");
        r.InChI = row_text(row, "InChI");
        r.InChIKey = row_text(row, "InChIKey");
        r.xLogP = col_num(row, "xLogP");
        r.database_id = row_text(row, "database_id");
        r.db_ms2_size = row_int(row, "db_ms2_size");
        r.db_ms2_mz = row_text(row, "db_ms2_mz");
        r.db_ms2_intensity = row_text(row, "db_ms2_intensity");
        r.db_ms2_formula = row_text(row, "db_ms2_formula");
        r.db_ms2_smiles = row_text(row, "db_ms2_smiles");
        r.exp_ms2_size = row_int(row, "exp_ms2_size");
        r.exp_ms2_mz = row_text(row, "exp_ms2_mz");
        r.exp_ms2_intensity = row_text(row, "exp_ms2_intensity");
        data.suspect_buffers[pos].append(&r);
    }
    Ok(())
}

pub(crate) fn load_internal_standards(
    project: &Project,
    data: &mut crate::nta::ProjectNonTargetAnalysis,
) -> Result<()> {
    project.execute_sql(NTA_INTERNAL_STANDARDS_SCHEMA)?;
    for buffer in data.internal_standard_buffers.iter_mut() {
        *buffer = crate::nta::NtaInternalStandards::default();
    }
    let names = data.analysis_names().to_vec();
    let rows = project.query_json(&format!(
        "SELECT * FROM MASS_SPEC_NTA_INTERNAL_STANDARDS ORDER BY analysis",
    ))?;
    for row in rows.as_array().into_iter().flatten() {
        let an = row_text(row, "analysis");
        let Some(pos) = names.iter().position(|n| *n == an) else {
            continue;
        };
        let mut r = crate::nta::NtaInternalStandardRow::default();
        r.analysis = an;
        r.feature = row_text(row, "feature");
        r.feature_group = row_text(row, "feature_group");
        r.feature_component = row_text(row, "feature_component");
        r.adduct = row_text(row, "adduct");
        r.candidate_rank = row_int(row, "candidate_rank");
        r.name = row_text(row, "name");
        r.polarity = row_int(row, "polarity");
        r.db_mass = col_num(row, "db_mass");
        r.exp_mass = col_num(row, "exp_mass");
        r.error_mass = col_num(row, "error_mass");
        r.db_rt = col_num(row, "db_rt");
        r.exp_rt = col_num(row, "exp_rt");
        r.error_rt = col_num(row, "error_rt");
        r.intensity = col_num(row, "intensity");
        r.area = col_num(row, "area");
        r.id_level = row_int(row, "id_level");
        r.score = col_num(row, "score");
        r.shared_fragments = row_int(row, "shared_fragments");
        r.cosine_similarity = col_num(row, "cosine_similarity");
        r.formula = row_text(row, "formula");
        r.SMILES = row_text(row, "SMILES");
        r.InChI = row_text(row, "InChI");
        r.InChIKey = row_text(row, "InChIKey");
        r.xLogP = col_num(row, "xLogP");
        r.database_id = row_text(row, "database_id");
        r.db_ms2_size = row_int(row, "db_ms2_size");
        r.db_ms2_mz = row_text(row, "db_ms2_mz");
        r.db_ms2_intensity = row_text(row, "db_ms2_intensity");
        r.db_ms2_formula = row_text(row, "db_ms2_formula");
        r.db_ms2_smiles = row_text(row, "db_ms2_smiles");
        r.exp_ms2_size = row_int(row, "exp_ms2_size");
        r.exp_ms2_mz = row_text(row, "exp_ms2_mz");
        r.exp_ms2_intensity = row_text(row, "exp_ms2_intensity");
        data.internal_standard_buffers[pos].append(&r);
    }
    Ok(())
}

/// Map the JSON `targets` array (suspects/internal standards) into
/// `SuspectQuery` objects (mirrors C++ `detail::parse_suspect_targets`).
pub(crate) fn parse_suspect_targets(
    parameters: &Value,
) -> Vec<crate::nta_suspect_screening::SuspectQuery> {
    let mut out = Vec::new();
    let targets = parameters
        .get("targets")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    for t in &targets {
        let mut q = crate::nta_suspect_screening::SuspectQuery::default();
        q.name = t
            .get("id")
            .or_else(|| t.get("name"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        if let Some(m) = t.get("mass").and_then(Value::as_f64) {
            q.has_mass = true;
            q.mass = m;
        } else if let Some(m) = t.get("mz").and_then(Value::as_f64) {
            q.has_mass = true;
            q.mass = m;
        }
        q.rt = t.get("rt").and_then(Value::as_f64).unwrap_or(0.0);
        q.formula = row_text(t, "formula");
        q.SMILES = t
            .get("SMILES")
            .or_else(|| t.get("smiles"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        q.InChI = t
            .get("InChI")
            .or_else(|| t.get("inchi"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        q.InChIKey = t
            .get("InChIKey")
            .or_else(|| t.get("inchikey"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        q.database_id = row_text(t, "database_id");
        q.score = t.get("score").and_then(Value::as_f64).unwrap_or(0.0);
        if let Some(x) = t.get("xLogP").and_then(Value::as_f64) {
            q.has_xLogP = true;
            q.xLogP = x;
        }
        let frag_mz = t
            .get("fragments_mz")
            .or_else(|| t.get("fragments_mz_pos"))
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        let frag_int = t
            .get("fragments_intensity")
            .or_else(|| t.get("fragments_intensity_pos"))
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        for v in &frag_mz {
            q.fragments_mz_pos.push(v.as_f64().unwrap_or(0.0));
        }
        for v in &frag_int {
            q.fragments_intensity_pos.push(v.as_f64().unwrap_or(0.0));
        }
        out.push(q);
    }
    out
}
fn opt_real(p: &Value, key: &str) -> f64 {
    match p.get(key) {
        Some(v) if !v.is_null() => v.as_f64().unwrap_or(f64::NAN),
        _ => f64::NAN,
    }
}

fn opt_int(p: &Value, key: &str) -> i32 {
    match p.get(key) {
        Some(v) if !v.is_null() => v.as_i64().unwrap_or(0) as i32,
        _ => 0,
    }
}

fn has_param(p: &Value, key: &str) -> bool {
    matches!(p.get(key), Some(v) if !v.is_null())
}

pub(crate) fn finished(info: &str) -> Value {
    json!({"status": "finished", "info": info})
}

pub fn subtract_blank(project: &mut Project, p: &Value) -> Result<Value> {
    let blank_threshold = p
        .get("blank_threshold")
        .and_then(Value::as_f64)
        .unwrap_or(5.0) as f32;
    let rt_expand = p.get("rt_expand").and_then(Value::as_f64).unwrap_or(10.0) as f32;
    let mz_expand = p.get("mz_expand").and_then(Value::as_f64).unwrap_or(0.005) as f32;
    let min_traces_intensity = p
        .get("min_traces_intensity")
        .and_then(Value::as_f64)
        .unwrap_or(0.0) as f32;
    if blank_threshold < 0.0 || rt_expand < 0.0 || mz_expand < 0.0 || min_traces_intensity < 0.0 {
        return Err(invalid("invalid blank subtraction parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    crate::nta_blank_subtraction::subtract_blank_impl(
        &mut data,
        blank_threshold,
        rt_expand,
        mz_expand,
        min_traces_intensity,
    )?;
    persist_features(project, &data)?;
    Ok(finished("Blank subtraction completed."))
}

pub fn filter_features(project: &mut Project, p: &Value) -> Result<Value> {
    // Optional numeric filters: null/absent (R NA) disable the filter via NaN.
    let mut has_only_filled = false;
    let mut only_filled_value = false;
    if let Some(v) = p.get("only_filled") {
        if !v.is_null() {
            has_only_filled = true;
            only_filled_value = v.as_bool().unwrap_or(false);
        }
    }
    let mut data = load_analysis_features(project, p)?;
    crate::nta_filters::filter_features_impl(
        &mut data,
        opt_real(p, "min_sn"),
        opt_real(p, "min_intensity"),
        opt_real(p, "min_area"),
        opt_real(p, "min_width"),
        opt_real(p, "max_width"),
        opt_real(p, "max_ppm"),
        opt_real(p, "min_fwhm_rt"),
        opt_real(p, "max_fwhm_rt"),
        opt_real(p, "min_fwhm_mz"),
        opt_real(p, "max_fwhm_mz"),
        opt_real(p, "min_gaussian_a"),
        opt_real(p, "min_gaussian_mu"),
        opt_real(p, "max_gaussian_mu"),
        opt_real(p, "min_gaussian_sigma"),
        opt_real(p, "max_gaussian_sigma"),
        opt_real(p, "min_gaussian_r2"),
        opt_real(p, "max_jaggedness"),
        opt_real(p, "min_sharpness"),
        opt_real(p, "min_asymmetry"),
        opt_real(p, "max_asymmetry"),
        opt_int(p, "max_modality"),
        has_param(p, "max_modality"),
        opt_real(p, "min_plates"),
        has_only_filled,
        only_filled_value,
        p.get("remove_filled")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        opt_int(p, "min_size_eic"),
        has_param(p, "min_size_eic"),
        opt_int(p, "min_size_ms1"),
        has_param(p, "min_size_ms1"),
        opt_int(p, "min_size_ms2"),
        has_param(p, "min_size_ms2"),
        opt_real(p, "min_rel_presence_replicate"),
        p.get("remove_isotopes")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        p.get("remove_adducts")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        p.get("remove_losses")
            .and_then(Value::as_bool)
            .unwrap_or(false),
    )?;
    persist_features(project, &data)?;
    Ok(finished("Features filtered."))
}

pub fn filter_features_ms2(project: &mut Project, p: &Value) -> Result<Value> {
    let top = p.get("top").and_then(Value::as_i64).unwrap_or(0) as i32;
    let min_intensity_ms2 = p
        .get("min_intensity_ms2")
        .and_then(Value::as_f64)
        .unwrap_or(f64::NAN) as f32;
    let rel_min_intensity = p
        .get("rel_min_intensity")
        .and_then(Value::as_f64)
        .unwrap_or(f64::NAN) as f32;
    let blank_clean = p
        .get("blank_clean")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let mz_clust = p.get("mz_clust").and_then(Value::as_f64).unwrap_or(0.005) as f32;
    let blank_presence_threshold = p
        .get("blank_presence_threshold")
        .and_then(Value::as_f64)
        .unwrap_or(0.8) as f32;
    let global_presence_threshold = p
        .get("global_presence_threshold")
        .and_then(Value::as_f64)
        .unwrap_or(0.1) as f32;
    if top < 0
        || mz_clust < 0.0
        || blank_presence_threshold < 0.0
        || blank_presence_threshold > 1.0
        || global_presence_threshold < 0.0
        || global_presence_threshold > 1.0
    {
        return Err(invalid("invalid MS2 feature filtering parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    crate::nta_filters::filter_features_ms2_impl(
        &mut data,
        top,
        min_intensity_ms2,
        rel_min_intensity,
        blank_clean,
        mz_clust,
        blank_presence_threshold,
        global_presence_threshold,
    )?;
    persist_features(project, &data)?;
    Ok(finished("MS2 peak lists filtered."))
}

pub fn group_features(project: &mut Project, p: &Value) -> Result<Value> {
    let method = p
        .get("method")
        .and_then(Value::as_str)
        .unwrap_or("internal_standards")
        .to_string();
    let rt_deviation = p.get("rt_deviation").and_then(Value::as_f64).unwrap_or(5.0) as f32;
    let ppm = p.get("ppm").and_then(Value::as_f64).unwrap_or(10.0) as f32;
    let min_samples = p.get("min_samples").and_then(Value::as_i64).unwrap_or(1) as i32;
    let bin_size = p.get("bin_size").and_then(Value::as_f64).unwrap_or(5.0) as f32;
    if method.is_empty() || rt_deviation < 0.0 || ppm < 0.0 || min_samples < 1 || bin_size <= 0.0 {
        return Err(invalid("invalid feature grouping parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    if method == "internal_standards" {
        load_internal_standards(project, &mut data)?;
    }
    crate::nta_alignment::group_features_impl(
        &mut data,
        &method,
        rt_deviation,
        ppm,
        min_samples,
        bin_size,
    )?;
    persist_features(project, &data)?;
    Ok(finished("Features grouped."))
}
pub fn fill_features(project: &mut Project, p: &Value) -> Result<Value> {
    let within_replicate = p
        .get("within_replicate")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let filtered = p.get("filtered").and_then(Value::as_bool).unwrap_or(false);
    let rt_expand = p.get("rt_expand").and_then(Value::as_f64).unwrap_or(10.0) as f32;
    let mz_expand = p.get("mz_expand").and_then(Value::as_f64).unwrap_or(0.01) as f32;
    let max_peak_width = p
        .get("max_peak_width")
        .and_then(Value::as_f64)
        .unwrap_or(30.0) as f32;
    let min_traces_intensity = p
        .get("min_traces_intensity")
        .and_then(Value::as_f64)
        .unwrap_or(1000.0) as f32;
    let min_number_traces = p
        .get("min_number_traces")
        .and_then(Value::as_i64)
        .unwrap_or(5) as i32;
    let min_intensity_ms1 = p
        .get("min_intensity")
        .or_else(|| p.get("min_intensity_ms1"))
        .and_then(Value::as_f64)
        .unwrap_or(5000.0) as f32;
    let rt_apex_deviation = p
        .get("rt_apex_deviation")
        .and_then(Value::as_f64)
        .unwrap_or(5.0) as f32;
    let min_signal_to_noise_ratio = p
        .get("min_signal_to_noise_ratio")
        .and_then(Value::as_f64)
        .unwrap_or(3.0) as f32;
    let min_gaussian_fit = p
        .get("min_gaussian_fit")
        .and_then(Value::as_f64)
        .unwrap_or(0.2) as f32;
    if rt_expand < 0.0
        || mz_expand < 0.0
        || max_peak_width <= 0.0
        || min_traces_intensity < 0.0
        || min_number_traces < 1
        || min_intensity_ms1 < 0.0
        || rt_apex_deviation < 0.0
        || min_signal_to_noise_ratio < 0.0
        || min_gaussian_fit < 0.0
        || min_gaussian_fit > 1.0
    {
        return Err(invalid("invalid gap filling parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    crate::nta_gap_filling::fill_features_impl(
        &mut data,
        within_replicate,
        filtered,
        rt_expand,
        mz_expand,
        max_peak_width,
        min_traces_intensity,
        min_number_traces,
        min_intensity_ms1,
        rt_apex_deviation,
        min_signal_to_noise_ratio,
        min_gaussian_fit,
    )?;
    persist_features(project, &data)?;
    Ok(finished("Feature gaps filled."))
}

pub fn create_components(project: &mut Project, p: &Value) -> Result<Value> {
    let min_correlation = p
        .get("min_correlation")
        .and_then(Value::as_f64)
        .unwrap_or(0.8) as f32;
    let mut rt_window: Vec<f32> = p
        .get("rt_window")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_f64)
                .map(|x| x as f32)
                .collect()
        })
        .unwrap_or_default();
    if rt_window.is_empty() {
        rt_window = vec![0.0, 0.0];
    }
    if min_correlation < 0.0 || min_correlation > 1.0 {
        return Err(invalid("invalid componentization parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    crate::nta_componentization::create_components_impl(&mut data, &rt_window, min_correlation)?;
    persist_features(project, &data)?;
    Ok(finished("Components created."))
}

pub fn annotate_components(project: &mut Project, p: &Value) -> Result<Value> {
    let max_isotopes = p.get("max_isotopes").and_then(Value::as_i64).unwrap_or(5) as i32;
    let max_charge = p.get("max_charge").and_then(Value::as_i64).unwrap_or(1) as i32;
    let max_gaps = p.get("max_gaps").and_then(Value::as_i64).unwrap_or(1) as i32;
    let ppm = p.get("ppm").and_then(Value::as_f64).unwrap_or(10.0) as f32;
    let isotope_elements: Vec<String> = p
        .get("isotope_elements")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_else(|| {
            vec![
                "C:1-60".into(),
                "N:0-10".into(),
                "O:0-20".into(),
                "S:0-4".into(),
                "Cl:0-6".into(),
                "Br:0-4".into(),
            ]
        });
    if max_isotopes < 1 || max_charge < 1 || max_gaps < 0 || ppm < 0.0 {
        return Err(invalid("invalid annotation parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    crate::nta_annotation::annotate_components_impl(
        &mut data,
        max_isotopes,
        max_charge,
        max_gaps,
        ppm,
        &isotope_elements,
        "",
        "",
    )?;
    persist_features(project, &data)?;
    Ok(finished("Components annotated."))
}

pub fn suspect_screening(project: &mut Project, p: &Value) -> Result<Value> {
    let ppm = p.get("ppm").and_then(Value::as_f64).unwrap_or(5.0);
    let sec = p.get("sec").and_then(Value::as_f64).unwrap_or(10.0);
    let ppm_ms2 = p.get("ppm_ms2").and_then(Value::as_f64).unwrap_or(10.0);
    let mzr_ms2 = p.get("mzr_ms2").and_then(Value::as_f64).unwrap_or(0.008);
    let min_cosine_similarity = p
        .get("min_cosine_similarity")
        .and_then(Value::as_f64)
        .unwrap_or(0.7);
    let min_shared_fragments = p
        .get("min_shared_fragments")
        .and_then(Value::as_i64)
        .unwrap_or(3) as i32;
    let filtered = p.get("filtered").and_then(Value::as_bool).unwrap_or(true);
    if ppm < 0.0
        || sec < 0.0
        || ppm_ms2 < 0.0
        || mzr_ms2 < 0.0
        || min_cosine_similarity < 0.0
        || min_cosine_similarity > 1.0
        || min_shared_fragments < 0
    {
        return Err(invalid("invalid suspect screening parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    let suspects = parse_suspect_targets(p);
    let analyses = data.analysis_names().to_vec();
    crate::nta_suspect_screening::suspect_screening_impl(
        &mut data,
        &analyses,
        &suspects,
        ppm,
        sec,
        ppm_ms2,
        mzr_ms2,
        min_cosine_similarity,
        min_shared_fragments,
        filtered,
    )?;
    persist_features(project, &data)?;
    persist_suspects(project, &data)?;
    Ok(finished("Suspect screening completed."))
}

pub fn filter_suspects(project: &mut Project, p: &Value) -> Result<Value> {
    let names: Vec<String> = p
        .get("names")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    let min_score = opt_real(p, "min_score");
    let max_error_rt = opt_real(p, "max_error_rt");
    let max_error_mass = opt_real(p, "max_error_mass");
    let id_levels: Vec<i32> = p
        .get("id_levels")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_i64)
                .map(|x| x as i32)
                .collect()
        })
        .unwrap_or_default();
    let min_shared_fragments = p
        .get("min_shared_fragments")
        .and_then(Value::as_i64)
        .unwrap_or(0) as i32;
    let min_cosine_similarity = opt_real(p, "min_cosine_similarity");
    if min_shared_fragments < 0 {
        return Err(invalid("invalid suspect filtering parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    load_suspects(project, &mut data)?;
    crate::nta_filters::filter_suspects_impl(
        &mut data,
        &names,
        min_score,
        max_error_rt,
        max_error_mass,
        &id_levels,
        min_shared_fragments,
        min_cosine_similarity,
    )?;
    persist_suspects(project, &data)?;
    Ok(finished("Suspects filtered."))
}
pub fn find_internal_standards(project: &mut Project, p: &Value) -> Result<Value> {
    let ppm = p.get("ppm").and_then(Value::as_f64).unwrap_or(5.0);
    let sec = p.get("sec").and_then(Value::as_f64).unwrap_or(10.0);
    let ppm_ms2 = p.get("ppm_ms2").and_then(Value::as_f64).unwrap_or(10.0);
    let mzr_ms2 = p.get("mzr_ms2").and_then(Value::as_f64).unwrap_or(0.008);
    let min_cosine_similarity = p
        .get("min_cosine_similarity")
        .and_then(Value::as_f64)
        .unwrap_or(0.7);
    let min_shared_fragments = p
        .get("min_shared_fragments")
        .and_then(Value::as_i64)
        .unwrap_or(3) as i32;
    let filtered = p.get("filtered").and_then(Value::as_bool).unwrap_or(true);
    if ppm < 0.0
        || sec < 0.0
        || ppm_ms2 < 0.0
        || mzr_ms2 < 0.0
        || min_cosine_similarity < 0.0
        || min_cosine_similarity > 1.0
        || min_shared_fragments < 0
    {
        return Err(invalid("invalid internal standard parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    let suspects = parse_suspect_targets(p);
    let analyses = data.analysis_names().to_vec();
    crate::nta_suspect_screening::find_internal_standards_impl(
        &mut data,
        &analyses,
        &suspects,
        ppm,
        sec,
        ppm_ms2,
        mzr_ms2,
        min_cosine_similarity,
        min_shared_fragments,
        filtered,
    )?;
    persist_features(project, &data)?;
    persist_internal_standards(project, &data)?;
    Ok(finished("Internal standards found."))
}

pub fn filter_internal_standards(project: &mut Project, p: &Value) -> Result<Value> {
    let names: Vec<String> = p
        .get("names")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    let min_score = opt_real(p, "min_score");
    let max_error_rt = opt_real(p, "max_error_rt");
    let max_error_mass = opt_real(p, "max_error_mass");
    let id_levels: Vec<i32> = p
        .get("id_levels")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_i64)
                .map(|x| x as i32)
                .collect()
        })
        .unwrap_or_default();
    let min_shared_fragments = p
        .get("min_shared_fragments")
        .and_then(Value::as_i64)
        .unwrap_or(0) as i32;
    let min_cosine_similarity = opt_real(p, "min_cosine_similarity");
    if min_shared_fragments < 0 {
        return Err(invalid("invalid internal standard filtering parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    load_internal_standards(project, &mut data)?;
    crate::nta_filters::filter_internal_standards_impl(
        &mut data,
        &names,
        min_score,
        max_error_rt,
        max_error_mass,
        &id_levels,
        min_shared_fragments,
        min_cosine_similarity,
    )?;
    persist_internal_standards(project, &data)?;
    Ok(finished("Internal standards filtered."))
}

pub fn correct_matrix_suppression(project: &mut Project, p: &Value) -> Result<Value> {
    let mp_rt_window = p
        .get("mp_rt_window")
        .and_then(Value::as_f64)
        .unwrap_or(10.0) as f32;
    let mut ref_blank_replicate = p
        .get("ref_blank_replicate")
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string();
    if ref_blank_replicate == "NA" || ref_blank_replicate == "NA_character_" {
        ref_blank_replicate.clear();
    }
    if mp_rt_window <= 0.0 {
        return Err(invalid("invalid matrix suppression correction parameters"));
    }
    let mut data = load_analysis_features(project, p)?;
    load_internal_standards(project, &mut data)?;
    crate::nta_correction_algorithms::correct_matrix_suppression_impl(
        &mut data,
        mp_rt_window,
        &ref_blank_replicate,
    )?;
    persist_features(project, &data)?;
    Ok(finished("Matrix suppression corrected."))
}

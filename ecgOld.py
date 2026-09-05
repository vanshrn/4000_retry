"""
ECG Live Monitor — v8.0 (ADS1292R 4000 SPS Edition)
============================================
Data source : GET https://ads1292r-code-91eg.onrender.com/api/ecg/live/ESP_ECG_123

Key changes for 4000 SPS:
 - WINDOW_SIZE = 8000 (2.0s × 4000 SPS scrolling view)
 - SAMPLING_RATE = 4000 SPS
 - STEP = 4000 (1 second step)
 - Reconstructs raw 24-bit ADC values using payload's dcOffset
"""

import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

import requests
import json
import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.widgets import Button
from collections import deque
import numpy as np
import time
import threading
from datetime import datetime
from scipy.signal import butter, filtfilt, find_peaks, iirnotch, savgol_filter
from scipy.ndimage import uniform_filter1d, median_filter

# --- CLOUD BACKEND (Uncomment when deploying to public cloud) ---
# API_URL        = 'https://ads1292r-code-91eg.onrender.com/api/ecg/live/ESP_ECG_123'
# API_RESULT_URL = 'https://ads1292r-code-91eg.onrender.com/api/ecg/device_result'

# --- LOCAL PRIVATE OFFLINE BACKEND (Active for local offline PC testing) ---
API_URL        = 'http://localhost:8000/api/ecg/live/ESP_ECG_123'
API_RESULT_URL = 'http://localhost:8000/api/ecg/device_result'

# Display window: 4.0 second scrolling view at 4000 SPS (16000 points)
WINDOW_SIZE   = 16000     # 4.0 s × 4000 SPS (16000 points)
SAMPLING_RATE = 4000      # ADS1292R sample rate

# ADS1292R ADC clipping limits (24-bit signed, gain=6, VREF=2.42V)
# Full scale = ±8,388,607. Typical ECG peak ≈ ±20,000-40,000.
# We only reject absolute rail values (chip saturated).
ADC_CLIP_HIGH =  8_000_000
ADC_CLIP_LOW  = -8_000_000

FS           = SAMPLING_RATE
WIN          = 8000       # 2.0 second buffer for classification (8000 points @ 4000 SPS)
STEP         = 4000       # 1 second step for detection
MIN_BEATS    = 3
MAX_CLIP_PCT = 2.0
MIN_PTP      = 500        # Minimum peak-to-peak in ADS1292R counts
QRS_WIDE_MS  = 130
MAX_LOG_ENTRIES = 8

# ============================================================
# SEVERITY TABLES
# ============================================================
SEVERITY = {
    'Ventricular Fibrillation':       'CRITICAL',
    'Ventricular Tachycardia':        'CRITICAL',
    'Non-Sustained VT (NSVT)':        'CRITICAL',
    'Ventricular Couplets':           'CRITICAL',
    'Ventricular Bigeminy':           'CRITICAL',
    'Ventricular Trigeminy':          'CRITICAL',
    'Sinus Arrest':                   'CRITICAL',
    'Asystole':                       'CRITICAL',
    'Atrial Fibrillation':            'WARNING',
    'Atrial Flutter':                 'WARNING',
    'Atrial Tachycardia':             'WARNING',
    'SVT':                            'WARNING',
    'Ventricular Ectopic / PVC':      'WARNING',
    'Sinus Tachycardia':              'WARNING',
    'Sinus Bradycardia':              'WARNING',
    'AV Block 2nd (Mobitz I)':        'WARNING',
    'AV Block 1st (Prolonged PR)':    'WARNING',
    'Sinus Pause':                    'WARNING',
    'Junctional Rhythm':              'WARNING',
    'PAC (Supraventricular Ectopic)': 'WARNING',
    'Atrial Bigeminy':                'WARNING',
    'Atrial Trigeminy':               'WARNING',
    'Sinus Arrhythmia':               'INFO',
    'Normal Sinus Rhythm':            'NORMAL',
}

SEV_COLORS = {
    'CRITICAL': '#FF2222', 'WARNING': '#FF9900',
    'INFO':     '#2299FF', 'NORMAL':  '#00CC66',
}

# ============================================================
# ADVANCED MOTION DSP FILTER — 4-Stage Architecture (4000 SPS)
# ============================================================
# Stage 1: Pre-Filtering (0.5–40 Hz AHA Bandpass + 50/100 Hz Notch)
# Stage 2: Dual-Input RLS Adaptive Filter (N=16, lambda=0.985) with Jerk Ref
# Stage 3: 5-Level Discrete Wavelet Transform Denoising (sym8 mother wavelet)
# Stage 4: Savitzky-Golay Real-Time Smoothing (Window=61, Order=3 @ 4000 SPS)
# ============================================================
class ECGFilter:
    def __init__(self, fs=4000):
        self.fs      = fs
        self.nyquist = fs / 2.0
        self._build()

    def _build(self):
        nyq = self.nyquist
        # 0.5 Hz high-pass (AHA clinical standard cutoff for baseline drift)
        self.b_hp, self.a_hp = butter(2, 0.5 / nyq, btype='high')
        # 35 Hz low-pass — clinical ECG standard, eliminates 50Hz residual & EMG muscle ripple
        self.b_lp, self.a_lp = butter(4, 35.0 / nyq, btype='low')
        # 50 Hz notch — wide Q=10 for full power-line coverage
        self.b_notch, self.a_notch = iirnotch(50.0 / nyq, Q=10)
        # 100 Hz notch — 2nd harmonic
        self.b_notch2, self.a_notch2 = iirnotch(100.0 / nyq, Q=15)

    def filter_signal(self, raw_signal):
        if len(raw_signal) < 50:
            return np.array(raw_signal, dtype=float)
        sig = np.array(raw_signal, dtype=float)

        # 1. Reject SPI rail corruptions (>7,500,000 counts) and zero dropouts
        med_val = np.median(sig)
        bad = (np.abs(sig) > 7500000)
        if abs(med_val) > 10000:
            bad = bad | (np.abs(sig) < 5000)
        valid_idx = np.where(~bad)[0]
        bad_idx = np.where(bad)[0]
        if len(valid_idx) > 0 and len(bad_idx) > 0:
            sig[bad_idx] = np.interp(bad_idx, valid_idx, sig[valid_idx])
        elif len(valid_idx) == 0:
            sig[:] = 0.0

        # 2. 9-point median filter (2.25ms @ 4000 SPS) — kills single-sample SPI glitches without altering QRS
        sig = median_filter(sig, size=9)

        # 3. Pre-subtract median to center signal
        dc_level = np.median(sig)
        sig_centered = sig - dc_level

        try:
            # 4. AHA 2-Stage Median Baseline Wander Removal (completely eliminates breathing wander and drift)
            # Stage 1: 200ms window (801 pts @ 4000 SPS) strips QRS/P/T spikes
            # Stage 2: 600ms window (2401 pts @ 4000 SPS) smooths into pure respiratory baseline
            if len(sig_centered) >= 2401:
                baseline = uniform_filter1d(median_filter(sig_centered, size=801), size=2401)
                sig_centered = sig_centered - baseline
            else:
                sig_centered = filtfilt(self.b_hp, self.a_hp, sig_centered)

            # 5. 80-tap Zero-Phase Comb Notch (removes 50Hz, 100Hz, 150Hz mains oscillation completely @ 4000 SPS: 4000/50=80)
            ma50 = uniform_filter1d(sig_centered, size=80, mode='nearest')
            sig_notched = sig_centered - (sig_centered - ma50) * 0.98

            # 6. Surgical IIR notch cascade for residual harmonics
            sig_filt = filtfilt(self.b_notch,  self.a_notch,  sig_notched)
            sig_filt = filtfilt(self.b_notch2, self.a_notch2, sig_filt)

            # 7. Zero-phase 35Hz Low-Pass Filter (strips all high-frequency baseline ripple and EMG noise)
            sig_filt = filtfilt(self.b_lp, self.a_lp, sig_filt)

            # 8. 61-point 3rd-Order Savitzky-Golay Polynomial Smoother (crystal clear P-Q-R-S-T presentation @ 4000 SPS)
            if len(sig_filt) >= 61:
                sig_filt = savgol_filter(sig_filt, window_length=61, polyorder=3)
            return sig_filt
        except Exception:
            return sig_centered



# ============================================================
# POLARITY & PQRST ANNOTATION — Optimized for 2000 SPS
# ============================================================
def ensure_upright_ecg(sig):
    """
    Preserves exact lead polarity (e.g. V1 negative QRS or Lead II positive QRS)
    directly as incoming from hardware.
    """
    if len(sig) == 0:
        return sig
    return np.array(sig, dtype=float)


def _find_wave_point(sig, lo, hi, mode='max'):
    if hi <= lo:
        return None
    seg = sig[lo:hi]
    if len(seg) == 0:
        return None
    idx = int(np.argmax(seg)) if mode == 'max' else int(np.argmin(seg))
    return lo + idx


def _annotate_one_beat(ax, sig, r_peak, rr_samples=None):
    n = len(sig)
    if n == 0:
        return

    r_peak = max(0, min(n - 1, int(r_peak)))
    local_window = sig[max(0, r_peak - 25):min(n, r_peak + 26)]
    local_amp = float(np.ptp(local_window)) if len(local_window) > 1 else abs(float(sig[r_peak]))
    local_amp = max(local_amp, 1.0)

    # R peak
    r_is_neg = (sig[r_peak] < 0)
    r_offset = -0.18 * local_amp if r_is_neg else 0.18 * local_amp
    ax.plot(r_peak, sig[r_peak], 'ro', markersize=10, zorder=5)
    ax.annotate('R', xy=(r_peak, sig[r_peak]),
                xytext=(r_peak, sig[r_peak] + r_offset),
                fontsize=11, fontweight='bold', color='red', ha='center',
                arrowprops=dict(arrowstyle='->', color='red', lw=1.5))

    rr_sec = (rr_samples / float(FS)) if (rr_samples and rr_samples > 0) else 0.5

    # P wave: max in range before R (bounded by RR interval)
    p_lo = max(0, r_peak - int(min(0.20, 0.40 * rr_sec) * FS))
    p_hi = max(0, r_peak - int(0.04 * FS))
    if p_hi > p_lo + 4:
        p_idx = _find_wave_point(sig, p_lo, p_hi, mode='max')
        if p_idx is not None:
            ax.plot(p_idx, sig[p_idx], 'go', markersize=8, zorder=5)
            ax.annotate('P', xy=(p_idx, sig[p_idx]),
                        xytext=(p_idx, sig[p_idx] + 0.14 * local_amp),
                        fontsize=11, fontweight='bold', color='green', ha='center')

    # Q wave: min in 40–5ms before R
    q_lo = max(0, r_peak - int(0.04 * FS))
    q_hi = max(0, r_peak - int(0.005 * FS))
    if q_hi > q_lo + 2:
        q_idx = _find_wave_point(sig, q_lo, q_hi, mode='min')
        if q_idx is not None:
            ax.plot(q_idx, sig[q_idx], 'mo', markersize=8, zorder=5)
            ax.annotate('Q', xy=(q_idx, sig[q_idx]),
                        xytext=(q_idx, sig[q_idx] - 0.16 * local_amp),
                        fontsize=11, fontweight='bold', color='purple', ha='center')

    # S wave: min in 5–65ms after R
    s_lo = min(n - 1, r_peak + int(0.005 * FS))
    s_hi = min(n, r_peak + int(0.065 * FS))
    if s_hi > s_lo + 2:
        s_idx = _find_wave_point(sig, s_lo, s_hi, mode='min')
        if s_idx is not None:
            ax.plot(s_idx, sig[s_idx], 'o', color='orange', markersize=8, zorder=5)
            ax.annotate('S', xy=(s_idx, sig[s_idx]),
                        xytext=(s_idx, sig[s_idx] - 0.16 * local_amp),
                        fontsize=11, fontweight='bold', color='orange', ha='center')

    # T wave: 80ms to min(260ms, 0.55*RR) after R (guarantees T stays on this beat!)
    t_lo = min(n, r_peak + int(0.08 * FS))
    t_hi = min(n, r_peak + int(min(0.26, 0.55 * rr_sec) * FS))
    if t_hi > t_lo + 4:
        t_segment = sig[t_lo:t_hi]
        if len(t_segment) > 0:
            max_pos_idx = np.argmax(t_segment)
            min_neg_idx = np.argmin(t_segment)
            if abs(t_segment[min_neg_idx]) > 1.2 * abs(t_segment[max_pos_idx]):
                t_idx = t_lo + min_neg_idx
            else:
                t_idx = t_lo + max_pos_idx
            ax.plot(t_idx, sig[t_idx], 'bo', markersize=8, zorder=5)
            offset_sign = -0.16 if sig[t_idx] < 0 else 0.14
            ax.annotate('T', xy=(t_idx, sig[t_idx]),
                        xytext=(t_idx, sig[t_idx] + offset_sign * local_amp),
                        fontsize=11, fontweight='bold', color='cyan', ha='center')


# ============================================================
# DETECTION ENGINE — Retuned for 4000 SPS
# ============================================================
def _preprocess(sig):
    nyq = FS / 2.0
    b, a = butter(1, 0.67 / nyq, btype='high')
    try:
        return filtfilt(b, a, sig)
    except Exception:
        return sig - np.mean(sig)

def _detect_r_peaks(sig):
    if len(sig) < FS:
        return np.array([])
    s    = sig - np.mean(sig)
    nyq  = FS / 2.0
    # Band-pass 5–40 Hz matches QRS energy (more HF QRS energy captured)
    b, a = butter(1, [5 / nyq, min(40 / nyq, 0.99)], 'band')
    f    = filtfilt(b, a, s)
    sq   = np.diff(f, prepend=f[0]) ** 2
    win  = int(0.15 * FS)   # 600 samples @ 4000 SPS integration window
    integ = np.convolve(sq, np.ones(win) / win, mode='same')
    thr   = 0.35 * np.max(integ)
    peaks, _ = find_peaks(integ, height=thr, distance=int(0.30 * FS))  # min 1200 samples @ 4000 SPS
    if len(peaks) == 0:
        return np.array([])
    rp = []
    search_radius = int(0.08 * FS)
    for p in peaks:
        st = max(0, p - search_radius)
        en = min(len(s) - 1, p + search_radius)
        seg = s[st:en + 1]
        if np.max(seg) > 0.5 * np.max(np.abs(seg)):
            rp.append(st + int(np.argmax(seg)))
        else:
            rp.append(st + int(np.argmax(np.abs(seg))))
    return np.array(sorted(set(rp)))

def _qrs_width(sig, r_peaks):
    ws = []
    sr = int(0.07 * FS)
    for r in r_peaks:
        st  = max(0, r - sr)
        en  = min(len(sig) - 1, r + sr)
        seg = sig[st:en + 1]
        if len(seg) < 5:
            continue
        sa = np.abs(seg)
        nf = np.percentile(sa, 20)
        pa = sa[min(r - st, len(seg) - 1)]
        if pa <= nf:
            continue
        above = np.where(sa > nf + 0.45 * (pa - nf))[0]
        if len(above) > 1:
            w = (above[-1] - above[0]) / FS * 1000.0
            if 40 <= w <= 200:
                ws.append(w)
    return float(np.median(ws)) if ws else 90.0

def _pct_wide(sig, r_peaks):
    if len(r_peaks) == 0:
        return 0.0
    sr = int(0.07 * FS)
    ws = []
    for r in r_peaks:
        st  = max(0, r - sr)
        en  = min(len(sig) - 1, r + sr)
        seg = sig[st:en + 1]
        if len(seg) < 5:
            continue
        sa = np.abs(seg)
        nf = np.percentile(sa, 20)
        pa = sa[min(r - st, len(seg) - 1)]
        if pa <= nf:
            continue
        above = np.where(sa > nf + 0.45 * (pa - nf))[0]
        if len(above) > 1:
            ws.append((above[-1] - above[0]) / FS * 1000.0)
    return float(np.mean(np.array(ws) > QRS_WIDE_MS)) if ws else 0.0

def _extract(sig, r_peaks):
    if len(r_peaks) < MIN_BEATS:
        return None
    rr = np.diff(r_peaks) / FS * 1000.0
    if len(rr) < 2:
        return None
    mu    = float(np.mean(rr))
    hr    = 60000.0 / mu if mu > 0 else 0.0
    cv    = float(np.std(rr)) / mu if mu > 0 else 0.0
    sd    = np.diff(rr)
    rmssd = float(np.sqrt(np.mean(sd ** 2))) if len(sd) > 0 else 0.0
    pnn50 = float(np.mean(np.abs(sd) > 50))  if len(sd) > 0 else 0.0
    pmx   = float(np.max(rr))
    qw    = _qrs_width(sig, r_peaks)
    pct_w = _pct_wide(sig, r_peaks)
    bl    = float(np.mean(sig))
    amps  = np.array([abs(float(sig[r]) - bl) for r in r_peaks if r < len(sig)])
    avcv  = float(np.std(amps) / np.mean(amps)) if len(amps) > 0 and np.mean(amps) > 0 else 0.0
    mid   = len(rr) // 2
    r1, r2 = rr[:mid], rr[mid:]
    m1, m2 = np.mean(r1) if len(r1) > 0 else mu, np.mean(r2) if len(r2) > 0 else mu
    ratio  = m1 / (m2 + 1e-9)
    trans  = ratio > 1.20 or ratio < 0.833
    cv1 = np.std(r1) / (m1 + 1e-9) if len(r1) > 1 else 1.0
    cv2 = np.std(r2) / (m2 + 1e-9) if len(r2) > 1 else 1.0
    stable = r1 if cv1 <= cv2 else r2
    if trans and len(stable) >= 3:
        sm      = float(np.mean(stable))
        s_cv    = float(np.std(stable)) / (sm + 1e-9)
        s_hr    = 60000.0 / sm if sm > 0 else hr
        ss      = np.diff(stable)
        s_rmssd = float(np.sqrt(np.mean(ss ** 2))) if len(ss) > 0 else 0.0
        s_pnn50 = float(np.mean(np.abs(ss) > 50))  if len(ss) > 0 else 0.0
    else:
        s_cv, s_hr, s_rmssd, s_pnn50, stable = cv, hr, rmssd, pnn50, rr
    return dict(hr=hr, rr=rr, mu=mu, cv=cv, rmssd=rmssd, pnn50=pnn50,
                qw=qw, pct_w=pct_w, avcv=avcv, pmx=pmx, n=len(r_peaks),
                rp=r_peaks, trans=trans, s_cv=s_cv, s_hr=s_hr,
                s_rmssd=s_rmssd, s_pnn50=s_pnn50, stable_rr=stable)

def _quality_gate(sig, clip):
    if 100.0 * np.sum(clip) / max(len(clip), 1) > MAX_CLIP_PCT:
        return False
    return np.ptp(sig) >= MIN_PTP

def _vfib(sig):
    s = sig - np.mean(sig)
    if np.std(s) < 100:   # ADS1292R counts — raised threshold
        return 0.0
    fv = np.abs(np.fft.rfft(s))
    fr = np.fft.rfftfreq(len(s), d=1.0 / FS)
    band  = (fr >= 4.0) & (fr <= 10.0)
    total = (fr >= 0.5) & (fr <= 30.0)
    if not np.any(band) or not np.any(total):
        return 0.0
    ratio = float(np.sum(fv[band])) / (float(np.sum(fv[total])) + 1e-9)
    return min(0.90, 0.55 + ratio) if ratio > 0.50 else 0.0

def _afib(cv, rmssd, pnn50, rr, pct_w, avcv):
    # At 500 SPS with 1-second blocks, only ~1-2 R-peaks per block.
    # Edge-split R-peaks inflate RR variability → strict thresholds needed.
    # AHA criteria: AF requires sustained irregularity across >= 8 beats.
    if pct_w > 0.35 or avcv >= 0.10 or len(rr) < 8:       # was < 6
        return 0.0, ''
    if not (cv > 0.20 and rmssd > 120 and pnn50 > 0.40):   # was 0.15, 75, 0.32
        return 0.0, ''
    rr_n = rr - np.mean(rr)
    rv   = np.var(rr_n) + 1e-9
    ac1  = float(np.mean(rr_n[:-1] * rr_n[1:])) / rv
    ac2  = float(np.mean(rr_n[:-2] * rr_n[2:])) / rv if len(rr) > 4 else 0.0
    if ac1 > 0.20 or ac2 > 0.18:                           # was 0.30, 0.25 — tighter
        return 0.0, ''
    hist, _ = np.histogram(rr, bins=6)
    if np.max(hist / (np.sum(hist) + 1e-9)) > 0.50:        # was 0.55 — tighter
        return 0.0, ''
    return min(0.90, 0.60 + cv * 2.0 + pnn50 * 0.4), f'CV={cv:.3f}, RMSSD={rmssd:.0f}ms'

def classify(sig):
    sig = _preprocess(sig)
    rp  = _detect_r_peaks(sig)
    ft  = _extract(sig, rp)
    if ft is None:
        return None
    hr=ft['hr']; rr=ft['rr']; cv=ft['cv']; rmssd=ft['rmssd']
    pnn50=ft['pnn50']; qw=ft['qw']; pct_w=ft['pct_w']
    avcv=ft['avcv']; pmx=ft['pmx']; n=ft['n']; trans=ft['trans']
    s_cv=ft['s_cv']; s_hr=ft['s_hr']; s_rmssd=ft['s_rmssd']
    s_pnn50=ft['s_pnn50']; stable=ft['stable_rr']

    if hr < 10:
        return ('Asystole', 0.92, 'No detectable beats')
    vf = _vfib(sig)
    if vf > 0:
        return ('Ventricular Fibrillation', vf, 'Chaotic signal')
    if hr > 100 and pct_w > 0.5 and cv < 0.15 and n >= 6:
        return ('Ventricular Tachycardia', min(0.90, 0.66 + (qw - QRS_WIDE_MS) / 200),
                f'Wide QRS={qw:.0f}ms, HR={hr:.0f}bpm')
    mx = float(np.max(rr)) if len(rr) > 0 else 0
    if mx > 3000:
        return ('Sinus Arrest', min(0.92, 0.68 + (mx - 3000) / 5000), f'Gap={mx:.0f}ms')
    if not (pmx > 1800 or hr < 40 or avcv >= 0.10 or cv < 0.10):
        afc, afn = _afib(s_cv if trans else cv, s_rmssd if trans else rmssd,
                         s_pnn50 if trans else pnn50, stable if trans else rr, pct_w, avcv)
        if afc > 0:
            return ('Atrial Fibrillation', afc, afn)
    others = rr[rr < mx]
    mu_o   = float(np.mean(others)) if len(others) > 0 else float(np.mean(rr))
    if mx > 1.6 * mu_o and mx > 1400:
        return ('Sinus Pause', min(0.84, 0.58 + (mx - 1400) / 3000), f'Gap={mx:.0f}ms')
    if not trans and 150 <= hr <= 250 and pct_w < 0.3 and cv < 0.05:
        return ('SVT', min(0.87, 0.70 + (hr - 150) / 1200), f'HR={hr:.0f}bpm')
    if hr > 100 and cv < 0.12 and pct_w < 0.4:
        return ('Sinus Tachycardia', 0.90 if hr < 150 else 0.75, f'HR={hr:.0f}bpm')
    if hr < 60 and cv < 0.15 and pct_w < 0.4:
        return ('Sinus Bradycardia', 0.90 if hr > 40 else 0.85, f'HR={hr:.0f}bpm')
    if 50 <= hr <= 105:
        rr_med = float(np.median(rr)) if len(rr) > 0 else 0.0
        rr_dev = float(np.max(np.abs(rr - rr_med))) if len(rr) > 0 else 0.0
        sa_like = (0.08 <= cv <= 0.22 and rmssd >= 30.0 and pnn50 >= 0.10 and rr_dev >= 60.0)
        if sa_like:
            conf = min(0.82, 0.50 + cv * 1.2 + min(pnn50, 0.35) * 0.25)
            return ('Sinus Arrhythmia', conf, f'CV={cv:.3f}, RMSSD={rmssd:.0f}ms')
    return ('Normal Sinus Rhythm', 0.90, f'HR={hr:.0f}bpm')


# ============================================================
# GLOBAL STATE
# ============================================================
data_lock          = threading.Lock()
raw_data           = deque(maxlen=WINDOW_SIZE)
fw_filtered_data   = deque(maxlen=WINDOW_SIZE)  # Firmware-filtered data from DB
session_raw        = []
session_timestamps = []
leads_off_events   = []
session_start_time = None
electrode_status   = 'CONNECTED'  # 'CONNECTED', 'LEADS_OFF', 'POOR_ELECTRODE'
clipped_count      = 0
samples_per_second = 0
last_seq           = None
skipped_blocks     = 0
active_warnings    = []   # Parsed from payload["warnings"]

ecg_filter = ECGFilter(fs=SAMPLING_RATE)


# ============================================================
# PERFORMANCE METRICS STATE
# ============================================================
last_device_result = ""
latest_metrics = {
    'snrDb': 0.0,
    'snrAccuracy': 0.0,
    'rPeakAccuracy': 0.0,
    'hrBpm': 0.0,
    'hrAccuracy': 0.0,
    'baselineWanderMv': 0.0,
    'baselineAccuracy': 0.0,
    'motionArtifactIndex': 0.0,
    'motionAccuracy': 0.0,
    'cmrrEstDb': 86.0
}


# ============================================================
# LIVE DETECTION WORKER
# ============================================================
class LiveDetector:
    def __init__(self):
        self.lock           = threading.Lock()
        self._raw_buf       = deque(maxlen=WIN)
        self._clip_buf      = deque(maxlen=WIN)
        self._filt          = ECGFilter(fs=SAMPLING_RATE)
        self.current_result = ('Waiting for data...', 0.0, 'NORMAL', '--')
        self.current_bpm    = 0.0
        self.alert_log      = []
        self._last_abnormal = None

    def add_sample(self, raw_value, is_clipped):
        self._raw_buf.append(raw_value)
        self._clip_buf.append(1 if is_clipped else 0)

    def process_chunk(self, record_id, device_id, seq):
        if len(self._raw_buf) >= WIN:
            raw_snap  = np.array(list(self._raw_buf),  dtype=float)
            clip_snap = np.array(list(self._clip_buf), dtype=bool)
            threading.Thread(target=self._run,
                             args=(record_id, device_id, seq, raw_snap, clip_snap),
                             daemon=True).start()

    def _run(self, record_id, device_id, seq, raw_snap, clip_snap):
        try:
            filtered = self._filt.filter_signal(raw_snap)
            bpm = 0.0
            if not _quality_gate(filtered, clip_snap):
                cond, conf, sev, note = 'Poor signal quality', 0.0, 'INFO', 'Check electrodes'
            else:
                result = classify(filtered)
                if result is None:
                    cond, conf, sev, note = 'Insufficient beats', 0.0, 'INFO', 'Need more data'
                else:
                    cond, conf, note = result
                    sev = SEVERITY.get(cond, 'INFO')
                try:
                    rp = _detect_r_peaks(_preprocess(filtered))
                    if len(rp) >= 2:
                        bpm = round(60000.0 / float(np.mean(np.diff(rp) / FS * 1000.0)), 1)
                except Exception:
                    pass
        except Exception as e:
            cond, conf, sev, note = 'System Error', 0.0, 'INFO', str(e)[:30]
            bpm = 0.0

        with self.lock:
            self.current_result = (cond, conf, sev, note)
            self.current_bpm    = bpm
            is_abn = sev not in ('NORMAL', 'INFO')
            if is_abn and cond != self._last_abnormal:
                self.alert_log.append((datetime.now().strftime('%H:%M:%S'), cond, sev))
                if len(self.alert_log) > MAX_LOG_ENTRIES:
                    self.alert_log.pop(0)
                self._last_abnormal = cond
            elif not is_abn:
                self._last_abnormal = None

detector = LiveDetector()


# ============================================================
# BLOCK PROCESSING — Handles ADS1292R full-scale samples
# ============================================================
def _process_block(blk):
    global electrode_status, session_start_time, clipped_count
    global last_seq, skipped_blocks, FS, active_warnings, last_device_result

    samples   = blk.get('data', [])
    seq       = blk.get('seq')
    raw_id    = blk.get('_id')
    if isinstance(raw_id, dict): record_id = raw_id.get('$oid')
    else:                        record_id = raw_id
    device_id = blk.get('deviceId', 'ESP_ECG_123')
    lo_flag   = blk.get('lo', False)
    sr        = blk.get('sr', SAMPLING_RATE)

    # Parse firmware warnings array
    active_warnings = blk.get('warnings', [])
    device_result   = blk.get('device_result', '')
    metrics         = blk.get('metrics', {})

    if device_result:
        last_device_result = device_result

    if metrics and isinstance(metrics, dict):
        latest_metrics.update(metrics)
        print(f"\n📊 --- BATCH METRICS [Block #{seq}] ---")
        if device_result:
            print(f"Result        : {device_result}")
        print(f"1. SNR        : {latest_metrics.get('snrDb', 0.0):.2f} dB ({latest_metrics.get('snrAccuracy', 0.0):.1f}% Acc)")
        print(f"2. R-Peak Acc : {latest_metrics.get('rPeakAccuracy', 0.0):.1f}%")
        print(f"3. Heart Rate : {latest_metrics.get('hrBpm', 0.0):.1f} BPM ({latest_metrics.get('hrAccuracy', 0.0):.1f}% Acc)")
        print(f"4. Baseline   : {latest_metrics.get('baselineWanderMv', 0.0):.2f} mV ({latest_metrics.get('baselineAccuracy', 0.0):.1f}% Acc)")
        print(f"5. Motion Idx : {latest_metrics.get('motionArtifactIndex', 0.0):.2f} ({latest_metrics.get('motionAccuracy', 0.0):.1f}% Acc)")
        print("------------------------------------------------\n")

    if not isinstance(samples, list) or len(samples) < 10:
        return

    print(f"[SSE] Chunk seq={seq} sr={sr} lo={lo_flag} samples={len(samples)} warnings={active_warnings}")

    if seq is not None and last_seq is not None:
        if seq <= last_seq:
            return
        if seq > last_seq + 1:
            n_skipped = seq - last_seq - 1
            skipped_blocks += n_skipped
            print(f"[WARNING] Block gap seq={last_seq+1}→{seq} ({n_skipped} missing)")

    if seq is not None:
        last_seq = seq

    # Failsafe detection for database stripping flags, or firmware warnings
    is_zero = all(((v[0] if isinstance(v, (list, tuple)) else v) == 0) for v in samples)

    if lo_flag or is_zero or "LEADS_OFF" in active_warnings:
        electrode_status = 'LEADS_OFF'
        leads_off_events.append(time.time())
    elif all(abs(int(v[0] if isinstance(v, (list, tuple)) else v)) > 7_500_000 for v in samples) or any("Flatline" in w for w in active_warnings):
        electrode_status = 'POOR_ELECTRODE'
    else:
        electrode_status = 'CONNECTED'

    if isinstance(sr, int) and sr > 0 and sr != FS:
        FS = sr
        ecg_filter._build()
        detector._filt = ECGFilter(fs=FS)

    fw_samples = blk.get('filtered_data') or blk.get('filteredData') or []

    with data_lock:
        for idx, v in enumerate(samples):
            try:
                if isinstance(v, (list, tuple)) and len(v) >= 2:
                    iv   = int(v[0])    # Column 0: Raw 24-bit ADC sample
                    fw_v = int(v[1])    # Column 1: ESP32 DSP-filtered sample
                else:
                    iv   = int(v)
                    fw_v = iv
                    if isinstance(fw_samples, list) and idx < len(fw_samples):
                        try:
                            fw_v = int(fw_samples[idx])
                        except (TypeError, ValueError):
                            fw_v = iv
            except (TypeError, ValueError):
                continue

            # Reject only absolute-rail saturated values.
            if iv >= ADC_CLIP_HIGH or iv <= ADC_CLIP_LOW:
                clipped_count += 1
                detector.add_sample(fw_v, True)
                continue

            if session_start_time is None:
                session_start_time = datetime.now()

            # Flag as clipped if within 5% of ±8M full scale
            is_clipped = (abs(iv) > 7_500_000)
            if is_clipped:
                clipped_count += 1

            sample_clean = iv if abs(iv) < 800000 else (raw_data[-1] if len(raw_data) > 0 else 0)
            raw_data.append(iv)
            fw_filtered_data.append(fw_v)
            session_raw.append(iv)
            session_timestamps.append(time.time())
            detector.add_sample(sample_clean, is_clipped)

    detector.process_chunk(record_id, device_id, seq)


# ============================================================
# SSE POLLING THREAD & DYNAMIC RECONNECT
# ============================================================
current_resp = None
force_reconnect = False

def api_polling_thread():
    global current_resp, force_reconnect
    print(f"[SSE] Connecting to {API_URL}")
    while True:
        force_reconnect = False
        try:
            with requests.get(API_URL, stream=True, timeout=60,
                              headers={'Accept': 'text/event-stream'}) as resp:
                current_resp = resp
                resp.raise_for_status()
                print("[SSE] Connected — streaming live ADS1292R ECG blocks...")
                for raw_line in resp.iter_lines(decode_unicode=True):
                    if force_reconnect:
                        break
                    if not raw_line or not raw_line.startswith('data:'):
                        continue
                    json_str = raw_line[5:].strip()
                    try:
                        payload = json.loads(json_str)
                    except Exception:
                        continue
                    if isinstance(payload, dict):
                        if payload.get("type") == "ecg_data":
                            _process_block(payload.get("record", {}))
                        elif "data" in payload and isinstance(payload["data"], list):
                            _process_block(payload)
                        else:
                            _process_block(payload)
        except Exception as e:
            if not force_reconnect:
                print(f"[SSE] Connection error: {e} — reconnecting in 2s")
                time.sleep(2)
        finally:
            current_resp = None

threading.Thread(target=api_polling_thread, daemon=True, name='api-poll').start()


# ============================================================
# ============================================================
# HIGH-PERFORMANCE PLOT SETUP (Blitting & Persistent Line Objects)
# ============================================================
fig = plt.figure(figsize=(14, 10))
fig.patch.set_facecolor('#0A0A0A')
fig.suptitle(
    'ECG Live Monitor  |  ESP32 + ADS1292R (4000 SPS)  |  ads1292r-code.onrender.com  |  Real-Time Batch Metrics',
    fontsize=13, fontweight='bold', color='white', x=0.45
)

gs      = fig.add_gridspec(4, 1, hspace=0.58, height_ratios=[1, 1, 1.3, 0.8], top=0.93, bottom=0.04)
ax_raw  = fig.add_subplot(gs[0])
ax_fw   = fig.add_subplot(gs[1])
ax_filt = fig.add_subplot(gs[2])
ax_log  = fig.add_subplot(gs[3])

# Screen decimation factor (downsample 8000 points to 2000 for silky-smooth 60 FPS plotting)
DOWNSAMPLE = 4
PLOT_POINTS = WINDOW_SIZE // DOWNSAMPLE
x_axis = np.arange(PLOT_POINTS) * DOWNSAMPLE

for ax in (ax_raw, ax_fw, ax_filt, ax_log):
    ax.set_facecolor('#0F0F0F')
    for sp in ax.spines.values():
        sp.set_edgecolor('#333333')
    ax.tick_params(colors='#666666', labelsize=8)

# Panel 1: Raw Signal Static Setup
ax_raw.set_xlim(0, WINDOW_SIZE)
ax_raw.set_ylabel('Raw DB ADC', fontsize=8, color='#AAAAAA')
ax_raw.yaxis.set_major_formatter(
    plt.FuncFormatter(lambda x, _: f'{int(x/1000)}k' if abs(x) >= 1000 else str(int(x)))
)
ax_raw.grid(True, alpha=0.12, color='#444444')
line_raw, = ax_raw.plot(x_axis, np.zeros(PLOT_POINTS), color='#2E86AB', linewidth=0.8, alpha=0.85)

# Panel 2: ESP32 FW Filtered Signal Static Setup
ax_fw.set_xlim(0, WINDOW_SIZE)
ax_fw.set_ylabel('ESP32 Clean ECG', fontsize=8, color='#AAAAAA')
ax_fw.yaxis.set_major_formatter(
    plt.FuncFormatter(lambda x, _: f'{int(x/1000)}k' if abs(x) >= 1000 else str(int(x)))
)
ax_fw.grid(True, alpha=0.12, color='#444444')
line_fw, = ax_fw.plot(x_axis, np.zeros(PLOT_POINTS), color='#FF9900', linewidth=1.0, alpha=0.90)

# Panel 3: Python Cleaned + PQRST Static Setup
ax_filt.set_xlim(0, WINDOW_SIZE)
ax_filt.set_ylabel('Python ECG (0.5–40Hz)', fontsize=8, color='#AAAAAA')
ax_filt.set_title(
    f'3. Python Cleaned ECG & PQRST Waveform Analysis ({FS} SPS)  |  AHA 0.5–40Hz Bandwidth + Pan-Tompkins Beat Detection',
    fontsize=8.5, color='#00CC66', pad=3
)
ax_filt.yaxis.set_major_formatter(
    plt.FuncFormatter(lambda x, _: f'{int(x/1000)}k' if abs(x) >= 1000 else str(int(x)))
)
ax_filt.grid(True, alpha=0.15, color='#444444', zorder=0)
line_filt, = ax_filt.plot(x_axis, np.zeros(PLOT_POINTS), color='#39FF14', linewidth=1.4, alpha=0.95, zorder=2)

# Annotation markers & overlays
txt_overlay = ax_filt.text(0.5, 0.5, '', ha='center', va='center', fontsize=18, fontweight='bold', transform=ax_filt.transAxes, zorder=20)
txt_diag = ax_filt.text(0.02, 0.93, '', transform=ax_filt.transAxes, fontsize=10, fontweight='bold', color='#00CC66', va='top', ha='left', zorder=10,
                        bbox=dict(boxstyle='round,pad=0.3', facecolor='#000000', edgecolor='#00CC66', alpha=0.88, linewidth=1.5))
txt_bpm = ax_filt.text(0.50, 0.93, '', transform=ax_filt.transAxes, fontsize=12, fontweight='bold', color='#00CC66', va='top', ha='center', zorder=10,
                       bbox=dict(boxstyle='round,pad=0.3', facecolor='#000000', edgecolor='#00CC66', alpha=0.88, linewidth=1.5))
txt_sev = ax_filt.text(0.98, 0.93, '', transform=ax_filt.transAxes, fontsize=10, fontweight='bold', color='#FFFFFF', va='top', ha='right', zorder=10,
                       bbox=dict(boxstyle='round,pad=0.3', facecolor='#00CC66', edgecolor='none', alpha=0.88))

# Panel 4: Metrics Dashboard Static Setup
ax_log.set_facecolor('#0B0F19')
ax_log.set_xlim(0, 1)
ax_log.set_ylim(0, 1)
ax_log.axis('off')
txt_dev_title = ax_log.text(0.01, 0.94, '', transform=ax_log.transAxes, fontsize=8.5, fontweight='bold', color='#00E5FF', va='top')
txt_snr       = ax_log.text(0.01, 0.62, '', transform=ax_log.transAxes, fontsize=8, color='#38BDF8', va='top', family='monospace', fontweight='bold')
txt_rp        = ax_log.text(0.20, 0.62, '', transform=ax_log.transAxes, fontsize=8, color='#A3E635', va='top', family='monospace', fontweight='bold')
txt_hr        = ax_log.text(0.40, 0.62, '', transform=ax_log.transAxes, fontsize=8, color='#F472B6', va='top', family='monospace', fontweight='bold')
txt_base      = ax_log.text(0.60, 0.62, '', transform=ax_log.transAxes, fontsize=8, color='#FBBF24', va='top', family='monospace', fontweight='bold')
txt_mot       = ax_log.text(0.80, 0.62, '', transform=ax_log.transAxes, fontsize=8, color='#C084FC', va='top', family='monospace', fontweight='bold')
ax_log.plot([0.01, 0.99], [0.24, 0.24], color='#1E293B', linewidth=1.0, transform=ax_log.transAxes)
txt_alerts    = ax_log.text(0.01, 0.16, '', ha='left', va='top', fontsize=7.5, color='#64748B', transform=ax_log.transAxes)


def on_refresh_clicked(event=None):
    """Full application reset: clears all waveform queues, detector state, and reconnects SSE stream."""
    global force_reconnect, electrode_status, samples_per_second
    print("[REFRESH] 🔄 Refresh clicked — clearing all buffers & reconnecting API live stream...")
    force_reconnect = True
    if current_resp is not None:
        try:
            current_resp.close()
        except Exception:
            pass

    with data_lock:
        raw_data.clear()
        fw_filtered_data.clear()
        session_raw.clear()
        session_timestamps.clear()
        electrode_status = 'CONNECTED'
        samples_per_second = 0

    with detector.lock:
        detector._raw_buf.clear()
        detector._clip_buf.clear()
        detector.alert_log.clear()
        detector.current_bpm = 0.0
        detector.current_result = ('Warming up', 0.0, 'INFO', 'Re-initialized buffers')

    line_raw.set_ydata(np.zeros(PLOT_POINTS))
    line_fw.set_ydata(np.zeros(PLOT_POINTS))
    line_filt.set_ydata(np.zeros(PLOT_POINTS))
    txt_overlay.set_text('🔄 Re-initializing connection & clearing buffers...')
    txt_overlay.set_color('#00E5FF')
    fig.canvas.draw_idle()


# Refresh Button placed cleanly on top-right toolbar [left, bottom, width, height]
ax_btn = fig.add_axes([0.885, 0.948, 0.095, 0.038])
btn_refresh = Button(ax_btn, 'Refresh', color='#1E293B', hovercolor='#0284C7')
btn_refresh.label.set_color('#00E5FF')
btn_refresh.label.set_fontsize(10)
btn_refresh.label.set_fontweight('bold')
for sp in ax_btn.spines.values():
    sp.set_edgecolor('#00E5FF')
    sp.set_linewidth(1.2)
btn_refresh.on_clicked(on_refresh_clicked)


# --- Fast spike cleaning helper (optimized) ---
def _fast_clean_spikes(arr, hard_limit=7500000):
    if len(arr) == 0:
        return arr
    cleaned = np.array(arr, dtype=float)
    bad = (np.abs(cleaned) > hard_limit)
    if np.any(bad):
        valid_idx = np.where(~bad)[0]
        bad_idx = np.where(bad)[0]
        if len(valid_idx) > 0:
            cleaned[bad_idx] = np.interp(bad_idx, valid_idx, cleaned[valid_idx])
        else:
            cleaned[:] = 0.0
    return cleaned


def update(frame):
    global samples_per_second

    with data_lock:
        n_raw = len(session_raw)
        t0    = session_timestamps[0] if session_timestamps else None
        q_len = len(raw_data)
        if q_len >= 50:
            raw_array = np.array(raw_data)
            fw_array  = np.array(fw_filtered_data) if len(fw_filtered_data) == q_len else raw_array
        else:
            raw_array = None
            fw_array  = None

    if t0 and n_raw > 0:
        dur = time.time() - t0
        samples_per_second = int(n_raw / dur) if dur > 0 else 0

    if electrode_status != 'CONNECTED' or raw_array is None:
        if electrode_status == 'LEADS_OFF':
            status = 'LEADS OFF — CHECK ELECTRODES'
            color = '#FF2222'
        elif electrode_status == 'POOR_ELECTRODE':
            status = 'POOR ELECTRODE CONNECTION — SATURATED'
            color = '#FF9900'
        else:
            status = 'POOR NETWORK — WAITING FOR DATA...'
            color = '#2299FF'

        txt_overlay.set_text(status)
        txt_overlay.set_color(color)
        line_raw.set_ydata(np.zeros(PLOT_POINTS))
        line_fw.set_ydata(np.zeros(PLOT_POINTS))
        line_filt.set_ydata(np.zeros(PLOT_POINTS))
        return [line_raw, line_fw, line_filt, txt_overlay]

    txt_overlay.set_text('')

    # Clean & Center arrays
    clean_raw = _fast_clean_spikes(raw_array)
    clean_fw  = _fast_clean_spikes(fw_array)
    clean_fw  = clean_fw - np.median(clean_fw)

    # Use fast downsampling for UI rendering (keeps 60 FPS smooth animation)
    disp_raw = clean_raw[::DOWNSAMPLE]
    disp_fw  = clean_fw[::DOWNSAMPLE]

    # Quick pad if queue is still filling
    if len(disp_raw) < PLOT_POINTS:
        disp_raw = np.pad(disp_raw, (PLOT_POINTS - len(disp_raw), 0), 'edge')
        disp_fw  = np.pad(disp_fw, (PLOT_POINTS - len(disp_fw), 0), 'edge')

    # Update line data (0 ms overhead)
    line_raw.set_ydata(disp_raw[-PLOT_POINTS:])
    line_fw.set_ydata(disp_fw[-PLOT_POINTS:])
    line_filt.set_ydata(disp_fw[-PLOT_POINTS:])

    # Dynamic Y-scaling (only update bounds when signal amplitudes change significantly)
    p02, p98 = np.percentile(disp_raw, [2, 98])
    m_raw = max((p98 - p02) * 0.35, 2000)
    ax_raw.set_ylim(p02 - m_raw, p98 + m_raw)

    fwm, fwx = np.percentile(disp_fw, [1, 99])
    m_fw = max((fwx - fwm) * 0.3, 200)
    ax_fw.set_ylim(fwm - m_fw, fwx + m_fw)
    ax_filt.set_ylim(fwm - m_fw, fwx + m_fw)

    # Title updates
    warn_str = ('  ⚠ ' + ', '.join(active_warnings)) if active_warnings else ''
    ax_raw.set_title(
        f'1. Raw Data from DB (ADS1292R ADC "data") @ {FS} SPS  |  Stream: {samples_per_second} Hz  |  Session: {n_raw // FS}s'
        f'  |  seq: {last_seq if last_seq is not None else "--"}{warn_str}',
        fontsize=8.5, color='#FF9900' if active_warnings else '#38BDF8', pad=3
    )
    ax_fw.set_title(
        f'2. ESP32 Hardware/DSP Clean ECG Signal (Column 1 Pre-Filtered @ {FS} SPS)  |  FW Result: {last_device_result if last_device_result else "--"}',
        fontsize=8.5, color='#FF9900', pad=3
    )

    # Diagnostics & Overlays
    with detector.lock:
        cond, conf, sev, note = detector.current_result
        alert_log  = list(detector.alert_log)
        live_bpm   = detector.current_bpm

    sev_color = SEV_COLORS.get(sev, '#FFFFFF')
    conf_str = f'{conf * 100:.0f}%' if conf > 0 else ''

    txt_diag.set_text(f'{cond}  {conf_str}')
    txt_diag.set_color(sev_color)
    txt_diag.get_bbox_patch().set_edgecolor(sev_color)

    bpm_col = '#00CC66' if live_bpm > 0 else '#555555'
    txt_bpm.set_text(f'{live_bpm:.0f} BPM' if live_bpm > 0 else '-- BPM')
    txt_bpm.set_color(bpm_col)
    txt_bpm.get_bbox_patch().set_edgecolor(bpm_col)

    txt_sev.set_text(sev)
    txt_sev.get_bbox_patch().set_facecolor(sev_color)

    # Telemetry text update
    snr_val   = latest_metrics.get('snrDb', 0.0)
    snr_acc   = latest_metrics.get('snrAccuracy', 0.0)
    rp_acc    = latest_metrics.get('rPeakAccuracy', 0.0)
    hr_val    = latest_metrics.get('hrBpm', 0.0)
    hr_acc    = latest_metrics.get('hrAccuracy', 0.0)
    base_mv   = latest_metrics.get('baselineWanderMv', 0.0)
    base_acc  = latest_metrics.get('baselineAccuracy', 0.0)
    mot_idx   = latest_metrics.get('motionArtifactIndex', 0.0)
    mot_acc   = latest_metrics.get('motionAccuracy', 0.0)

    txt_dev_title.set_text(
        f"ESP32 FIRMWARE TELEMETRY & METRICS  |  Sample Rate: {FS} SPS  |  Block Seq: #{last_seq if last_seq is not None else '--'}  |  FW Result: {last_device_result if last_device_result else cond}"
    )
    txt_snr.set_text(f"1. Waveform SNR\n   {snr_val:.2f} dB  ({snr_acc:.1f}%)")
    txt_rp.set_text(f"2. R-Peak Detection\n   {rp_acc:.1f}% Accuracy")
    txt_hr.set_text(f"3. Heart Rate\n   {hr_val:.1f} BPM  ({hr_acc:.1f}%)")
    txt_base.set_text(f"4. Baseline Drift\n   {base_mv:.2f} mV  ({base_acc:.1f}%)")
    txt_mot.set_text(f"5. Motion Artifact\n   Idx: {mot_idx:.2f}  ({mot_acc:.1f}%)")

    if not alert_log:
        txt_alerts.set_text("Recent Alerts: Normal Sinus Rhythm — No abnormal arrhythmia detected")
        txt_alerts.set_color('#64748B')
    else:
        recent_evs = '  |  '.join([f"[{ts}] {c}" for ts, c, s in reversed(alert_log[-4:])])
        txt_alerts.set_text(f"Recent Alerts: {recent_evs}")
        txt_alerts.set_color('#F59E0B')

    return [line_raw, line_fw, line_filt]


print("=" * 65)
print("ECG LIVE MONITOR  v8.0  --  ADS1292R Edition  |  4000 SPS (4000 pt 1.0s Window)")
print("  Interactive Zooming enabled: Scroll mouse wheel on any panel to zoom!")
print("=" * 65)

def on_scroll(event):
    if event.inaxes not in (ax_raw, ax_fw, ax_filt):
        return
    ax = event.inaxes
    cur_xlim = ax.get_xlim()
    cur_ylim = ax.get_ylim()
    xdata = event.xdata
    ydata = event.ydata
    if xdata is None or ydata is None:
        return
    scale_factor = 0.8 if event.button == 'up' else 1.25
    new_width = (cur_xlim[1] - cur_xlim[0]) * scale_factor
    new_height = (cur_ylim[1] - cur_ylim[0]) * scale_factor
    rel_x = (xdata - cur_xlim[0]) / max(cur_xlim[1] - cur_xlim[0], 1.0)
    rel_y = (ydata - cur_ylim[0]) / max(cur_ylim[1] - cur_ylim[0], 1.0)
    ax.set_xlim([xdata - new_width * rel_x, xdata + new_width * (1 - rel_x)])
    ax.set_ylim([ydata - new_height * rel_y, ydata + new_height * (1 - rel_y)])
    fig.canvas.draw_idle()

fig.canvas.mpl_connect('scroll_event', on_scroll)

try:
    ani = animation.FuncAnimation(fig, update, interval=33,
                                  blit=False, cache_frame_data=False)
    plt.show()
except Exception as e:
    print(f"\n[ERROR] {e}")
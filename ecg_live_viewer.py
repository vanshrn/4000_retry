"""
ECG Live Viewer — Ultra-Fast Real-Time Monitor (4000 SPS)
=========================================================
Lightweight, zero-lag real-time visualizer for ESP32-S3 + ADS1292R.
Connects to live SSE stream and renders:
  1. Top: Raw 24-bit ADC ECG Signal
  2. Bottom: ESP32 Hardware DSP Filtered ECG Signal
"""

import sys
import io
import time
import json
import threading
from collections import deque
import requests
import numpy as np

# Set standard output encoding
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

import matplotlib
try:
    matplotlib.use('TkAgg')
except Exception:
    pass
import matplotlib.pyplot as plt
import matplotlib.animation as animation

# ============================================================
# CONFIGURATION
# ============================================================
API_URL       = 'https://ads1292r-code-91eg.onrender.com/api/ecg/live/ESP_ECG_123'
SAMPLING_RATE = 4000       # 4000 SPS
VIEW_SECONDS  = 2.5        # 2.5 seconds scrolling window (10,000 points @ 4000 SPS)
WINDOW_POINTS = int(VIEW_SECONDS * SAMPLING_RATE)
DOWNSAMPLE    = 4          # Decimate 10,000 -> 2,500 points for silky-smooth 60 FPS plotting

PLOT_LEN = WINDOW_POINTS // DOWNSAMPLE
t_axis = np.linspace(-VIEW_SECONDS, 0.0, PLOT_LEN)

# Thread-safe ring buffers
raw_buffer  = deque(maxlen=WINDOW_POINTS)
filt_buffer = deque(maxlen=WINDOW_POINTS)
data_lock   = threading.Lock()

# Live status trackers
latest_seq     = 0
latest_leads_off = False
latest_metrics = {}
connected_time = None
blocks_received = 0

# ============================================================
# BACKGROUND SSE STREAM RECEIVER
# ============================================================
def sse_stream_worker():
    global latest_seq, latest_leads_off, latest_metrics, connected_time, blocks_received
    print(f"[STREAM] Connecting to {API_URL}...")
    
    while True:
        try:
            with requests.get(API_URL, stream=True, timeout=30, headers={'Accept': 'text/event-stream'}) as resp:
                resp.raise_for_status()
                connected_time = time.time()
                print("[STREAM] Connected! Streaming 4000 SPS live ECG data...")
                
                for line in resp.iter_lines(decode_unicode=True):
                    if not line or not line.startswith('data:'):
                        continue
                    
                    json_str = line[5:].strip()
                    try:
                        payload = json.loads(json_str)
                    except Exception:
                        continue
                    
                    if not isinstance(payload, dict):
                        continue
                    
                    # Extract block data (direct or wrapped inside 'record')
                    blk = payload.get("record", payload)
                    samples = blk.get("data", [])
                    
                    if not isinstance(samples, list) or len(samples) == 0:
                        continue
                    
                    latest_seq = blk.get("seq", latest_seq + 1)
                    latest_leads_off = blk.get("leadsOff", blk.get("leads_off", False))
                    latest_metrics = blk.get("metrics", {})
                    blocks_received += 1
                    
                    # Unpack samples: [raw, filtered]
                    raw_chunk = []
                    filt_chunk = []
                    for s in samples:
                        if isinstance(s, (list, tuple)) and len(s) >= 2:
                            raw_chunk.append(float(s[0]))
                            filt_chunk.append(float(s[1]))
                        elif isinstance(s, (int, float)):
                            raw_chunk.append(float(s))
                            filt_chunk.append(float(s))
                    
                    with data_lock:
                        raw_buffer.extend(raw_chunk)
                        filt_buffer.extend(filt_chunk)
                        
        except Exception as e:
            print(f"[STREAM] Disconnected ({e}). Reconnecting in 1.5s...")
            time.sleep(1.5)

# Start background data fetcher
fetch_thread = threading.Thread(target=sse_stream_worker, daemon=True, name='ecg-fetcher')
fetch_thread.start()

# ============================================================
# FAST HIGH-PERFORMANCE UI SETUP
# ============================================================
plt.style.use('dark_background')
fig, (ax_raw, ax_filt) = plt.subplots(2, 1, figsize=(12, 7), sharex=True)
fig.canvas.manager.set_window_title('Lightweight ECG Live Monitor — 4000 SPS')

# Customize Background and Grids
fig.patch.set_facecolor('#0d1117')
for ax in (ax_raw, ax_filt):
    ax.set_facecolor('#161b22')
    ax.grid(True, color='#21262d', linestyle='--', linewidth=0.7)
    ax.tick_params(colors='#8b949e', labelsize=9)
    for spine in ax.spines.values():
        spine.set_color('#30363d')

# Line Objects
line_raw, = ax_raw.plot([], [], color='#f78166', linewidth=1.1, label='Raw ADC Signal (24-bit)')
line_filt, = ax_filt.plot([], [], color='#3fb950', linewidth=1.4, label='ESP32 DSP Filtered Signal (Clean ECG)')

ax_raw.set_title('Raw ECG Signal (ADC Counts)', fontsize=11, fontweight='bold', color='#f78166', loc='left')
ax_filt.set_title('DSP Filtered ECG (DC Block + 80-tap 50Hz Notch + FIR Low-Pass)', fontsize=11, fontweight='bold', color='#3fb950', loc='left')
ax_filt.set_xlabel('Time (Seconds)', fontsize=10, color='#8b949e')

ax_raw.set_xlim(-VIEW_SECONDS, 0.0)
ax_filt.set_xlim(-VIEW_SECONDS, 0.0)

# Status info text overlays
info_text = fig.text(0.5, 0.96, 'Connecting to Live Stream...', fontsize=11, fontweight='bold', color='#58a6ff', ha='center')
metrics_text = fig.text(0.98, 0.96, '', fontsize=9, color='#8b949e', ha='right')

# ============================================================
# REAL-TIME ANIMATION LOOP (60 FPS)
# ============================================================
def init():
    line_raw.set_data([], [])
    line_filt.set_data([], [])
    return line_raw, line_filt, info_text, metrics_text

def update_frame(frame):
    with data_lock:
        if len(raw_buffer) < 100:
            return line_raw, line_filt, info_text, metrics_text
        
        raw_arr = np.array(raw_buffer)
        filt_arr = np.array(filt_buffer)
    
    # Decimate for lightning fast UI rendering
    if len(raw_arr) >= WINDOW_POINTS:
        raw_display  = raw_arr[-WINDOW_POINTS::DOWNSAMPLE]
        filt_display = filt_arr[-WINDOW_POINTS::DOWNSAMPLE]
    else:
        raw_display  = raw_arr[::DOWNSAMPLE]
        filt_display = filt_arr[::DOWNSAMPLE]
    
    n_pts = len(raw_display)
    x_curr = t_axis[-n_pts:]
    
    # Update line data
    line_raw.set_data(x_curr, raw_display)
    line_filt.set_data(x_curr, filt_display)
    
    # Dynamic Auto-Scaling (Smooth Y-Limits)
    if n_pts > 50:
        # Raw Signal Y-scale
        r_min, r_max = np.min(raw_display), np.max(raw_display)
        r_pad = max((r_max - r_min) * 0.1, 500.0)
        ax_raw.set_ylim(r_min - r_pad, r_max + r_pad)
        
        # Filtered Signal Y-scale
        f_min, f_max = np.min(filt_display), np.max(filt_display)
        f_pad = max((f_max - f_min) * 0.15, 1000.0)
        ax_filt.set_ylim(f_min - f_pad, f_max + f_pad)
    
    # Update Status Text
    if latest_leads_off:
        info_text.set_text(f"⚠️ LEADS OFF DETECTED | Sequence #{latest_seq} | Blocks: {blocks_received}")
        info_text.set_color('#ff7b72')
    else:
        info_text.set_text(f"🟢 STREAMING 4000 SPS LIVE | Sequence #{latest_seq} | Blocks: {blocks_received}")
        info_text.set_color('#3fb950')
    
    hr = latest_metrics.get('hrBpm', 0.0)
    snr = latest_metrics.get('snrDb', 0.0)
    metrics_text.set_text(f"HR: {hr:.1f} BPM  |  SNR: {snr:.1f} dB")
    
    return line_raw, line_filt, info_text, metrics_text

ani = animation.FuncAnimation(
    fig, update_frame, init_func=init, interval=25, blit=False, cache_frame_data=False
)

plt.subplots_adjust(top=0.91, bottom=0.08, left=0.08, right=0.95, hspace=0.32)

if __name__ == '__main__':
    print("Starting Lightweight Real-Time ECG Viewer...")
    plt.show()

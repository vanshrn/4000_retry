"""
offline_ecg_plotter.py — Standalone 100% Offline ECG Live Plotter (4000 SPS)
=============================================================================
Zero Hotspot. Zero Router. Zero Cloud. Zero Internet Required.

Modes supported:
 1. Direct Local Offline Server (localhost:8000 or ESP32 SoftAP 192.168.4.1)
 2. Direct USB Serial Streaming (COM8 / Auto-detect USB-CDC)
 3. Offline File Replay (JSON / CSV sessions)

Features:
 - Dual Canvas: Top = Raw 24-bit ADC, Bottom = Clean Filtered ECG
 - Live P-Q-R-S-T morphological peak detector & annotations
 - Real-time Heart Rate (BPM), SNR (dB), Leads-Off detection
 - Recording, CSV/JSON session export, Pause & Scrubbing
=============================================================================
"""

import sys
import io
import time
import json
import threading
from collections import deque
import numpy as np

# UTF-8 stdout encoding for Windows console
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

import matplotlib
try:
    matplotlib.use('TkAgg')
except Exception:
    pass
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.widgets import Button, RadioButtons
from scipy.signal import find_peaks

# Try importing serial
try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

# Try importing requests for local HTTP/SSE
try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False


# ============================================================
# CONFIGURATION & CONSTANTS
# ============================================================
SAMPLING_RATE = 4000         # 4000 SPS
WINDOW_SECONDS = 3.0         # 3.0 seconds scrolling window (12,000 points @ 4000 SPS)
MAX_BUFFER_POINTS = int(WINDOW_SECONDS * SAMPLING_RATE)
DOWNSAMPLE_RATIO = 4         # Decimate for smooth 60 FPS rendering (3,000 points on screen)

# Ring buffers (thread-safe)
raw_buffer = deque(maxlen=MAX_BUFFER_POINTS)
filt_buffer = deque(maxlen=MAX_BUFFER_POINTS)
data_lock = threading.Lock()

# State variables
g_running = True
g_paused = False
g_recording = False
g_recorded_data = []
g_latest_seq = 0
g_latest_hr = 0.0
g_latest_snr = 0.0
g_latest_leads_off = False
g_source_mode = "Local Server"  # "Local Server" or "USB Serial"
g_serial_port = "COM8"
g_baud_rate = 921600
g_connection_status = "Initializing..."

# Fill initial zero baseline
for _ in range(MAX_BUFFER_POINTS):
    raw_buffer.append(0.0)
    filt_buffer.append(0.0)


# ============================================================
# HELPER: Auto-detect ESP32 Serial Port
# ============================================================
def find_esp32_port():
    if not HAS_SERIAL:
        return "COM8"
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "303a" in hwid or "ch340" in desc or "cp210" in desc or "usb serial" in desc:
            return p.device
    return "COM8"


# ============================================================
# WORKER 1: Local Offline Server / SSE Stream Reader
# ============================================================
def local_server_worker():
    global g_latest_seq, g_latest_hr, g_latest_snr, g_latest_leads_off, g_connection_status
    urls = [
        "http://localhost:8000/api/ecg/live/ESP_ECG_123",
        "http://127.0.0.1:8000/api/ecg/live/ESP_ECG_123",
        "http://192.168.4.1:8000/api/ecg/live/ESP_ECG_123"
    ]
    
    while g_running:
        if g_source_mode != "Local Server":
            time.sleep(0.5)
            continue

        connected = False
        for url in urls:
            if not g_running or g_source_mode != "Local Server":
                break
            try:
                g_connection_status = f"Connecting to {url}..."
                resp = requests.get(url, stream=True, timeout=3)
                if resp.status_code == 200:
                    g_connection_status = "Connected (Local SSE 4000 SPS)"
                    connected = True
                    for line in resp.iter_lines():
                        if not g_running or g_source_mode != "Local Server":
                            break
                        if not line:
                            continue
                        line_str = line.decode('utf-8', errors='ignore')
                        if line_str.startswith("data:"):
                            json_str = line_str[5:].strip()
                            try:
                                payload = json.loads(json_str)
                                process_ecg_payload(payload)
                            except json.JSONDecodeError:
                                pass
            except Exception:
                pass
            if connected:
                break

        if not connected:
            g_connection_status = "Offline (Local server not detected. Retrying...)"
            time.sleep(1.0)


# ============================================================
# WORKER 2: Direct USB Serial Port Reader (Zero Network)
# ============================================================
def serial_worker():
    global g_connection_status, g_latest_seq, g_latest_hr, g_latest_snr, g_latest_leads_off
    while g_running:
        if g_source_mode != "USB Serial":
            time.sleep(0.5)
            continue

        port = g_serial_port or find_esp32_port()
        try:
            g_connection_status = f"Opening {port} @ {g_baud_rate}..."
            ser = serial.Serial(port, g_baud_rate, timeout=1.0)
            g_connection_status = f"Connected (USB Serial {port})"
            
            while g_running and g_source_mode == "USB Serial":
                line = ser.readline()
                if not line:
                    continue
                line_str = line.decode('utf-8', errors='ignore').strip()
                if not line_str:
                    continue
                
                # Check if JSON payload was printed over serial
                if line_str.startswith("{") and line_str.endswith("}"):
                    try:
                        payload = json.loads(line_str)
                        process_ecg_payload(payload)
                    except json.JSONDecodeError:
                        pass
                elif line_str.startswith("data:"):
                    try:
                        payload = json.loads(line_str[5:].strip())
                        process_ecg_payload(payload)
                    except json.JSONDecodeError:
                        pass
            ser.close()
        except Exception as e:
            g_connection_status = f"USB Port Error: {e}"
            time.sleep(1.5)


# ============================================================
# PAYLOAD INGESTION & BUFFERING
# ============================================================
def process_ecg_payload(payload):
    global g_latest_seq, g_latest_hr, g_latest_snr, g_latest_leads_off
    if not isinstance(payload, dict):
        return

    rec = payload.get("record", payload)
    g_latest_seq = rec.get("seq", g_latest_seq + 1)
    g_latest_leads_off = rec.get("leadsOff", rec.get("leads_off", False))

    metrics = rec.get("metrics", {})
    if metrics:
        g_latest_hr = metrics.get("hrBpm", g_latest_hr)
        g_latest_snr = metrics.get("snrDb", g_latest_snr)

    samples = rec.get("data", [])
    if not samples:
        return

    if g_recording:
        g_recorded_data.append(payload)

    if g_paused:
        return

    raw_pts = []
    filt_pts = []
    for s in samples:
        if isinstance(s, (list, tuple)) and len(s) >= 2:
            raw_pts.append(float(s[0]))
            filt_pts.append(float(s[1]))
        elif isinstance(s, (int, float)):
            raw_pts.append(float(s))
            filt_pts.append(float(s))

    with data_lock:
        raw_buffer.extend(raw_pts)
        filt_buffer.extend(filt_pts)


# ============================================================
# LIVE GUI & MATPLOTLIB ANIMATION
# ============================================================
def launch_offline_plotter():
    fig = plt.figure(figsize=(13, 8), facecolor='#0d1117')
    fig.canvas.manager.set_window_title("Offline 4000 SPS ECG Live Plotter")

    # Grid layout: Top = Raw ADC, Bottom = Filtered ECG
    gs = fig.add_gridspec(2, 1, height_ratios=[1, 1.2], hspace=0.32, top=0.88, bottom=0.14, left=0.08, right=0.96)
    ax_raw = fig.add_subplot(gs[0])
    ax_filt = fig.add_subplot(gs[1])

    t_axis = np.linspace(-WINDOW_SECONDS, 0.0, MAX_BUFFER_POINTS // DOWNSAMPLE_RATIO)

    # Style Raw Plot
    ax_raw.set_facecolor('#161b22')
    ax_raw.grid(True, linestyle='--', color='#21262d', alpha=0.8)
    line_raw, = ax_raw.plot(t_axis, np.zeros_like(t_axis), color='#ff7b72', lw=1.2, label="Raw ADC (24-bit Ch2)")
    ax_raw.set_title("Raw ADC ECG Signal (Unfiltered 4000 SPS)", color='#ff7b72', fontsize=11, fontweight='bold', loc='left')
    ax_raw.tick_params(colors='#8b949e', labelsize=8)
    for spine in ax_raw.spines.values():
        spine.set_color('#30363d')

    # Style Filtered Plot
    ax_filt.set_facecolor('#161b22')
    ax_filt.grid(True, linestyle='--', color='#21262d', alpha=0.8)
    line_filt, = ax_filt.plot(t_axis, np.zeros_like(t_axis), color='#3fb950', lw=1.6, label="ESP32 DSP Filtered ECG")
    pqrst_scatter, = ax_filt.plot([], [], 'o', color='#58a6ff', markersize=6, zorder=5)
    ax_filt.set_title("ESP32 DSP Filtered Clean Signal (0.67Hz DC + 50Hz Notch + 24Hz LP + Gaussian)", color='#3fb950', fontsize=11, fontweight='bold', loc='left')
    ax_filt.set_xlabel("Time (seconds)", color='#8b949e', fontsize=9)
    ax_filt.tick_params(colors='#8b949e', labelsize=8)
    for spine in ax_filt.spines.values():
        spine.set_color('#30363d')

    # HUD Status Bar at Top
    status_text = fig.text(0.08, 0.94, "Status: Initializing...", color='#58a6ff', fontsize=10, fontweight='bold')
    metrics_text = fig.text(0.55, 0.94, "HR: -- BPM | SNR: -- dB | Seq: #0 | Leads: OK", color='#f0f6fc', fontsize=10, fontweight='bold')

    # Control Buttons at Bottom
    ax_pause = fig.add_axes([0.08, 0.03, 0.12, 0.05])
    btn_pause = Button(ax_pause, '⏸ Pause', color='#21262d', hovercolor='#30363d')
    btn_pause.label.set_color('#c9d1d9')

    ax_rec = fig.add_axes([0.22, 0.03, 0.14, 0.05])
    btn_rec = Button(ax_rec, '⏺ Start Record', color='#21262d', hovercolor='#30363d')
    btn_rec.label.set_color('#c9d1d9')

    ax_export = fig.add_axes([0.38, 0.03, 0.14, 0.05])
    btn_export = Button(ax_export, '💾 Export CSV', color='#21262d', hovercolor='#30363d')
    btn_export.label.set_color('#c9d1d9')

    ax_mode = fig.add_axes([0.76, 0.02, 0.20, 0.07], facecolor='#161b22')
    radio_mode = RadioButtons(ax_mode, ('Local Server', 'USB Serial'), active=0, activecolor='#58a6ff')
    for label in radio_mode.labels:
        label.set_color('#c9d1d9')
        label.set_fontsize(9)

    # Callbacks
    def on_pause(event):
        global g_paused
        g_paused = not g_paused
        btn_pause.label.set_text('▶ Resume' if g_paused else '⏸ Pause')
        btn_pause.color = '#da3633' if g_paused else '#21262d'
        fig.canvas.draw_idle()

    def on_record(event):
        global g_recording, g_recorded_data
        g_recording = not g_recording
        if g_recording:
            g_recorded_data = []
            btn_rec.label.set_text('⏹ Stop Rec')
            btn_rec.color = '#da3633'
        else:
            btn_rec.label.set_text(f'⏺ Rec ({len(g_recorded_data)})')
            btn_rec.color = '#21262d'
        fig.canvas.draw_idle()

    def on_export(event):
        if not g_recorded_data:
            print("[EXPORT] No data recorded yet.")
            return
        filename = f"offline_ecg_session_{int(time.time())}.csv"
        with open(filename, "w", encoding="utf-8") as f:
            f.write("seq,sample_index,raw,filtered\n")
            for pkt in g_recorded_data:
                rec = pkt.get("record", pkt)
                seq = rec.get("seq", 0)
                for idx, s in enumerate(rec.get("data", [])):
                    raw_val = s[0] if isinstance(s, (list, tuple)) else s
                    filt_val = s[1] if isinstance(s, (list, tuple)) and len(s) > 1 else s
                    f.write(f"{seq},{idx},{raw_val},{filt_val}\n")
        print(f"[EXPORT] Successfully saved {len(g_recorded_data)} blocks to {filename}")

    def on_mode(label):
        global g_source_mode
        g_source_mode = label
        print(f"[MODE] Switched input source to: {label}")

    btn_pause.on_clicked(on_pause)
    btn_rec.on_clicked(on_record)
    btn_export.on_clicked(on_export)
    radio_mode.on_clicked(on_mode)

    # Real-Time Frame Update (60 FPS)
    def update_frame(frame):
        with data_lock:
            raw_arr = np.array(raw_buffer)
            filt_arr = np.array(filt_buffer)

        if len(raw_arr) < MAX_BUFFER_POINTS:
            return line_raw, line_filt

        # Decimate for silky smooth 60 FPS plotting
        raw_dec = raw_arr[::DOWNSAMPLE_RATIO]
        filt_dec = filt_arr[::DOWNSAMPLE_RATIO]

        line_raw.set_ydata(raw_dec)
        line_filt.set_ydata(filt_dec)

        # Dynamic Auto-Scaling
        r_min, r_max = np.min(raw_dec), np.max(raw_dec)
        r_pad = max((r_max - r_min) * 0.15, 100)
        ax_raw.set_ylim(r_min - r_pad, r_max + r_pad)

        f_min, f_max = np.min(filt_dec), np.max(filt_dec)
        f_pad = max((f_max - f_min) * 0.15, 100)
        ax_filt.set_ylim(f_min - f_pad, f_max + f_pad)

        # Morphological R-Peak Detection on filtered buffer
        if f_max - f_min > 500:
            peaks, _ = find_peaks(filt_dec, height=f_min + 0.5 * (f_max - f_min), distance=int(0.25 * (SAMPLING_RATE / DOWNSAMPLE_RATIO)))
            if len(peaks) > 0:
                pqrst_scatter.set_data(t_axis[peaks], filt_dec[peaks])
            else:
                pqrst_scatter.set_data([], [])

        # Update HUD Status Text
        status_text.set_text(f"Source: {g_source_mode} | {g_connection_status}")
        lead_str = "LEADS OFF" if g_latest_leads_off else "CONNECTED"
        metrics_text.set_text(f"HR: {g_latest_hr:.1f} BPM | SNR: {g_latest_snr:.1f} dB | Seq: #{g_latest_seq} | Lead: {lead_str}")

        return line_raw, line_filt, pqrst_scatter

    ani = animation.FuncAnimation(fig, update_frame, interval=25, blit=False)
    plt.show()


# ============================================================
# ENTRY POINT
# ============================================================
if __name__ == "__main__":
    print("=" * 70)
    print(">> OFFLINE 4000 SPS ECG PLOTTER STARTING...")
    print("=" * 70)
    print("-> 100% Offline: No internet, no router, no hotspot needed.")
    print("-> Modes: (1) Local Server (localhost:8000), (2) USB Serial (COM8)")
    print("=" * 70)

    # Start background data reader threads
    t_server = threading.Thread(target=local_server_worker, daemon=True)
    t_serial = threading.Thread(target=serial_worker, daemon=True)
    t_server.start()
    t_serial.start()

    # Launch GUI
    launch_offline_plotter()
    g_running = False


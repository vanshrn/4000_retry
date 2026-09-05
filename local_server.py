"""
local_server.py — Private Offline ECG Backend & WebSocket/SSE Relay Server
===========================================================================
Mimics the public Render cloud backend 100% locally on your PC.
- Receives 4000 SPS blocks from ESP32 via WebSocket (/ws) or HTTP POST (/api/ecg)
- Relays real-time data to ecg_dashboard.html and ecgOld.py via SSE (/api/ecg/live/{deviceId})
- 100% Offline: Zero internet needed, zero cloud delays, zero dropped packets.
===========================================================================
"""

import asyncio
import json
import socket
from typing import Set
from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, JSONResponse, StreamingResponse
import uvicorn

app = FastAPI(title="Private Offline ECG Server")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

sse_clients: Set[asyncio.Queue] = set()
ws_clients: Set[WebSocket] = set()
latest_payload = {}
device_results = {}
total_received_blocks = 0


def get_local_ip() -> str:
    """Finds the local LAN IP of this PC."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip


async def broadcast_to_sse(payload: dict):
    """Broadcasts received 4000 SPS ECG payload to all connected SSE clients."""
    global total_received_blocks, latest_payload
    total_received_blocks += 1
    latest_payload = payload

    seq = payload.get("seq", total_received_blocks)
    device_id = payload.get("deviceId", "ESP_ECG_123")
    sample_count = len(payload.get("data", []))
    hr = payload.get("metrics", {}).get("hrBpm", "—")
    snr = payload.get("metrics", {}).get("snrDb", "—")

    print(f"[LOCAL BACKEND] Block #{seq} ({sample_count} pts @ 4000 SPS) from {device_id} | HR={hr} BPM | SNR={snr} dB")

    sse_data = f"data: {json.dumps(payload)}\n\n"
    
    dead_queues = set()
    for q in sse_clients:
        try:
            q.put_nowait(sse_data)
        except asyncio.QueueFull:
            dead_queues.add(q)
        except Exception:
            dead_queues.add(q)
            
    for dq in dead_queues:
        sse_clients.discard(dq)


# ==========================================================
# 1. ESP32 WebSocket Ingestion Endpoint (/ws)
# ==========================================================
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    ws_clients.add(websocket)
    print(f"[WS CLIENT] ESP32 Connected from {websocket.client.host}:{websocket.client.port}")

    try:
        while True:
            text_data = await websocket.receive_text()
            try:
                payload = json.loads(text_data)
                await broadcast_to_sse(payload)
                await websocket.send_text(json.dumps({"status": "ok", "seq": payload.get("seq", 0)}))
            except json.JSONDecodeError:
                pass
    except WebSocketDisconnect:
        print("[WS CLIENT] ESP32 Disconnected")
    finally:
        ws_clients.discard(websocket)


# ==========================================================
# 2. ESP32 HTTP POST Ingestion Fallback (/api/ecg)
# ==========================================================
@app.post("/api/ecg")
async def handle_post_ecg(request: Request):
    try:
        body = await request.body()
        payload = json.loads(body)
        await broadcast_to_sse(payload)
        return JSONResponse({"status": "ok", "seq": payload.get("seq", 0)})
    except Exception as e:
        return JSONResponse({"error": str(e)}, status_code=400)


# ==========================================================
# 3. Live SSE Stream for ecg_dashboard.html & ecgOld.py
# ==========================================================
@app.get("/api/ecg/live/{device_id}")
async def sse_live_stream(device_id: str):
    queue = asyncio.Queue(maxsize=100)
    sse_clients.add(queue)
    print(f"[SSE CLIENT] Dashboard / Viewer connected for device: {device_id}")

    async def event_generator():
        yield f"data: {json.dumps({'status': 'connected', 'deviceId': device_id})}\n\n"
        try:
            while True:
                data = await queue.get()
                yield data
        except asyncio.CancelledError:
            pass
        finally:
            sse_clients.discard(queue)
            print(f"[SSE CLIENT] Disconnected: {device_id}")

    return StreamingResponse(event_generator(), media_type="text/event-stream")


# ==========================================================
# 4. Device Results Endpoint
# ==========================================================
@app.post("/api/ecg/device_result")
async def post_device_result(request: Request):
    try:
        body = await request.body()
        data = json.loads(body)
        device_id = data.get("deviceId", "ESP_ECG_123")
        device_results[device_id] = data
        return JSONResponse({"status": "ok"})
    except Exception as e:
        return JSONResponse({"error": str(e)}, status_code=400)


@app.get("/api/ecg/device_result")
async def get_device_result():
    return JSONResponse(device_results)


# ==========================================================
# 5. Serve ecg_dashboard.html at Root URL
# ==========================================================
@app.get("/")
async def serve_dashboard():
    try:
        with open("ecg_dashboard.html", "r", encoding="utf-8") as f:
            content = f.read()
        return HTMLResponse(content)
    except Exception:
        return HTMLResponse("<h1>Private Offline ECG Server Running</h1><p>Open ecg_dashboard.html</p>")


if __name__ == "__main__":
    local_ip = get_local_ip()
    print("=" * 70)
    print(">> PRIVATE OFFLINE ECG SERVER INITIALIZED")
    print("=" * 70)
    print(f"[*] Local LAN IP      : http://{local_ip}:8000")
    print(f"[*] Localhost URL     : http://localhost:8000")
    print(f"[*] Dashboard URL     : http://localhost:8000/")
    print(f"[*] Live SSE Endpoint : http://localhost:8000/api/ecg/live/ESP_ECG_123")
    print(f"[*] WebSocket Endpoint: ws://{local_ip}:8000/ws")
    print("=" * 70)
    print("-> Set API_HOST in include/config.h to your PC's IP or connect via Localhost!")
    print("=" * 70)

    uvicorn.run(app, host="0.0.0.0", port=8000, log_level="warning", ws="websockets")


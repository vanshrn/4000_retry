# 📡 ESP32 2000 SPS Bluetooth (BLE) Data Flow & Flutter Integration Guide

This guide documents the complete **BLE protocol**, **binary packet format (RAW + FILTERED + METRICS + DIAGNOSIS)**, **chunking mechanism**, **ready-to-use Flutter integration code**, and **Cloud/Server JSON Upload specifications** for streaming real-time **2000 SPS ECG data** from Seeed Studio XIAO ESP32-S3 (ADS1292R).

---

## 1. BLE Protocol Specifications

* **Device Name**: `ECG_Setup`
* **Primary Service UUID**: `12345678-1234-5678-1234-56789abcdef0`
* **ECG Data Characteristic UUID**: `12345678-1234-5678-1234-56789abcdef1` (Properties: `NOTIFY` | `READ`)
* **Status & Wi-Fi Provisioning Characteristic UUID**: `12345678-1234-5678-1234-56789abcdef2` (Properties: `NOTIFY` | `READ` | `WRITE` | `WRITE_NR`)

---

## 2. Complete Binary Packet Structure (2000 SPS)

Total Block Size = **16,098 bytes** per 1-second block.

| Offset (Bytes) | Field Name | Data Type | Endianness | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0..3` | `seq` | `uint32_t` (4 bytes) | Little Endian | Block sequence counter |
| `4..5` | `sampleRate` | `uint16_t` (2 bytes) | Little Endian | Sampling frequency (`2000`) |
| `6` | `flags` | `uint8_t` (1 byte) | - | Bit 0: `leadsOff`, Bit 1: `loPlus`, Bit 2: `loMinus` |
| `7` | `severity` | `uint8_t` (1 byte) | - | `0` = INFO, `1` = WARNING, `2` = CRITICAL |
| `8..9` | `numSamples` | `uint16_t` (2 bytes) | Little Endian | Number of samples in block (`2000`) |
| `10..57` | `deviceResult` | `char[48]` (48 bytes) | ASCII / UTF-8 | Diagnosis (e.g. *"Normal Sinus Rhythm \| 90% \| 75 bpm"*) |
| `58..61` | `snrDb` | `float` (4 bytes) | Little Endian | Signal-to-Noise Ratio (dB) |
| `62..65` | `snrAccuracy` | `float` (4 bytes) | Little Endian | SNR Accuracy (%) |
| `66..69` | `rPeakAccuracy` | `float` (4 bytes) | Little Endian | R-Peak Detection Accuracy (%) |
| `70..73` | `hrBpm` | `float` (4 bytes) | Little Endian | **Heart Rate (BPM)** |
| `74..77` | `hrAccuracy` | `float` (4 bytes) | Little Endian | Heart Rate Accuracy (%) |
| `78..81` | `baselineWanderMv` | `float` (4 bytes) | Little Endian | Baseline Wander (mV) |
| `82..85` | `baselineAccuracy` | `float` (4 bytes) | Little Endian | Baseline Accuracy (%) |
| `86..89` | `motionArtifactIndex`| `float` (4 bytes) | Little Endian | Motion Artifact Index |
| `90..93` | `motionAccuracy` | `float` (4 bytes) | Little Endian | Motion Accuracy (%) |
| `94..97` | `cmrrEstDb` | `float` (4 bytes) | Little Endian | CMRR Estimate (dB) |
| `98..8097` | `raw_samples` | `int32_t[2000]` (8000B) | Little Endian | **2000 Raw 24-bit ADC points** |
| `8098..16097`| `filtered_samples`| `int32_t[2000]` (8000B) | Little Endian | **2000 Clean DSP-Filtered ECG points** |

---

## 3. BLE MTU Chunking & Reassembly (34 Chunks)

To prevent mobile BLE stack packet drops, the ESP32 splits the 16,098-byte block into **34 chunks** (capped at 480 bytes per notification with 20ms pacing):

Each BLE Notification packet contains a **6-byte chunk header**:

```
+----------------+------------------+--------------------+-------------------------+
| seq (4 bytes)  | chunkIdx (1 byte)| totalChunks (1 byte)| Raw Data Slice (N bytes)|
+----------------+------------------+--------------------+-------------------------+
```

* Chunks `0` to `32`: **480 bytes** (6 bytes header + 474 bytes data)
* Chunk `33` (34th chunk): **462 bytes** (6 bytes header + 456 bytes data)
* Total Chunks = **34 chunks**

---

## 4. Flutter Integration (Ready-to-Use Code)

### A. Dependencies (`pubspec.yaml`)
```yaml
dependencies:
  flutter:
    sdk: flutter
  flutter_blue_plus: ^1.34.5
  http: ^1.2.0
  web_socket_channel: ^3.0.1
```

---

### B. Complete Dart BLE Manager (`ecg_2000_ble_manager.dart`)

```dart
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class EcgMetrics {
  final double snrDb;
  final double snrAccuracy;
  final double rPeakAccuracy;
  final double hrBpm;
  final double hrAccuracy;
  final double baselineWanderMv;
  final double baselineAccuracy;
  final double motionArtifactIndex;
  final double motionAccuracy;
  final double cmrrEstDb;

  EcgMetrics({
    required this.snrDb,
    required this.snrAccuracy,
    required this.rPeakAccuracy,
    required this.hrBpm,
    required this.hrAccuracy,
    required this.baselineWanderMv,
    required this.baselineAccuracy,
    required this.motionArtifactIndex,
    required this.motionAccuracy,
    required this.cmrrEstDb,
  });

  Map<String, dynamic> toJson() => {
    "snrDb": double.parse(snrDb.toStringAsFixed(2)),
    "snrAccuracy": double.parse(snrAccuracy.toStringAsFixed(1)),
    "rPeakAccuracy": double.parse(rPeakAccuracy.toStringAsFixed(1)),
    "hrBpm": double.parse(hrBpm.toStringAsFixed(1)),
    "hrAccuracy": double.parse(hrAccuracy.toStringAsFixed(1)),
    "baselineWanderMv": double.parse(baselineWanderMv.toStringAsFixed(2)),
    "baselineAccuracy": double.parse(baselineAccuracy.toStringAsFixed(1)),
    "motionArtifactIndex": double.parse(motionArtifactIndex.toStringAsFixed(2)),
    "motionAccuracy": double.parse(motionAccuracy.toStringAsFixed(1)),
    "cmrrEstDb": double.parse(cmrrEstDb.toStringAsFixed(1)),
  };
}

class EcgBlock {
  final int seq;
  final int sampleRate; // 2000
  final bool leadsOff;
  final bool loPlus;
  final bool loMinus;
  final int severity;
  final String deviceResult;
  final EcgMetrics metrics;
  final List<int> rawSamples;      // 2000 raw ADC points
  final List<int> filteredSamples; // 2000 filtered ECG points

  EcgBlock({
    required this.seq,
    required this.sampleRate,
    required this.leadsOff,
    required this.loPlus,
    required this.loMinus,
    required this.severity,
    required this.deviceResult,
    required this.metrics,
    required this.rawSamples,
    required this.filteredSamples,
  });

  String get severityString {
    switch (severity) {
      case 2: return "CRITICAL";
      case 1: return "WARNING";
      default: return "INFO";
    }
  }

  /// Converts the BLE block into the exact JSON schema required by Cloud / Backend
  Map<String, dynamic> toCloudJson({
    required String userId,
    required String deviceId,
    String mode = "ble",
  }) {
    // Convert parallel raw and filtered arrays into 2D sample pairs: [[raw0, filt0], [raw1, filt1], ...]
    List<List<int>> dataPairs = [];
    int count = rawSamples.length < filteredSamples.length 
        ? rawSamples.length 
        : filteredSamples.length;

    for (int i = 0; i < count; i++) {
      dataPairs.add([rawSamples[i], filteredSamples[i]]);
    }

    return {
      "userId": userId,
      "deviceId": deviceId,
      "mode": mode,
      "seq": seq,
      "sr": sampleRate,
      "lo": leadsOff,
      "loPlus": loPlus,
      "loMinus": loMinus,
      "severity": severityString,
      "device_result": deviceResult,
      "metrics": metrics.toJson(),
      "data": dataPairs,
    };
  }
}

class Ecg2000BleManager {
  static const String SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0";
  static const String ECG_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef1";
  static const String STATUS_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef2";

  BluetoothDevice? targetDevice;
  BluetoothCharacteristic? ecgCharacteristic;
  BluetoothCharacteristic? statusCharacteristic;

  // Stream controllers for UI
  final _ecgStreamController = StreamController<EcgBlock>.broadcast();
  Stream<EcgBlock> get ecgStream => _ecgStreamController.stream;

  final _statusStreamController = StreamController<String>.broadcast();
  Stream<String> get statusStream => _statusStreamController.stream;

  // Reassembly buffers
  int _currentSeq = -1;
  int _totalChunks = 0;
  final Map<int, Uint8List> _chunks = {};

  /// 1. Connect to ECG_Setup and request MTU 512
  Future<void> connect(BluetoothDevice device) async {
    targetDevice = device;
    await device.connect(autoConnect: false);

    // Request MTU 512 for maximum throughput
    await device.requestMtu(512);

    List<BluetoothService> services = await device.discoverServices();
    for (var service in services) {
      if (service.uuid.toString().toLowerCase() == SERVICE_UUID.toLowerCase()) {
        for (var char in service.characteristics) {
          if (char.uuid.toString().toLowerCase() == ECG_CHAR_UUID.toLowerCase()) {
            ecgCharacteristic = char;
            await char.setNotifyValue(true);
            // Use onValueReceived (NOT lastValueStream) to guarantee every chunk is received
            char.onValueReceived.listen(_onEcgDataReceived);
          } else if (char.uuid.toString().toLowerCase() == STATUS_CHAR_UUID.toLowerCase()) {
            statusCharacteristic = char;
            await char.setNotifyValue(true);
            char.onValueReceived.listen((bytes) {
              String msg = utf8.decode(bytes);
              _statusStreamController.add(msg);
            });
          }
        }
      }
    }
  }

  /// 2. Chunk Reassembly Handler
  void _onEcgDataReceived(List<int> rawBytes) {
    if (rawBytes.length < 6) return;
    Uint8List data = Uint8List.fromList(rawBytes);
    ByteData byteData = ByteData.sublistView(data);

    int seq = byteData.getUint32(0, Endian.little);
    int chunkIdx = byteData.getUint8(4);
    int totalChunks = byteData.getUint8(5);

    // New block received -> reset buffer
    if (_currentSeq != seq) {
      _currentSeq = seq;
      _totalChunks = totalChunks;
      _chunks.clear();
    }

    _chunks[chunkIdx] = data.sublist(6);

    // All chunks received (34 chunks) -> parse full block
    if (_chunks.length == _totalChunks && _totalChunks > 0) {
      _parseFullPayload(_currentSeq);
      _chunks.clear();
    }
  }

  /// 3. Parse Full 16098-byte Packet into Metadata + Metrics + 2000 RAW + 2000 FILTERED Samples
  void _parseFullPayload(int seq) {
    List<int> fullBytes = [];
    for (int i = 0; i < _totalChunks; i++) {
      if (_chunks.containsKey(i)) {
        fullBytes.addAll(_chunks[i]!);
      } else {
        return; // Incomplete block, drop safely
      }
    }

    Uint8List fullPayload = Uint8List.fromList(fullBytes);
    if (fullPayload.length < 98) return;

    ByteData bd = ByteData.sublistView(fullPayload);
    int pSeq = bd.getUint32(0, Endian.little);
    int sampleRate = bd.getUint16(4, Endian.little); // 2000
    int flags = bd.getUint8(6);
    int severity = bd.getUint8(7);
    int numSamples = bd.getUint16(8, Endian.little); // 2000

    bool leadsOff = (flags & 0x01) != 0;
    bool loPlus   = (flags & 0x02) != 0;
    bool loMinus  = (flags & 0x04) != 0;

    // Parse Device Result String (48 bytes null-terminated)
    List<int> resultBytes = fullPayload.sublist(10, 58);
    int nullIdx = resultBytes.indexOf(0);
    if (nullIdx >= 0) resultBytes = resultBytes.sublist(0, nullIdx);
    String deviceResult = utf8.decode(resultBytes, allowMalformed: true);

    // Parse 10 Performance Metrics (40 bytes)
    EcgMetrics metrics = EcgMetrics(
      snrDb: bd.getFloat32(58, Endian.little),
      snrAccuracy: bd.getFloat32(62, Endian.little),
      rPeakAccuracy: bd.getFloat32(66, Endian.little),
      hrBpm: bd.getFloat32(70, Endian.little),
      hrAccuracy: bd.getFloat32(74, Endian.little),
      baselineWanderMv: bd.getFloat32(78, Endian.little),
      baselineAccuracy: bd.getFloat32(82, Endian.little),
      motionArtifactIndex: bd.getFloat32(86, Endian.little),
      motionAccuracy: bd.getFloat32(90, Endian.little),
      cmrrEstDb: bd.getFloat32(94, Endian.little),
    );

    List<int> rawSamples = [];
    List<int> filteredSamples = [];

    // Parse 2000 RAW samples (Offset 98)
    int offset = 98;
    for (int i = 0; i < numSamples; i++) {
      if (offset + 4 <= fullPayload.length) {
        rawSamples.add(bd.getInt32(offset, Endian.little));
        offset += 4;
      }
    }

    // Parse 2000 FILTERED samples
    for (int i = 0; i < numSamples; i++) {
      if (offset + 4 <= fullPayload.length) {
        filteredSamples.add(bd.getInt32(offset, Endian.little));
        offset += 4;
      }
    }

    EcgBlock block = EcgBlock(
      seq: pSeq,
      sampleRate: sampleRate,
      leadsOff: leadsOff,
      loPlus: loPlus,
      loMinus: loMinus,
      severity: severity,
      deviceResult: deviceResult,
      metrics: metrics,
      rawSamples: rawSamples,           // 2000 raw points
      filteredSamples: filteredSamples, // 2000 filtered points
    );

    _ecgStreamController.add(block);
  }

  /// 4. Wi-Fi Provisioning over BLE
  Future<void> sendWifiCredentials({
    required String ssid,
    required String password,
    String? deviceId,
    String? userId,
  }) async {
    if (statusCharacteristic == null) return;

    Map<String, dynamic> json = {
      "ssid": ssid,
      "password": password,
      "deviceId": deviceId ?? "ESP_ECG_123",
      "userId": userId ?? "ESP_ECG_123",
    };

    String payload = jsonEncode(json);
    await statusCharacteristic!.write(utf8.encode(payload), withoutResponse: false);
  }

  void dispose() {
    _ecgStreamController.close();
    _statusStreamController.close();
  }
}
```

---

## 5. Cloud & Server JSON Payload Specifications

When streaming from Mobile to Cloud or Backend, use the exact same schema that ESP32 sends over Wi-Fi:

### A. JSON Payload Format

```json
{
  "userId": "ESP_ECG_123",
  "deviceId": "ESP_ECG_123",
  "mode": "ble",
  "seq": 115,
  "sr": 2000,
  "lo": false,
  "loPlus": false,
  "loMinus": false,
  "severity": "INFO",
  "device_result": "Normal Sinus Rhythm | 90% | 75 bpm",
  "metrics": {
    "snrDb": 18.52,
    "snrAccuracy": 92.4,
    "rPeakAccuracy": 98.1,
    "hrBpm": 75.0,
    "hrAccuracy": 95.0,
    "baselineWanderMv": 0.08,
    "baselineAccuracy": 96.2,
    "motionArtifactIndex": 0.02,
    "motionAccuracy": 99.0,
    "cmrrEstDb": 86.0
  },
  "data": [
    [raw_0, filtered_0],
    [raw_1, filtered_1],
    ...
    [raw_1999, filtered_1999]
  ]
}
```

---

### B. Mobile-to-Cloud Upload Service (`ecg_cloud_service.dart`)

```dart
import 'dart:convert';
import 'package:http/http.dart' as http;
import 'package:web_socket_channel/web_socket_channel.dart';
import 'ecg_2000_ble_manager.dart';

class EcgCloudService {
  static const String HTTP_UPLOAD_URL = "https://ads1292r-code-91eg.onrender.com/api/ecg";
  static const String WS_STREAM_URL   = "wss://ads1292r-code.onrender.com/ws";

  WebSocketChannel? _wsChannel;

  /// 1. Initialize Real-Time WebSocket Streaming to Cloud
  void initWebSocketStream() {
    try {
      _wsChannel = WebSocketChannel.connect(Uri.parse(WS_STREAM_URL));
      print("[CLOUD WS] Connected to $WS_STREAM_URL");
    } catch (e) {
      print("[CLOUD WS ERROR] $e");
    }
  }

  /// 2. Stream Block to Cloud via WebSocket (Lowest Latency)
  void streamBlockToWebSocket(EcgBlock block, {required String userId, required String deviceId}) {
    if (_wsChannel == null) return;
    try {
      Map<String, dynamic> jsonPayload = block.toCloudJson(
        userId: userId,
        deviceId: deviceId,
        mode: "ble_mobile_relay",
      );
      _wsChannel!.sink.add(jsonEncode(jsonPayload));
    } catch (e) {
      print("[CLOUD WS SEND ERROR] $e");
    }
  }

  /// 3. Upload Block to Cloud via HTTP POST (Rest API)
  Future<bool> postBlockHttp(EcgBlock block, {required String userId, required String deviceId}) async {
    try {
      Map<String, dynamic> jsonPayload = block.toCloudJson(
        userId: userId,
        deviceId: deviceId,
        mode: "ble_mobile_relay",
      );

      final response = await http.post(
        Uri.parse(HTTP_UPLOAD_URL),
        headers: {"Content-Type": "application/json"},
        body: jsonEncode(jsonPayload),
      );

      return response.statusCode == 200 || response.statusCode == 201;
    } catch (e) {
      print("[CLOUD HTTP ERROR] $e");
      return false;
    }
  }

  void dispose() {
    _wsChannel?.sink.close();
  }
}
```

---

## 6. Wi-Fi Provisioning JSON Format over BLE

To configure Wi-Fi credentials over Bluetooth, write JSON UTF-8 bytes to `STATUS_CHAR_UUID`:

```json
{
  "ssid": "MyHotspotName",
  "password": "MyHotspotPassword",
  "deviceId": "ESP_ECG_123",
  "userId": "ESP_ECG_123"
}
```

### Response from Device:
```json
{
  "status": "ok",
  "message": "WiFi credentials saved via BLE",
  "ssid": "MyHotspotName"
}
```
When Wi-Fi connects, the device pushes:
```json
{
  "status": "wifi_connected",
  "ip": "192.168.43.15",
  "deviceId": "ESP_ECG_123"
}
```

# MapTrack

**A cellular-only GPS tracker and field camera unit — no Wi-Fi, no paired phone.**

MapTrack is an ESP32-S3-based field device that logs GPS location on a schedule and captures on-demand photographs, uploading both over a 4G cellular connection to a live map dashboard. It was built end-to-end — research, hardware integration, firmware, backend, and dashboard — for a Tata Power field-asset tracking use case.

---

## Features

- 📍 **Cellular-only GPS logging** — no Wi-Fi dependency, works anywhere with cell coverage
- 📷 **On-demand photo capture** — 5MP camera with an on-screen preview, verify, upload/delete flow
- 🗺️ **Live map dashboard** — real-time position and full GPS trail, built with MapLibre
- 🔋 **Battery-powered** — Li-ion/LiPo charge management for standalone field deployment
- 🔁 **Resilient GPS reporting** — if a fix can't be acquired in the acquisition window, a placeholder row is still logged so the timeline never goes silent
- 🖥️ **Two-button physical interface** — wake/sleep and capture, with an on-screen state machine (Sleep → Awake → Preview → Verify)

---

## Hardware

| Component | Specification |
|---|---|
| ESP32-S3 2" Display Development Board | 240×320 px display, 32-bit dual-core LX7, onboard Wi-Fi & Bluetooth, onboard camera interface |
| SIM7600X 4G Communication Module | Multi-band 4G/3G/2G modem with integrated GNSS positioning |
| OV5640 Camera Board (A) | 5MP sensor, 2592×1944 max resolution |
| DFRobot Lithium Battery Charger V1.0 | Single-cell Li-ion/LiPo charge management |
| GNSS + cellular antennas | External antennas for positioning and data link |

---

## Architecture

```
┌─────────────┐     4G/HTTPS      ┌──────────────────┐
│  MapTrack   │ ────────────────▶ │  Flask Gallery    │
│  Device     │                   │  Server (images)  │
│ (ESP32-S3 + │                   └──────────────────┘
│  SIM7600X)  │     4G/HTTPS      ┌──────────────────┐
│             │ ────────────────▶ │ Google Sheets via │
└─────────────┘                   │ Apps Script (GPS) │
                                   └────────┬─────────┘
                                            │
                                            ▼
                                   ┌──────────────────┐
                                   │  MapLibre Live    │
                                   │  Dashboard         │
                                   └──────────────────┘
```

- **Firmware** — C++ (Arduino framework) running on the ESP32-S3, using TinyGSM for AT-command modem control, the ESP32 camera driver, and Arduino_GFX for the onboard display.
- **Image backend** — Flask server hosting an in-memory image gallery with expiry. Deployed on a **persistent host** (not serverless — the design relies on in-memory state and a background expiry thread, which serverless platforms don't support).
- **Location backend** — Google Sheets, written to via a Google Apps Script web endpoint. Acts as a zero-cost, append-only log of timestamped coordinates.
- **Dashboard** — Static MapLibre map reading directly from the Google Sheet, deployed on Vercel.

---

## Firmware State Machine

| State | Behavior |
|---|---|
| `Sleep` | Display and non-essential peripherals off; short press on wake button resumes |
| `Awake` | Idle state — shows modem status and last known GPS fix |
| `Preview` | Live camera feed shown after a single press of the capture button |
| `Verify` | Captured photo held on screen; short press uploads, long press (4s) deletes |

GNSS acquisition runs on its own cycle, independent of the UI: the modem's GNSS engine is enabled on a fixed interval and given a bounded window to get a fix. If the window elapses with no fix, a placeholder coordinate is still pushed to the sheet so the device never goes silent on the dashboard.

---

## Getting Started

### Prerequisites
- Arduino IDE with the ESP32 board package installed
- Libraries: `TinyGSM`, `ArduinoJson`, `Arduino_GFX_Library`, ESP32 camera driver (`esp_camera.h`)
- **PSRAM must be enabled** — set `Tools → PSRAM → OPI PSRAM` for this board; camera init will fail with a frame-buffer allocation error otherwise

### Firmware Setup
1. Open the `.ino` file in Arduino IDE
2. Set your APN in the `apn` / `apnUser` / `apnPass` fields
3. Point `GALLERY_HOST` at your deployed Flask backend (must be a persistent host — see Architecture above)
4. Point `scriptHost` / `scriptPath` at your deployed Google Apps Script web app URL
5. Flash to the ESP32-S3

### Backend Setup
1. Deploy `gallery_server.py` (Flask) to a persistent host (e.g. Render, a VPS, PythonAnywhere)
2. Deploy the Apps Script project behind a Google Sheet and publish it as a web app
3. Deploy the dashboard's static files (e.g. to Vercel) pointed at the same Sheet

---

## Known Issues Resolved

| Issue | Fix |
|---|---|
| Multi-hop image upload failure (early prototype) | Traced and corrected the broken hop across device → server → storage |
| Captured images missing GPS metadata | Tag uploads with the most recent GPS fix at capture time |
| GPIO conflict on modem UART | Re-mapped conflicting pins between display, camera, and modem |
| Flask backend timezone bug | Corrected timestamp handling to match device local time |
| Gallery backend unreachable on serverless deploy | Moved to a persistent host — serverless instances don't share the in-memory state this backend relies on |
| Camera init failing with frame-buffer malloc error | Added PSRAM diagnostics at boot; root cause was PSRAM not enabled in board config |
| Silent GPS logging when no fix available | Added a bounded acquisition window with a `0.000, 0.000` placeholder push on timeout |

---

## Roadmap

- [ ] Field trial under real outdoor conditions
- [ ] Enclosure design and battery-life characterization
- [ ] Refined two-button interface for the next hardware revision
- [ ] Authentication on dashboard and upload endpoints

---

## Project Background

Built during an internship at **PNT Robotics & Automation**, developed from initial research through to a working prototype for a Tata Power field-tracking use case. An earlier proof-of-concept used an ESP32-CAM module before migrating to the current ESP32-S3 + SIM7600X hardware.

---

# Flexy-Meter

A DIY smart electricity meter reader: reads P1 telemetry from a **Meter&Control Flexy F2** meter (DSMR V5.0 / DLMS-COSEM protocol) over its P1/RJ12 port, sends it over **LoRaWAN**, and stores/visualizes it with a self-hosted **ChirpStack → InfluxDB → Grafana** pipeline running on a Synology NAS.

Originally built to monitor overall household power/energy per phase (Swiss three-phase supply), and now extended to detect appliance events (e.g. a washing machine finishing its cycle) from per-phase current changes.

## How it works

```
Flexy F2 meter (P1 port)
        │  DLMS/COSEM telegram over UART (via BS170 inverter circuit)
        ▼
Heltec CubeCell HTCC-AB02A
        │  parses telegram → packs binary payload → sends over LoRaWAN (OTAA, CN470)
        ▼
LoRaWAN Gateway
        │
        ▼
ChirpStack v4  (Docker, on Synology NAS)
        │  decodes payload (JS codec) → forwards to InfluxDB
        ▼
InfluxDB v2  (Docker, on Synology NAS)  —  bucket: flexymeter
        │
        ▼
Grafana  (Docker, on Synology NAS)  —  dashboards: per-phase current, per-phase voltage, energy
```

## Hardware

- **Meter**: Meter&Control Flexy F2 (DSMR V5.0, DLMS/COSEM), read via its P1 RJ12 port
- **MCU**: Heltec CubeCell HTCC-AB02A
- **Interface circuit**: BS170 MOSFET inverter between the meter's P1 output and the CubeCell UART
  - DATA → Gate (10kΩ pull-up to +5V)
  - Drain → UART_RX2
  - Source → GND
- **Network**: LoRaWAN, CN470 region, OTAA activation, via a gateway feeding ChirpStack
- **Schematic**: see [`p1_Flexy_Meter.fzz`](./p1_Flexy_Meter.fzz) (Fritzing file) for the full interface circuit

## Payload format

The CubeCell parses the DLMS/COSEM telegram and packs a compact binary payload (currently 54 bytes) containing:

- Power import/export (W)
- Cumulative energy import/export, and per-tariff (T1/T2, HP/HC) energy (Wh)
- Per-phase voltage L1/L2/L3 (V)
- Per-phase current L1/L2/L3 (×0.01 A)
- A high-resolution window summary for appliance-event detection: min/max current on L1 seen since the last transmission, a count of sharp current drops (a likely sign an appliance just stopped), and the time offset of the most recent drop

Between the normal 15-minute transmissions, the CubeCell wakes briefly every 30s to sample the meter and update these stats, so short events aren't missed even though the device only transmits once per 15 minutes.

The matching decode logic is deployed as a JavaScript payload codec on ChirpStack.

## Repository contents

| File | Purpose |
|---|---|
| `p1_flexy_parser` | Standalone sketch: reads and parses the DLMS/COSEM P1 telegram, prints the decoded values over serial — use this first to confirm the interface circuit and parsing work before adding LoRaWAN |
| `p1_meter_lorawan` | Base sketch: parser + LoRaWAN send, without the appliance-detection logic — the starting point once the parser is confirmed working |
| `p1_lorawan_washingmachine_detection_v2.ino` | CubeCell firmware: DLMS/COSEM parsing, high-res sampling, LoRaWAN payload build/send, plus appliance-event detection (**still in development**) |
| `secrets.h.example` | Template for LoRaWAN credentials — copy and fill in your own (see below) |
| `.gitignore` | Excludes your real credentials from being committed |
| `p1_Flexy_Meter.fzz` | Fritzing schematic for the P1-to-UART interface circuit |

## Getting started — recommended order

This repo contains three sketches, meant to be tried in this order:

1. **`p1_flexy_parser`** — confirms your interface circuit and DLMS/COSEM parsing are working, by printing decoded meter values over serial. Start here.
2. **`p1_meter_lorawan`** — adds LoRaWAN OTAA join and sends the parsed values once the parser is confirmed working. This is the base firmware for hooking your meter into ChirpStack/InfluxDB/Grafana.
3. **`p1_lorawan_washingmachine_detection_v2.ino`** — adds the high-resolution appliance-event detection on top of the base firmware. This part is **still in development**, especially isolating one appliance's signal when others share the same phase — use `p1_meter_lorawan` if you just want reliable metering.

## Setup

### 1. Firmware credentials

This project keeps LoRaWAN credentials (`devEui`, `appEui`, `appKey`) out of the committed code. Two options, pick one:

- **Simplest**: copy `secrets.h.example` to `secrets.h` in this same folder, fill in your device's real EUIs/key from the ChirpStack console, and keep `#include "secrets.h"` in the `.ino`. `secrets.h` is already gitignored.
- **Shared across multiple sketches**: put `secrets.h` in your Arduino libraries folder (e.g. `~/Arduino/libraries/Secrets/secrets.h`) and use `#include <secrets.h>` instead. This keeps it out of every sketch folder, so no `.gitignore` entry is needed for it.

Never commit a `secrets.h` with real values — if one ever gets pushed, treat the key as compromised and regenerate it in ChirpStack.

### 2. Flash the CubeCell

Open the `.ino` in the Arduino IDE with the Heltec CubeCell board package installed, select the HTCC-AB02A board, and upload.

### 3. Backend: ChirpStack, InfluxDB, Grafana on Synology NAS

The backend runs as Docker containers on a Synology NAS, managed through **Container Manager** (Synology's Docker/Compose app). At a high level:

1. Install **Container Manager** from Synology Package Center.
2. Set up a `docker-compose.yml` project (in Container Manager, or via SSH if you prefer the command line) defining the ChirpStack v4 stack (ChirpStack server, PostgreSQL, Redis, Mosquitto/MQTT), InfluxDB v2, and Grafana as services.
3. Point the ChirpStack InfluxDB integration at your InfluxDB instance using the **full write API path** (`http://<host>:8086/api/v2/write`, not just the host) — pointing at just the host fails silently.
4. In ChirpStack, register the device (OTAA, CN470), set the correct channel mask for your gateway, and upload the JS payload codec.
5. In InfluxDB, create a bucket (e.g. `flexymeter`) for the incoming telemetry.
6. In Grafana, add InfluxDB as a data source and build dashboards (current/voltage per phase, energy totals).

> A Synology NAS running Docker works well for this because it keeps ChirpStack, InfluxDB, and Grafana running 24/7 on existing home-server hardware, with Container Manager handling the Compose stack through a GUI instead of a bare command line.

### 4. Appliance-event detection (optional)

The high-res window stats (min/max current, drop-event count) in the payload are designed to help flag events like a washing machine stopping — from Grafana or via automation reading the InfluxDB bucket (e.g. triggering an [ntfy.sh](https://ntfy.sh) notification). This part is still evolving, especially for isolating one appliance's signal when multiple appliances share the same phase.

## Notes / known limitations

- Devices on the same phase as the appliance you're trying to detect (e.g. dishwasher, heat pump, EV charger) can make it hard to isolate a single appliance's signal from combined per-phase current.
- RF link quality can cause intermittent uplink gaps; pinning a fixed data rate/spreading factor (rather than relying on ADR) can help in weak-signal locations.

## Credits

The hardware design — the P1-to-UART interface circuit and overall CubeCell wiring — is essentially a copy of [maxcharlier/p1-meter-lorawan](https://github.com/maxcharlier/p1-meter-lorawan). This project builds on that hardware groundwork, adapting the firmware for the Flexy F2 meter's payload and adding the high-resolution appliance-event detection logic. Many thanks to maxcharlier for the original work.

## Useful links

https://icube.ch/obishelper/obishelper.html

// ============================================================
// P1 DLMS/COSEM Parser + LoRaWAN — Meter&Control Flexy F2
// Romande Energie configuration
// Target: Heltec CubeCell HTCC-AB02A
// Network: ChirpStack v4, CN470, OTAA
// ============================================================
// Payload format (38 bytes total):
//   [0..3]   uint32  power import (W)
//   [4..7]   uint32  power export (W)
//   [8..11]  uint32  energy import total (Wh)
//   [12..15] uint32  energy export total (Wh)
//   [16..17] uint16  voltage L1 (V)
//   [18..19] uint16  voltage L2 (V)
//   [20..21] uint16  voltage L3 (V)
//   [22..23] uint16  current L1 (×0.01 A)
//   [24..25] uint16  current L2 (×0.01 A)
//   [26..27] uint16  current L3 (×0.01 A)
//   [28..31] uint32  energy import T1 HP (Wh)
//   [32..35] uint32  energy import T2 HC (Wh)
//   [36..39] uint32  energy export T1 HP (Wh)
//   [40..43] uint32  energy export T2 HC (Wh)
//   [44..45] uint16  min current L1 seen this window (x0.01 A)
//   [46..47] uint16  max current L1 seen this window (x0.01 A)
//   [48..49] uint16  drop-event count this window (appliance likely stopped)
//   [50..53] uint32  seconds-into-window of the most recent drop event
// ============================================================
// High-resolution monitoring: between the normal 15-min sends, the
// CubeCell wakes briefly every SAMPLE_INTERVAL_MS to read one telegram
// and update the stats above, so short events (like a washing machine's
// spin-cycle stopping) aren't missed even though we still only transmit
// once per 15 minutes.
// ============================================================

#include "LoRaWan_APP.h"
#include "Arduino.h"


// ---- LoRaWAN credentials (match ChirpStack registration) ---
// Real values live in secrets.h, which is gitignored and never
// committed. Copy secrets.h.example to secrets.h and fill in your
// own devEui/appEui/appKey from the ChirpStack console.
#include <secrets.h>

// ---- LoRaWAN config --------------------------------------------
uint16_t userChannelsMask[6] = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x00FF };
LoRaMacRegion_t loraWanRegion = LORAMAC_REGION_CN470;
DeviceClass_t   loraWanClass  = CLASS_A;
uint32_t appTxDutyCycle       = 15 * 60 * 1000;  // 15 minutes in ms
bool overTheAirActivation     = true;
bool loraWanAdr               = false;
bool keepNet                  = true;
bool isTxConfirmed            = false;
uint8_t appPort               = 2;
uint8_t confirmedNbTrials     = 8;

// ---- UART / frame buffer ---------------------------------------
#define RX_BUFFER_SIZE 512
uint8_t  frameBuffer[RX_BUFFER_SIZE];
uint16_t frameLen = 0;
bool     inFrame  = false;

// ---- Parsed meter values ---------------------------------------
char     serialNumber[16] = "";
uint32_t powerImport      = 0;
uint32_t powerExport      = 0;
uint32_t energyImportTotal= 0;
uint32_t energyExportTotal= 0;
uint16_t voltageL1        = 0;
uint16_t voltageL2        = 0;
uint16_t voltageL3        = 0;
uint16_t currentL1        = 0;
uint16_t currentL2        = 0;
uint16_t currentL3        = 0;
uint32_t energyImportT1   = 0;
uint32_t energyImportT2   = 0;
uint32_t energyExportT1   = 0;
uint32_t energyExportT2   = 0;
bool     newDataReady     = false;

// ---- High-resolution window stats (reset every TX cycle) -------
#define SAMPLE_INTERVAL_MS   30000UL   // read the meter every 30s between sends
#define DROP_THRESHOLD       150       // 1.50 A single-step drop -> "something stopped"

uint16_t minCurrentL1      = 0xFFFF;
uint16_t maxCurrentL1      = 0;
uint16_t lastCurrentL1     = 0;
uint16_t dropEventCount    = 0;
uint32_t lastDropOffsetSec = 0;
uint32_t windowStartMs     = 0;

TimerEvent_t sampleTimer;
volatile bool sampleDue    = false;

void onSampleTimer() {
  sampleDue = true;
  TimerStart(&sampleTimer);   // CubeCell timers are one-shot, re-arm each time
}

// ---- Helpers ---------------------------------------------------
uint32_t readU32(const uint8_t* p) {
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
uint16_t readU16(const uint8_t* p) {
  return ((uint16_t)p[0]<<8)|p[1];
}

// ---- DLMS/COSEM frame parser -----------------------------------
void parseFrame(const uint8_t* buf, uint16_t len) {
  if (len < 30 || buf[0] != 0x7E) return;

  // Find end-of-descriptor anchor: 02 12 00 00 09 06 00 06 19 09 00 FF
  const uint8_t anchor[] = { 0x02, 0x12, 0x00, 0x00, 0x09, 0x06,
                              0x00, 0x06, 0x19, 0x09, 0x00, 0xFF };
  uint16_t pos = 0;
  for (uint16_t i = 0; i < len - 20; i++) {
    bool match = true;
    for (uint8_t j = 0; j < 12; j++) {
      if (buf[i+j] != anchor[j]) { match = false; break; }
    }
    if (match) { pos = i + 12; break; }
  }
  if (pos == 0) return;

  // Expect: 09 07 <7 bytes serial>
  if (buf[pos] != 0x09 || buf[pos+1] != 0x07) return;
  pos += 2;
  memcpy(serialNumber, &buf[pos], 7);
  serialNumber[7] = '\0';
  pos += 7;

  // Read 14 values in fixed order
  #define READ_U32(var) if (buf[pos] != 0x06 || pos+4 >= len) return; \
                        var = readU32(&buf[pos+1]); pos += 5;
  #define READ_U16(var) if (buf[pos] != 0x12 || pos+2 >= len) return; \
                        var = readU16(&buf[pos+1]); pos += 3;

  READ_U32(powerImport)
  READ_U32(powerExport)
  READ_U32(energyImportTotal)
  READ_U32(energyExportTotal)
  READ_U16(voltageL1)
  READ_U16(voltageL2)
  READ_U16(voltageL3)
  READ_U16(currentL1)
  READ_U16(currentL2)
  READ_U16(currentL3)
  READ_U32(energyImportT1)
  READ_U32(energyImportT2)
  READ_U32(energyExportT1)
  READ_U32(energyExportT2)

  #undef READ_U32
  #undef READ_U16

  newDataReady = true;
}

// ---- Read + parse one telegram off the UART ---------------------
// Shared by the periodic high-res sampler and the TX preparation.
// Returns true if a full frame was parsed before timeoutMs elapsed.
bool readMeterFrame(uint32_t timeoutMs) {
  uint32_t timeout = millis() + timeoutMs;
  newDataReady = false;
  while (!newDataReady && millis() < timeout) {
    while (Serial1.available()) {
      uint8_t b = Serial1.read();
      if (b == 0x7E) {
        if (inFrame && frameLen > 10) {
          frameBuffer[frameLen++] = b;
          parseFrame(frameBuffer, frameLen);
        }
        frameLen = 0;
        frameBuffer[frameLen++] = b;
        inFrame = true;
      } else if (inFrame) {
        if (frameLen < RX_BUFFER_SIZE - 1) frameBuffer[frameLen++] = b;
        else { inFrame = false; frameLen = 0; }
      }
    }
  }
  return newDataReady;
}

// ---- Periodic high-res sample (called between TX cycles) -------
void doPeriodicSample() {
  sampleDue = false;
  if (!readMeterFrame(3000)) return;   // short timeout — don't block long

  if (currentL1 < minCurrentL1) minCurrentL1 = currentL1;
  if (currentL1 > maxCurrentL1) maxCurrentL1 = currentL1;

  if (lastCurrentL1 != 0 &&
      (int32_t)lastCurrentL1 - (int32_t)currentL1 >= DROP_THRESHOLD) {
    dropEventCount++;
    lastDropOffsetSec = (millis() - windowStartMs) / 1000;
  }
  lastCurrentL1 = currentL1;
}

// ---- Build LoRaWAN payload -------------------------------------
// Called by the LoRaWAN stack just before transmitting
void prepareTxFrame(uint8_t port) {
  // Wait for a fresh frame from the meter (up to 10 seconds)
  bool gotFrame = readMeterFrame(10000);

  // Fold this last reading into the window stats too
  if (gotFrame) {
    if (currentL1 < minCurrentL1) minCurrentL1 = currentL1;
    if (currentL1 > maxCurrentL1) maxCurrentL1 = currentL1;
  }

  if (!gotFrame) {
    Serial.println("No P1 data — skipping TX");
    appDataSize = 0;
    return;
  }

  // Pack payload big-endian
  uint8_t* p = appData;

  // uint32 fields
  auto packU32 = [&](uint32_t v) {
    *p++ = (v >> 24) & 0xFF;
    *p++ = (v >> 16) & 0xFF;
    *p++ = (v >>  8) & 0xFF;
    *p++ =  v        & 0xFF;
  };
  auto packU16 = [&](uint16_t v) {
    *p++ = (v >>  8) & 0xFF;
    *p++ =  v        & 0xFF;
  };

  packU32(powerImport);
  packU32(powerExport);
  packU32(energyImportTotal);
  packU32(energyExportTotal);
  packU16(voltageL1);
  packU16(voltageL2);
  packU16(voltageL3);
  packU16(currentL1);
  packU16(currentL2);
  packU16(currentL3);
  packU32(energyImportT1);
  packU32(energyImportT2);
  packU32(energyExportT1);
  packU32(energyExportT2);
  packU16(minCurrentL1 == 0xFFFF ? currentL1 : minCurrentL1);
  packU16(maxCurrentL1);
  packU16(dropEventCount);
  packU32(lastDropOffsetSec);
  appDataSize = 54;

  Serial.println("Payload ready:");
  Serial.print("  Power import:  "); Serial.print(powerImport);  Serial.println(" W");
  Serial.print("  Voltage L1:    "); Serial.print(voltageL1);    Serial.println(" V");
  Serial.print("  Current L1:    "); Serial.print(currentL1/100.0, 2); Serial.println(" A");
  Serial.print("  Energy import: "); Serial.print(energyImportTotal); Serial.println(" Wh");
  Serial.print("  Window min/max L1: "); Serial.print(minCurrentL1/100.0, 2);
  Serial.print(" / "); Serial.print(maxCurrentL1/100.0, 2); Serial.println(" A");
  Serial.print("  Drop events:   "); Serial.println(dropEventCount);

  // Reset the window for the next 15-minute cycle
  minCurrentL1 = 0xFFFF;
  maxCurrentL1 = 0;
  dropEventCount = 0;
  lastDropOffsetSec = 0;
  windowStartMs = millis();
}

// ---- Setup -----------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("Flexy F2 P1 + LoRaWAN ready");

  Serial1.begin(115200, SERIAL_8N1, UART_RX2, UART_TX2, false, 20000UL);

  TimerInit(&sampleTimer, onSampleTimer);
  TimerSetValue(&sampleTimer, SAMPLE_INTERVAL_MS);
  TimerStart(&sampleTimer);
  windowStartMs = millis();

  deviceState = DEVICE_STATE_INIT;
}

// ---- Loop — LoRaWAN state machine ------------------------------
void loop() {
  if (sampleDue) doPeriodicSample();

  switch (deviceState) {
    case DEVICE_STATE_INIT:
      LoRaWAN.init(loraWanClass, loraWanRegion);
      LoRaWAN.setDataRateForNoADR(2);  // DR2 = SF10 for CN470 — better link margin through  concrete walls than the DR5/SF7 default
      deviceState = DEVICE_STATE_JOIN;
      break;

    case DEVICE_STATE_JOIN:
      LoRaWAN.join();
      break;

    case DEVICE_STATE_SEND:
      prepareTxFrame(appPort);
      LoRaWAN.send();
      deviceState = DEVICE_STATE_CYCLE;
      break;

    case DEVICE_STATE_CYCLE:
      txDutyCycleTime = appTxDutyCycle + randr(-APP_TX_DUTYCYCLE_RND, APP_TX_DUTYCYCLE_RND);
      LoRaWAN.cycle(txDutyCycleTime);
      deviceState = DEVICE_STATE_SLEEP;
      break;

    case DEVICE_STATE_SLEEP:
      LoRaWAN.sleep();
      break;

    default:
      deviceState = DEVICE_STATE_INIT;
      break;
  }
}

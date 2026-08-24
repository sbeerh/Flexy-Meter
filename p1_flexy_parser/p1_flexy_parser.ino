// ============================================================
// P1 DLMS/COSEM Parser — Meter&Control Flexy F2
// Romande Energie configuration
// Target: Heltec CubeCell HTCC-AB02A
// ============================================================
// Wiring:
//   RJ12 pin 1 (+5V) ── pin 2 (RTS)
//   RJ12 pin 5 (DATA) ──+──[10kΩ]── +5V (meter)
//                       └── BS170 Gate (middle leg)
//   BS170 Source (right leg) ── GND (common with CubeCell)
//   BS170 Drain  (left leg)  ── UART_RX2
//   RJ12 pin 3/6 (GND) ── CubeCell GND
//   CubeCell powered by USB only — do NOT connect RJ12 +5V to CubeCell
// ============================================================
// Frame structure (verified against live captures):
//   7E ... E6 E6 00 ... [OBIS descriptor block] ...
//   09 06 <serial OBIS 6 bytes>  09 07 <serial ASCII 7 bytes>
//   06 <u32> power import (W)
//   06 <u32> power export (W)
//   06 <u32> energy import total (Wh)
//   06 <u32> energy export total (Wh)
//   12 <u16> voltage L1 (V, no scaling)
//   12 <u16> voltage L2 (V)
//   12 <u16> voltage L3 (V)
//   12 <u16> current L1 (×0.01 A)
//   12 <u16> current L2 (×0.01 A)
//   12 <u16> current L3 (×0.01 A)
//   06 <u32> energy import T1 HP (Wh)
//   06 <u32> energy import T2 HC (Wh)
//   06 <u32> energy export T1 HP (Wh)
//   06 <u32> energy export T2 HC (Wh)
//   <2 bytes CRC> 7E
// ============================================================

#define RX_BUFFER_SIZE 512

uint8_t  frameBuffer[RX_BUFFER_SIZE];
uint16_t frameLen = 0;
bool     inFrame  = false;

// Parsed values
char     serialNumber[16] = "";
uint32_t powerImport      = 0;  // W
uint32_t powerExport      = 0;  // W
uint32_t energyImportTotal= 0;  // Wh
uint32_t energyExportTotal= 0;  // Wh
uint16_t voltageL1        = 0;  // V
uint16_t voltageL2        = 0;
uint16_t voltageL3        = 0;
uint16_t currentL1        = 0;  // raw (divide by 100 for A)
uint16_t currentL2        = 0;
uint16_t currentL3        = 0;
uint32_t energyImportT1   = 0;  // Wh HP
uint32_t energyImportT2   = 0;  // Wh HC
uint32_t energyExportT1   = 0;  // Wh HP
uint32_t energyExportT2   = 0;  // Wh HC

// ============================================================
// Helpers
// ============================================================
uint32_t readU32(const uint8_t* p) {
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
uint16_t readU16(const uint8_t* p) {
  return ((uint16_t)p[0]<<8)|p[1];
}

// ============================================================
// Parse one complete HDLC frame
// Strategy: find the serial number octet-string marker
//   09 07 <7 ASCII bytes>
// then read the 14 values that follow in fixed order.
// ============================================================
void parseFrame(const uint8_t* buf, uint16_t len) {
  if (len < 30 || buf[0] != 0x7E) return;

  const uint8_t anchor[] = {0x02, 0x12, 0x00, 0x00, 0x09, 0x06,
                             0x00, 0x06, 0x19, 0x09, 0x00, 0xFF};
  uint16_t pos = 0;

  for (uint16_t i = 0; i < len - 20; i++) {
    bool match = true;
    for (uint8_t j = 0; j < 12; j++) {
      if (buf[i+j] != anchor[j]) { match = false; break; }
    }
    if (match) { pos = i + 12; break; }
  }

  if (pos == 0) {
    Serial.println("DBG: anchor not found");
    return;
  }

  Serial.print("DBG: pos="); Serial.println(pos);
  Serial.print("DBG: next 12 bytes: ");
  for (uint8_t i = 0; i < 12; i++) {
    if (buf[pos+i] < 0x10) Serial.print("0");
    Serial.print(buf[pos+i], HEX); Serial.print(" ");
  }
  Serial.println();

  // Now at: 09 07 <7 bytes serial>
  if (buf[pos] != 0x09 || buf[pos+1] != 0x07) return;
  pos += 2;

  memcpy(serialNumber, &buf[pos], 7);
  serialNumber[7] = '\0';
  pos += 7;

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
}

// ============================================================
// Print decoded values
// ============================================================
void printValues() {
  Serial.println("========== Flexy F2 P1 ==========");
  Serial.print("Serial:        "); Serial.println(serialNumber);
  Serial.print("Power import:  "); Serial.print(powerImport);        Serial.println(" W");
  Serial.print("Power export:  "); Serial.print(powerExport);        Serial.println(" W");
  Serial.print("Energy import: "); Serial.print(energyImportTotal);  Serial.println(" Wh");
  Serial.print("Energy export: "); Serial.print(energyExportTotal);  Serial.println(" Wh");
  Serial.print("Voltage L1:    "); Serial.print(voltageL1);          Serial.println(" V");
  Serial.print("Voltage L2:    "); Serial.print(voltageL2);          Serial.println(" V");
  Serial.print("Voltage L3:    "); Serial.print(voltageL3);          Serial.println(" V");
  Serial.print("Current L1:    "); Serial.print(currentL1 / 100.0, 2); Serial.println(" A");
  Serial.print("Current L2:    "); Serial.print(currentL2 / 100.0, 2); Serial.println(" A");
  Serial.print("Current L3:    "); Serial.print(currentL3 / 100.0, 2); Serial.println(" A");
  Serial.print("Energy HP T1:  "); Serial.print(energyImportT1);    Serial.println(" Wh");
  Serial.print("Energy HC T2:  "); Serial.print(energyImportT2);    Serial.println(" Wh");
  Serial.print("Export HP T1:  "); Serial.print(energyExportT1);    Serial.println(" Wh");
  Serial.print("Export HC T2:  "); Serial.print(energyExportT2);    Serial.println(" Wh");
  Serial.println("=================================");
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("Flexy F2 P1 Parser ready");
  Serial1.begin(115200, SERIAL_8N1, UART_RX2, UART_TX2, false, 20000UL);
}

// ============================================================
// Loop — accumulate HDLC frames between 7E flags
// ============================================================
void loop() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    if (b == 0x7E) {
      if (inFrame && frameLen > 10) {
        frameBuffer[frameLen++] = b;
        parseFrame(frameBuffer, frameLen);
        printValues();
      }
      frameLen = 0;
      frameBuffer[frameLen++] = b;
      inFrame = true;
    } else if (inFrame) {
      if (frameLen < RX_BUFFER_SIZE - 1) {
        frameBuffer[frameLen++] = b;
      } else {
        inFrame  = false;
        frameLen = 0;
      }
    }
  }
}

// ============================================================
// ChirpStack Payload Codec — Flexy F2 P1 Meter
// Romande Energie configuration
// Device: CubeCell HTCC-AB02A
// ============================================================
// Paste this into: Device Profile → Codec → Decoder function
// ============================================================

function decodeUplink(input) {
  var bytes = input.bytes;
  var data = {};

  function readU32(offset) {
    return (bytes[offset] << 24 | bytes[offset+1] << 16 |
            bytes[offset+2] << 8 | bytes[offset+3]) >>> 0;
  }

  function readU16(offset) {
    return (bytes[offset] << 8) | bytes[offset+1];
  }

  if (bytes.length < 44) {
    return {
      data: {},
      errors: ["Payload too short, expected 44 bytes, got " + bytes.length]
    };
  }

  data.powerImport         = readU32(0);   // W
  data.powerExport         = readU32(4);   // W
  data.energyImportTotal   = readU32(8);   // Wh
  data.energyExportTotal   = readU32(12);  // Wh
  data.voltageL1           = readU16(16);  // V
  data.voltageL2           = readU16(18);  // V
  data.voltageL3           = readU16(20);  // V
  data.currentL1           = readU16(22) / 100.0;  // A
  data.currentL2           = readU16(24) / 100.0;  // A
  data.currentL3           = readU16(26) / 100.0;  // A
  data.energyImportT1      = readU32(28);  // Wh (HP - Heures Pleines)
  data.energyImportT2      = readU32(32);  // Wh (HC - Heures Creuses)
  data.energyExportT1      = readU32(36);  // Wh (HP)
  data.energyExportT2      = readU32(40);  // Wh (HC)
  data.minCurrentL1        = readU16(44) / 100.0; // A min current L1 seen this window (x0.01 A)
  data.maxCurrentL1        = readU16(46) / 100.0; // A max current L1 seen this window (x0.01 A)
  data.dropEventCount      = readU16(48); // drop-event count this window (appliance likely stopped)
  data.secondsIntoWindow   = readU16(50); // seconds-into-window of the most recent drop event

  return {
    data: data
  };
}


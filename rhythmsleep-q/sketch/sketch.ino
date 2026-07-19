// ============================================================================
// RhythmSleep — UNO Q  (I2C Slave @ 0x09)
// Responsibilities:
//   • I2C slave — receives commands from R3 master
//   • Forwards EEG data + events to Python server via USB Serial
//   • Forwards Python commands back to R3 via I2C (future use)
//
// Commands from R3 (received via I2C onReceive):
//   0xA1 + float(4B) + state(1B) + conf(1B) → CSV line to Python
//   0xA2                                     → "WAKE" to Python
//   0xA3 + alarmH(1B) + alarmM(1B) + buf(1B) → SET_ALARM to Python
//
// USB Serial format to Python server (unchanged):
//   "freq,BandName"   e.g.  "7.25,Light Sleep"
//   "WAKE"
//   "SET_ALARM:HH:MM,bufMin"
// ============================================================================

#include <Wire.h>

#define MY_I2C_ADDR 0x09

// Band label lookup
const char* BAND_LBL[] = {"Unknown","Deep Sleep","Light Sleep","Relaxed","Active","Focused"};

// I2C receive buffer
#define RX_BUF_SIZE 16
volatile uint8_t rxBuf[RX_BUF_SIZE];
volatile uint8_t rxLen = 0;
volatile bool    rxReady = false;

// ─────────────────────────────────────────────────────────────────────────────
void onI2CReceive(int numBytes) {
  rxLen = 0;
  while (Wire.available() && rxLen < RX_BUF_SIZE) {
    rxBuf[rxLen++] = Wire.read();
  }
  rxReady = true;
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin(MY_I2C_ADDR);
  Wire.onReceive(onI2CReceive);
  Serial.println(F("UNOQ BOOT: I2C slave active."));
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Handle incoming I2C message in main loop (safe, not in ISR)
  if (rxReady) {
    rxReady = false;
    processI2CMessage();
  }

  // Forward any Python → R3 commands (future use: currently echo only)
  // If Python sends a line, forward it back via Serial so it's logged
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    // Reserved for future R3 acknowledgement commands
    // For now, just echo back for debugging
    Serial.print(F("ECHO:"));
    Serial.println(cmd);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void processI2CMessage() {
  if (rxLen == 0) return;

  uint8_t cmd = rxBuf[0];

  if (cmd == 0xA1 && rxLen >= 7) {
    // EEG data: freq(4B float) + state(1B) + conf(1B)
    float freq = 0.0f;
    memcpy(&freq, (void*)(rxBuf + 1), 4);
    uint8_t state = rxBuf[5];
    uint8_t conf  = rxBuf[6];
    const char* band = (state < 6) ? BAND_LBL[state] : "Unknown";

    // CSV line: "freq,Band,confidence"
    Serial.print(freq, 2);
    Serial.print(',');
    Serial.print(band);
    Serial.print(',');
    Serial.println(conf);
  }
  else if (cmd == 0xA2) {
    // Alarm triggered
    Serial.println(F("WAKE"));
  }
  else if (cmd == 0xA3 && rxLen >= 4) {
    // Alarm settings: alarmH + alarmM + buf
    uint8_t aH  = rxBuf[1];
    uint8_t aM  = rxBuf[2];
    uint8_t buf = rxBuf[3];
    // Format: SET_ALARM:HH:MM,bufMin
    Serial.print(F("SET_ALARM:"));
    if (aH < 10) Serial.print('0'); Serial.print(aH); Serial.print(':');
    if (aM < 10) Serial.print('0'); Serial.print(aM); Serial.print(',');
    Serial.println(buf);
  }
}

// ============================================================================
// RhythmSleep — Arduino UNO R3  (I2C Master)
// Responsibilities:
//   • Owns PCF8563 RTC — source of truth for time
//   • Drives I2C crystal LCD (16×2) — always shows current time
//   • Polls R4 Minima (slave 0x08) for sleep state + alarm settings
//   • Runs smart alarm: fires early if user is in light/lighter sleep
//   • Relays EEG data + WAKE commands to UNO Q (slave 0x09) → Python
//
// I2C Bus (shared):
//   R3  = Master        | PCF8563 @ 0x51 | LCD @ 0x27
//   R4  = Slave @ 0x08  | UNO Q = Slave @ 0x09
//
// Wiring reminder:
//   SDA (A4) and SCL (A5) shared across all boards + devices.
//   One pair of 4.7kΩ pull-ups to 3.3 V (R4 Minima logic level).
//   Use I2C level-shifter between 5 V boards and R4 Minima if needed.
// ============================================================================

#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>   // PCF8574 backpack, install via Library Manager

// ── I2C slave addresses ──────────────────────────────────────────────────────
#define R4_ADDR   0x08
#define UNOQ_ADDR 0x09

// ── LCD: 16×2 with I2C backpack (try 0x3F if 0x27 doesn't work) ─────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_PCF8563       rtc;

// ── Alarm output ─────────────────────────────────────────────────────────────
#define BUZZER_PIN 8
#define LED_PIN    13

// ── I2C command bytes sent to UNO Q ──────────────────────────────────────────
#define CMD_DATA  0xA1   // EEG data payload
#define CMD_WAKE  0xA2   // Alarm fired
#define CMD_ALARM 0xA3   // Forward alarm settings

// ── Live data from R4 Minima ──────────────────────────────────────────────────
float   eegFreq     = 0.0f;
uint8_t sleepState  = 0;   // 0=Unknown 1=Deep 2=Light 3=Relaxed 4=Active 5=Focused
uint8_t confidence  = 0;   // 0–100
uint8_t alarmHour   = 6;
uint8_t alarmMin    = 0;
uint8_t bufferMin   = 30;

// ── Alarm state ───────────────────────────────────────────────────────────────
bool          alarmRinging  = false;
unsigned long alarmStartMs  = 0;
#define ALARM_AUTO_STOP_MS 120000UL  // Auto-dismiss after 2 min

// ── Intervals ────────────────────────────────────────────────────────────────
unsigned long tLcd   = 0, tPoll  = 0, tRelay = 0;
#define LCD_MS   1000UL
#define POLL_MS  2000UL
#define RELAY_MS 5000UL

// ── State label table ─────────────────────────────────────────────────────────
const char* STATE_LBL[] = {"Unknown ","DeepSlp ","LtSleep ","Relaxed ","Active  ","Focused "};

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin();            // R3 = I2C master

  // RTC
  if (!rtc.begin()) Serial.println(F("ERR: RTC"));

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print(F("RhythmSleep v2  "));
  lcd.setCursor(0, 1); lcd.print(F("Booting...      "));
  delay(1200);
  lcd.clear();

  // Alarm output pins
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    LOW);
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (now - tLcd   >= LCD_MS)   { tLcd   = now; updateLCD(); }
  if (now - tPoll  >= POLL_MS)  { tPoll  = now; pollR4Minima(); checkAlarm(); }
  if (now - tRelay >= RELAY_MS) { tRelay = now; relayToUnoQ(); }

  driveAlarm();
}

// ── LCD: row0 = time + alarm indicator  row1 = sleep state + confidence ──────
void updateLCD() {
  DateTime dt = rtc.now();

  // Row 0
  lcd.setCursor(0, 0);
  char buf[17];
  if (alarmRinging) {
    sprintf(buf, "%02d:%02d:%02d  WAKE!", dt.hour(), dt.minute(), dt.second());
  } else {
    sprintf(buf, "%02d:%02d:%02d A%02d:%02d", dt.hour(), dt.minute(), dt.second(),
            alarmHour, alarmMin);
  }
  lcd.print(buf);

  // Row 1
  lcd.setCursor(0, 1);
  char row1[17];
  sprintf(row1, "%-8s  %3d%%", STATE_LBL[sleepState < 6 ? sleepState : 0], confidence);
  lcd.print(row1);
}

// ── Poll R4 Minima: send 0x01 request, read 10-byte response ─────────────────
void pollR4Minima() {
  Wire.beginTransmission(R4_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return;  // slave not responding

  delayMicroseconds(800);

  if (Wire.requestFrom((uint8_t)R4_ADDR, (uint8_t)10) == 10) {
    uint8_t buf[10];
    for (uint8_t i = 0; i < 10; i++) buf[i] = Wire.read();

    memcpy(&eegFreq, buf + 0, 4);   // bytes 0-3: float freq
    sleepState = buf[4];
    confidence = buf[5];
    alarmHour  = buf[6];
    alarmMin   = buf[7];
    bufferMin  = buf[8];
    // buf[9]  = reserved
  }
}

// ── Smart alarm logic ─────────────────────────────────────────────────────────
void checkAlarm() {
  if (alarmRinging) return;

  DateTime dt        = rtc.now();
  int nowM           = dt.hour() * 60 + dt.minute();
  int alarmM         = (int)alarmHour * 60 + (int)alarmMin;
  int bufStart       = alarmM - (int)bufferMin;
  if (bufStart < 0) bufStart += 1440;

  // Check if we're inside the buffer window (handles midnight wrap)
  bool inWindow;
  if (bufStart <= alarmM)
    inWindow = (nowM >= bufStart && nowM <= alarmM);
  else
    inWindow = (nowM >= bufStart || nowM <= alarmM);

  if (!inWindow) return;

  // Exact alarm time → fire unconditionally
  if (nowM == alarmM) { triggerAlarm(); return; }

  // Within buffer: fire if state is Light Sleep or lighter (>= 2) with confidence >= 35
  if (sleepState >= 2 && confidence >= 35) triggerAlarm();
}

void triggerAlarm() {
  alarmRinging = true;
  alarmStartMs = millis();
  // Notify UNO Q → Python
  Wire.beginTransmission(UNOQ_ADDR);
  Wire.write(CMD_WAKE);
  Wire.endTransmission();
  Serial.println(F("ALARM_TRIGGERED"));
}

void driveAlarm() {
  if (!alarmRinging) return;
  if (millis() - alarmStartMs > ALARM_AUTO_STOP_MS) {
    alarmRinging = false;
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN,    LOW);
    return;
  }
  bool phase = ((millis() / 400) & 1);
  digitalWrite(BUZZER_PIN, phase);
  digitalWrite(LED_PIN,    phase);
}

// ── Relay EEG data to UNO Q for Python logging ────────────────────────────────
void relayToUnoQ() {
  // Pack: CMD_DATA | freq(4B) | sleepState | confidence
  Wire.beginTransmission(UNOQ_ADDR);
  Wire.write(CMD_DATA);
  Wire.write((uint8_t*)&eegFreq, 4);
  Wire.write(sleepState);
  Wire.write(confidence);
  Wire.endTransmission();
}

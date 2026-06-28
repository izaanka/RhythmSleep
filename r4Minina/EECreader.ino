// ============================================================================
// RhythmSleep — R4 Minima EEG Reader + OLED Interface
// ============================================================================
// Hardware:
//   - Analog EEG sensor on A2
//   - PCF8563 RTC on I2C (SDA/SCL)
//   - SSD1306 128x64 OLED on I2C (SDA/SCL)
//   - 4 Tactile Buttons: MODE(D2), UP(D3), DOWN(D4), SELECT(D5)
//   - Serial1 TX/RX → UNO Q
//
// Role: Acquire EEG data, run FFT, extract dominant frequency,
//       send FREQ:XX.XX to UNO Q. Display info on OLED. No alarm
//       logic — that runs on the UNO Q side.
// ============================================================================

#include <Wire.h>
#include "RTClib.h"
#include "arduinoFFT.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── FFT Configuration ───────────────────────────────────────────────────────
#define SAMPLES 1024
#define SAMPLING_FREQUENCY 256
#define EEG_PIN A2

// ── OLED Configuration ─────────────────────────────────────────────────────
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

// ── Button Pins ─────────────────────────────────────────────────────────────
#define BTN_MODE   2
#define BTN_UP     3
#define BTN_DOWN   4
#define BTN_SELECT 5

// ── Display Modes ───────────────────────────────────────────────────────────
enum DisplayMode {
  MODE_TIME = 0,
  MODE_STATE = 1,
  MODE_ALARM = 2,
  MODE_COUNT = 3
};

// ── Alarm Edit Fields ───────────────────────────────────────────────────────
enum AlarmField {
  FIELD_ALARM_HOUR = 0,
  FIELD_ALARM_MIN = 1,
  FIELD_BUFFER = 2,
  FIELD_COUNT = 3
};

// ── FFT Buffers (declared before ArduinoFFT constructor) ────────────────────
double vReal[SAMPLES];
double vImag[SAMPLES];

// ── Global Objects ──────────────────────────────────────────────────────────
RTC_PCF8563 rtc;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQUENCY);

unsigned int sampling_period_us;

// ── State Variables ─────────────────────────────────────────────────────────
DisplayMode currentMode = MODE_TIME;
AlarmField  currentField = FIELD_ALARM_HOUR;

// Alarm settings (display-only on Minima; actual logic runs on UNO Q)
int alarm_hour = 5;
int alarm_min  = 30;
int alarm_m    = 330;     // Minutes from midnight (5*60+30)
int buffer_m   = 30;

// Current EEG data
double dominantFreq = 0.0;
String currentBand  = "---";

// Alarm display flag (set when UNO Q sends WAKE)
bool alarmRinging = false;

// Button edge detection (press, not hold)
bool prevBtnState[4] = {HIGH, HIGH, HIGH, HIGH};
unsigned long lastBtnChange[4] = {0, 0, 0, 0};
#define DEBOUNCE_MS 50

// Display refresh throttle
unsigned long lastDisplayUpdate = 0;
#define DISPLAY_INTERVAL_MS 250

// ── Forward Declarations ────────────────────────────────────────────────────
void updateDisplay();
void drawTimeMode(DateTime now);
void drawStateMode(DateTime now);
void drawAlarmMode();
void handleButtons();
bool buttonPressed(int pin, int idx);
void classifyFrequency(double freq);
void processCommands();
void recalcAlarmMinutes();
void sendAlarmToUNOQ();

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);   // Debug to PC
  Serial1.begin(115200);  // Data link to UNO Q

  while (!Serial) { ; }
  Serial.println("SYSTEM BOOT: Initializing I2C peripherals...");

  Wire.begin();

  // ── RTC Init ──────────────────────────────────────────────────────────────
  if (!rtc.begin()) {
    Serial.println("CRITICAL: PCF8563 RTC NOT FOUND. HALTING.");
    Serial1.println("ERROR: RTC_NOT_FOUND");
    while (1);
  }
  Serial.println("RTC OK.");

  // ── OLED Init ─────────────────────────────────────────────────────────────
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("WARNING: SSD1306 OLED not found.");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("RhythmSleep");
    display.println("Initializing...");
    display.display();
    Serial.println("OLED OK.");
  }

  // ── Button Init ───────────────────────────────────────────────────────────
  pinMode(BTN_MODE,   INPUT_PULLUP);
  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  // ── FFT Init ──────────────────────────────────────────────────────────────
  sampling_period_us = round(1000000.0 / SAMPLING_FREQUENCY);
  analogReadResolution(12);

  Serial.println("BOOT COMPLETE. Entering main loop.");
  delay(1000);
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // ── 1. Acquire EEG samples ───────────────────────────────────────────────
  unsigned long microseconds;
  for (int i = 0; i < SAMPLES; i++) {
    microseconds = micros();
    vReal[i] = analogRead(EEG_PIN);
    vImag[i] = 0;
    while (micros() - microseconds < sampling_period_us) {}
  }

  // ── 2. Run FFT ────────────────────────────────────────────────────────────
  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  // ── 3. Extract dominant frequency ─────────────────────────────────────────
  // Each FFT bin = SAMPLING_FREQUENCY / SAMPLES = 0.25 Hz
  // Search bins 2 (0.5 Hz) through 400 (100 Hz)
  double maxMagnitude = 0;
  int    maxBin = 2;

  for (int i = 2; i <= 400; i++) {
    if (vReal[i] > maxMagnitude) {
      maxMagnitude = vReal[i];
      maxBin = i;
    }
  }

  dominantFreq = (double)maxBin * SAMPLING_FREQUENCY / SAMPLES;

  // ── 4. Classify for OLED display ──────────────────────────────────────────
  classifyFrequency(dominantFreq);

  // ── 5. Send dominant frequency to UNO Q ───────────────────────────────────
  Serial1.print("FREQ:");
  Serial1.println(dominantFreq, 2);

  // ── 6. Debug output to PC ─────────────────────────────────────────────────
  DateTime now = rtc.now();
  char timeBuffer[25];
  sprintf(timeBuffer, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());

  Serial.print("PC -> Time: ");
  Serial.print(timeBuffer);
  Serial.print(" | Freq: ");
  Serial.print(dominantFreq, 2);
  Serial.print(" Hz | Band: ");
  Serial.println(currentBand);

  // ── 7. Handle buttons ─────────────────────────────────────────────────────
  handleButtons();

  // ── 8. Update OLED display ────────────────────────────────────────────────
  if (millis() - lastDisplayUpdate > DISPLAY_INTERVAL_MS) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }

  // ── 9. Process incoming commands from UNO Q ───────────────────────────────
  processCommands();

  delay(100);
}

// ============================================================================
// FREQUENCY CLASSIFICATION (for OLED display only)
// ============================================================================
void classifyFrequency(double freq) {
  if (freq >= 30.0)       currentBand = "Focused";     // Gamma
  else if (freq >= 12.0)  currentBand = "Active";      // Beta
  else if (freq >= 8.0)   currentBand = "Relaxed";     // Alpha
  else if (freq >= 4.0)   currentBand = "Light Sleep"; // Theta
  else if (freq >= 0.5)   currentBand = "Deep Sleep";  // Delta
  else                    currentBand = "---";
}

// ============================================================================
// BUTTON HANDLING
// ============================================================================
bool buttonPressed(int pin, int idx) {
  bool reading = digitalRead(pin);
  bool pressed = false;

  // Only act on edges: previous was HIGH (released), now LOW (pressed)
  if (reading == LOW && prevBtnState[idx] == HIGH) {
    if (millis() - lastBtnChange[idx] > DEBOUNCE_MS) {
      pressed = true;
    }
  }

  if (reading != prevBtnState[idx]) {
    lastBtnChange[idx] = millis();
  }

  prevBtnState[idx] = reading;
  return pressed;
}

void handleButtons() {
  // BTN_MODE — Cycle display mode
  if (buttonPressed(BTN_MODE, 0)) {
    currentMode = (DisplayMode)((currentMode + 1) % MODE_COUNT);
    currentField = FIELD_ALARM_HOUR;
    Serial.print("MODE -> ");
    Serial.println(currentMode == MODE_TIME ? "Time" :
                   currentMode == MODE_STATE ? "State" : "Alarm");
  }

  // Only process UP/DOWN/SELECT in alarm mode
  if (currentMode == MODE_ALARM) {
    // BTN_SELECT — Toggle between alarm fields
    if (buttonPressed(BTN_SELECT, 3)) {
      currentField = (AlarmField)((currentField + 1) % FIELD_COUNT);
      Serial.print("FIELD -> ");
      Serial.println(currentField == FIELD_ALARM_HOUR ? "Hour" :
                     currentField == FIELD_ALARM_MIN ? "Minute" : "Buffer");
    }

    // BTN_UP — Increment selected field
    if (buttonPressed(BTN_UP, 1)) {
      switch (currentField) {
        case FIELD_ALARM_HOUR:
          alarm_hour = (alarm_hour + 1) % 24;
          break;
        case FIELD_ALARM_MIN:
          alarm_min = (alarm_min + 1) % 60;
          break;
        case FIELD_BUFFER:
          buffer_m = min(buffer_m + 5, 120);
          break;
        default: break;
      }
      recalcAlarmMinutes();
      sendAlarmToUNOQ();
    }

    // BTN_DOWN — Decrement selected field
    if (buttonPressed(BTN_DOWN, 2)) {
      switch (currentField) {
        case FIELD_ALARM_HOUR:
          alarm_hour = (alarm_hour - 1 + 24) % 24;
          break;
        case FIELD_ALARM_MIN:
          alarm_min = (alarm_min - 1 + 60) % 60;
          break;
        case FIELD_BUFFER:
          buffer_m = max(buffer_m - 5, 5);
          break;
        default: break;
      }
      recalcAlarmMinutes();
      sendAlarmToUNOQ();
    }
  }
}

void recalcAlarmMinutes() {
  alarm_m = alarm_hour * 60 + alarm_min;
  Serial.print("ALARM SET: ");
  Serial.print(alarm_hour); Serial.print(":"); Serial.print(alarm_min);
  Serial.print(" | Buffer: "); Serial.print(buffer_m); Serial.println("m");
}

// Send updated alarm settings to UNO Q → Python server
void sendAlarmToUNOQ() {
  Serial1.print("SET_ALARM:");
  Serial1.print(alarm_m);
  Serial1.print(",");
  Serial1.println(buffer_m);
}

// ============================================================================
// OLED DISPLAY
// ============================================================================
void updateDisplay() {
  display.clearDisplay();

  DateTime now = rtc.now();

  switch (currentMode) {
    case MODE_TIME:
      drawTimeMode(now);
      break;
    case MODE_STATE:
      drawStateMode(now);
      break;
    case MODE_ALARM:
      drawAlarmMode();
      break;
    default: break;
  }

  // Mode indicator bar at bottom
  display.drawLine(0, 56, 127, 56, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 58);
  display.print(currentMode == MODE_TIME  ? ">TIME" : " TIME");
  display.setCursor(42, 58);
  display.print(currentMode == MODE_STATE ? ">STATE" : " STATE");
  display.setCursor(88, 58);
  display.print(currentMode == MODE_ALARM ? ">ALM" : " ALM");

  display.display();
}

void drawTimeMode(DateTime now) {
  display.setTextSize(2);
  display.setCursor(10, 5);
  char timeBuf[6];
  sprintf(timeBuf, "%02d:%02d", now.hour(), now.minute());
  display.print(timeBuf);

  display.setTextSize(1);
  display.setCursor(98, 10);
  char secBuf[4];
  sprintf(secBuf, ":%02d", now.second());
  display.print(secBuf);

  display.setTextSize(1);
  display.setCursor(10, 28);
  char dateBuf[12];
  sprintf(dateBuf, "%04d-%02d-%02d", now.year(), now.month(), now.day());
  display.print(dateBuf);

  display.setCursor(10, 42);
  display.print("Alarm: ");
  char almBuf[6];
  sprintf(almBuf, "%02d:%02d", alarm_hour, alarm_min);
  display.print(almBuf);

  if (alarmRinging) {
    display.setCursor(90, 42);
    display.print("RING!");
  }
}

void drawStateMode(DateTime now) {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Brain State");

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(dominantFreq, 1);
  display.setTextSize(1);
  display.print(" Hz");

  display.setTextSize(1);
  display.setCursor(0, 34);
  display.print("Band: ");
  display.print(currentBand);

  int barWidth = constrain((int)(dominantFreq * 1.28), 0, 128);
  display.drawRect(0, 46, 128, 8, SSD1306_WHITE);
  display.fillRect(0, 46, barWidth, 8, SSD1306_WHITE);
}

void drawAlarmMode() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Alarm Settings");

  display.setCursor(0, 14);
  display.print("Wake: ");
  display.setTextSize(2);

  if (currentField == FIELD_ALARM_HOUR) {
    display.setCursor(36, 12);
    display.print("[");
    char hBuf[3];
    sprintf(hBuf, "%02d", alarm_hour);
    display.print(hBuf);
    display.print("]:");
    char mBuf[3];
    sprintf(mBuf, "%02d", alarm_min);
    display.print(mBuf);
  } else if (currentField == FIELD_ALARM_MIN) {
    display.setCursor(36, 12);
    char hBuf[3];
    sprintf(hBuf, "%02d", alarm_hour);
    display.print(hBuf);
    display.print(":[");
    char mBuf[3];
    sprintf(mBuf, "%02d", alarm_min);
    display.print(mBuf);
    display.print("]");
  } else {
    display.setCursor(36, 12);
    char almBuf[6];
    sprintf(almBuf, "%02d:%02d", alarm_hour, alarm_min);
    display.print(almBuf);
  }

  display.setTextSize(1);
  display.setCursor(0, 34);
  if (currentField == FIELD_BUFFER) {
    display.print("Buffer: [");
    display.print(buffer_m);
    display.print("] min");
  } else {
    display.print("Buffer:  ");
    display.print(buffer_m);
    display.print("  min");
  }

  display.setCursor(0, 46);
  display.print("UP/DN:adj SEL:field");
}

// ============================================================================
// COMMAND PROCESSING (from UNO Q)
// ============================================================================
void processCommands() {
  if (Serial1.available() > 0) {
    String command = Serial1.readStringUntil('\n');
    command.trim();

    Serial.print("CMD FROM UNO Q: ");
    Serial.println(command);

    if (command.indexOf("WAKE") >= 0) {
      // UNO Q triggered the alarm — update OLED display
      alarmRinging = true;
      Serial.println("ALARM SIGNAL RECEIVED FROM UNO Q.");
    }
    else if (command.indexOf("SET_TIME:") >= 0) {
      int y, m, d, h, mn, s;
      sscanf(command.substring(9).c_str(), "%d-%d-%d %d:%d:%d", &y, &m, &d, &h, &mn, &s);
      rtc.adjust(DateTime(y, m, d, h, mn, s));
      Serial.println("RTC SYNCED VIA NTP.");
    }
    else if (command.indexOf("SET_ALARM:") >= 0) {
      // Alarm settings pushed from web UI via UNO Q
      int commaIdx = command.indexOf(',', 10);
      if (commaIdx > 0) {
        alarm_m  = command.substring(10, commaIdx).toInt();
        buffer_m = command.substring(commaIdx + 1).toInt();
        alarm_hour = alarm_m / 60;
        alarm_min  = alarm_m % 60;
        Serial.print("ALARM UPDATED VIA WEB: ");
        Serial.print(alarm_hour); Serial.print(":"); Serial.print(alarm_min);
        Serial.print(" Buffer: "); Serial.println(buffer_m);
      }
    }
  }
}

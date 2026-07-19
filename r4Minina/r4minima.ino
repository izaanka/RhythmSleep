// ============================================================================
// RhythmSleep — Arduino R4 Minima  (I2C Slave @ 0x08)
// Responsibilities:
//   • EEG sampling (A2) + optimized FFT sleep-state engine
//   • OLED display: Sleep State screen + Alarm Settings screen (NO time)
//   • 4 buttons: MODE / UP / DOWN / SELECT
//   • I2C slave: responds to R3 master requests with state packet
//
// Optimized algorithm — Weighted Band-Power Accumulator:
//   Each FFT frame (1024 samples @ 256 Hz = 4 s) computes power in all 5
//   EEG bands. A sliding window of 8 frames (~32 s total) accumulates band
//   power. The winning band drives the state. A 3-frame hysteresis gate and
//   40% confidence threshold prevent jitter. Artifact frames are discarded.
//
// I2C bus split:
//   Wire  (A4/A5)   → I2C slave bus shared with R3 master
//   Wire1 (SDA1/SCL1) → local master bus for SSD1306 OLED only
// ============================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "arduinoFFT.h"

// ── FFT ──────────────────────────────────────────────────────────────────────
#define SAMPLES            1024
#define SAMPLING_FREQ      256
#define EEG_PIN            A2
#define BIN_RES_INV        4    // SAMPLES/SAMPLING_FREQ — bins per Hz

// ── OLED (on Wire1, local bus) ────────────────────────────────────────────────
#define SCREEN_W   128
#define SCREEN_H   64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire1, -1);

// ── Buttons ───────────────────────────────────────────────────────────────────
#define BTN_MODE   2
#define BTN_UP     3
#define BTN_DOWN   4
#define BTN_SELECT 5
#define DEBOUNCE_MS 50

// ── Display modes ─────────────────────────────────────────────────────────────
enum DispMode { MODE_STATE = 0, MODE_ALARM = 1, MODE_COUNT = 2 };
enum AlarmFld { FLD_HOUR = 0, FLD_MIN = 1, FLD_BUF = 2, FLD_COUNT = 3 };

// ── Alarm settings (editable by user) ────────────────────────────────────────
volatile uint8_t alarmHour  = 6;
volatile uint8_t alarmMin   = 0;
volatile uint8_t bufferMin  = 30;

// ── Sleep state constants ─────────────────────────────────────────────────────
// Enum index → band name / label
// 0=Unknown 1=DeepSlp(Delta) 2=LtSleep(Theta) 3=Relaxed(Alpha) 4=Active(Beta) 5=Focused(Gamma)
const char* BAND_LBL[] = {"Unknown", "Deep Sleep", "Light Sleep", "Relaxed", "Active", "Focused"};

// ── Band-power accumulator ────────────────────────────────────────────────────
#define NUM_BANDS      5
#define WIN_SIZE       8     // frames to accumulate (~32 s)
#define HYSTERESIS_N   3     // consecutive agreements to accept state
#define CONF_THRESHOLD 40    // minimum confidence to apply hysteresis gate

// Bin ranges (inclusive): bin = freq_hz * BIN_RES_INV
//  Delta 0.5–4 Hz → bins  2–16
//  Theta 4–8 Hz   → bins 16–32
//  Alpha 8–12 Hz  → bins 32–48
//  Beta 12–30 Hz  → bins 48–120
//  Gamma 30–60 Hz → bins 120–240
const uint16_t BAND_BIN_LO[NUM_BANDS] = {  2,  16,  32,  48, 120 };
const uint16_t BAND_BIN_HI[NUM_BANDS] = { 16,  32,  48, 120, 240 };

double bandWindow[WIN_SIZE][NUM_BANDS];  // rolling power window
uint8_t winIdx      = 0;
uint8_t winFilled   = 0;

uint8_t candState   = 0;
uint8_t candCount   = 0;

// ── Shared volatile state (written by main loop, read by I2C ISR) ─────────────
volatile float   gFreq       = 0.0f;
volatile uint8_t gState      = 0;
volatile uint8_t gConf       = 0;

// ── FFT buffers ───────────────────────────────────────────────────────────────
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);

// ── Sampling state ────────────────────────────────────────────────────────────
unsigned int  sampPeriodUs;
unsigned long lastSampleUs = 0;
int           sampleIdx    = 0;

// ── Artifact rejection threshold (ADC counts, empirical) ─────────────────────
#define ARTIFACT_PEAK_THRESHOLD 3500.0

// ── UI state ─────────────────────────────────────────────────────────────────
DispMode  dispMode    = MODE_STATE;
AlarmFld  alarmField  = FLD_HOUR;
bool      prevBtn[4]  = {HIGH, HIGH, HIGH, HIGH};
unsigned long lastBtnMs[4] = {0, 0, 0, 0};
unsigned long tDisplay = 0;
#define DISP_MS 300

// ── I2C response buffer (10 bytes) ───────────────────────────────────────────
//  [0-3] float freq  [4] state  [5] conf  [6] alarmH  [7] alarmM  [8] bufMin  [9] 0x00
volatile uint8_t i2cBuf[10];

// ─────────────────────────────────────────────────────────────────────────────
// I2C slave callbacks
// ─────────────────────────────────────────────────────────────────────────────
void onI2CRequest() {
  Wire.write((uint8_t*)i2cBuf, 10);
}

void onI2CReceive(int) {
  while (Wire.available()) Wire.read();  // consume any incoming byte (command)
}

// ── Helper: pack latest state into i2cBuf (call from main loop) ──────────────
void packI2CBuf() {
  float f = gFreq;
  uint8_t tmp[4];
  memcpy(tmp, &f, 4);
  i2cBuf[0] = tmp[0]; i2cBuf[1] = tmp[1]; i2cBuf[2] = tmp[2]; i2cBuf[3] = tmp[3];
  i2cBuf[4] = gState;
  i2cBuf[5] = gConf;
  i2cBuf[6] = alarmHour;
  i2cBuf[7] = alarmMin;
  i2cBuf[8] = bufferMin;
  i2cBuf[9] = 0x00;
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  analogReadResolution(12);
  sampPeriodUs = (unsigned int)(1000000.0 / SAMPLING_FREQ);

  // ── I2C: Wire1 = local OLED master ───────────────────────────────────────
  Wire1.begin();
  delay(200);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0); display.println(F("RhythmSleep"));
  display.setCursor(0, 12); display.println(F("EEG Engine Ready"));
  display.display();

  // ── I2C: Wire = slave to R3 master ───────────────────────────────────────
  Wire.begin(0x08);
  Wire.onRequest(onI2CRequest);
  Wire.onReceive(onI2CReceive);

  // ── Buttons ───────────────────────────────────────────────────────────────
  pinMode(BTN_MODE,   INPUT_PULLUP);
  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  // Initialise band-power window to zero
  memset(bandWindow, 0, sizeof(bandWindow));
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // ── Non-blocking EEG sampling ─────────────────────────────────────────────
  unsigned long nowUs = micros();
  if (nowUs - lastSampleUs >= sampPeriodUs) {
    lastSampleUs = nowUs;
    vReal[sampleIdx] = (double)analogRead(EEG_PIN);
    vImag[sampleIdx] = 0.0;
    sampleIdx++;

    if (sampleIdx >= SAMPLES) {
      sampleIdx = 0;
      runFFT();
      packI2CBuf();
    }
  }

  // ── Buttons ───────────────────────────────────────────────────────────────
  handleButtons();

  // ── OLED ──────────────────────────────────────────────────────────────────
  if (millis() - tDisplay > DISP_MS) {
    tDisplay = millis();
    updateOLED();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Optimized FFT + band-power accumulator
// ─────────────────────────────────────────────────────────────────────────────
void runFFT() {
  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  // ── 1. Artifact rejection ─────────────────────────────────────────────────
  double peakMag = 0;
  for (int i = 2; i < 241; i++)
    if (vReal[i] > peakMag) peakMag = vReal[i];
  if (peakMag > ARTIFACT_PEAK_THRESHOLD) return;  // discard noisy frame

  // ── 2. Dominant peak frequency (for display) ──────────────────────────────
  int peakBin = 2;
  for (int i = 2; i < 241; i++)
    if (vReal[i] > vReal[peakBin]) peakBin = i;
  gFreq = (float)peakBin * SAMPLING_FREQ / (float)SAMPLES;

  // ── 3. Compute per-band power for this frame ──────────────────────────────
  double framePow[NUM_BANDS] = {0};
  for (uint8_t b = 0; b < NUM_BANDS; b++) {
    for (uint16_t bin = BAND_BIN_LO[b]; bin < BAND_BIN_HI[b]; bin++) {
      double m = vReal[bin];
      framePow[b] += m * m;
    }
  }

  // ── 4. Store in rolling window ────────────────────────────────────────────
  for (uint8_t b = 0; b < NUM_BANDS; b++)
    bandWindow[winIdx][b] = framePow[b];
  winIdx = (winIdx + 1) % WIN_SIZE;
  if (winFilled < WIN_SIZE) winFilled++;

  // ── 5. Accumulate window power ────────────────────────────────────────────
  double cumPow[NUM_BANDS] = {0};
  double totalPow = 0;
  for (uint8_t f = 0; f < winFilled; f++)
    for (uint8_t b = 0; b < NUM_BANDS; b++) {
      cumPow[b] += bandWindow[f][b];
      totalPow  += bandWindow[f][b];
    }

  if (totalPow < 1e-6) return;

  // ── 6. Find winning band ──────────────────────────────────────────────────
  uint8_t winBand = 0;
  for (uint8_t b = 1; b < NUM_BANDS; b++)
    if (cumPow[b] > cumPow[winBand]) winBand = b;

  // Confidence = winning band share of total power
  uint8_t conf = (uint8_t)constrain((cumPow[winBand] / totalPow) * 100.0, 0, 100);

  // Map band index → state enum (0=Delta→1=Deep ... 4=Gamma→5=Focused)
  uint8_t newState = winBand + 1;   // bands 0-4 → states 1-5

  // ── 7. Hysteresis gate ────────────────────────────────────────────────────
  if (newState == candState) {
    candCount++;
  } else {
    candState = newState;
    candCount = 1;
  }

  if (candCount >= HYSTERESIS_N && conf >= CONF_THRESHOLD) {
    gState = newState;
    gConf  = conf;
  } else if (conf < CONF_THRESHOLD) {
    // Low confidence → report unknown
    gState = 0;
    gConf  = conf;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Button handling
// ─────────────────────────────────────────────────────────────────────────────
bool btnPressed(int pin, int idx) {
  bool rd = digitalRead(pin);
  bool pressed = (rd == LOW && prevBtn[idx] == HIGH &&
                  millis() - lastBtnMs[idx] > DEBOUNCE_MS);
  if (rd != prevBtn[idx]) lastBtnMs[idx] = millis();
  prevBtn[idx] = rd;
  return pressed;
}

void handleButtons() {
  if (btnPressed(BTN_MODE, 0)) {
    dispMode   = (DispMode)((dispMode + 1) % MODE_COUNT);
    alarmField = FLD_HOUR;
  }

  if (dispMode == MODE_ALARM) {
    if (btnPressed(BTN_SELECT, 3))
      alarmField = (AlarmFld)((alarmField + 1) % FLD_COUNT);

    if (btnPressed(BTN_UP, 1)) {
      switch (alarmField) {
        case FLD_HOUR: alarmHour   = (alarmHour + 1) % 24; break;
        case FLD_MIN:  alarmMin    = (alarmMin  + 1) % 60; break;
        case FLD_BUF:  bufferMin   = min(bufferMin + 5, 120); break;
        default: break;
      }
    }

    if (btnPressed(BTN_DOWN, 2)) {
      switch (alarmField) {
        case FLD_HOUR: alarmHour   = (alarmHour + 23) % 24; break;
        case FLD_MIN:  alarmMin    = (alarmMin  + 59) % 60; break;
        case FLD_BUF:  bufferMin   = max((int)bufferMin - 5, 5); break;
        default: break;
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// OLED display  (no time — time lives on R3 LCD)
// ─────────────────────────────────────────────────────────────────────────────
void updateOLED() {
  display.clearDisplay();

  if (dispMode == MODE_STATE) drawStateScreen();
  else                        drawAlarmScreen();

  // Bottom tab bar
  display.drawLine(0, 55, 127, 55, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(8,  57); display.print(dispMode == MODE_STATE ? F(">STATE") : F(" STATE"));
  display.setCursor(76, 57); display.print(dispMode == MODE_ALARM ? F(">ALARM") : F(" ALARM"));

  display.display();
}

void drawStateScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("Brain State"));

  // Frequency
  display.setTextSize(2);
  display.setCursor(0, 12);
  display.print(gFreq, 1);
  display.setTextSize(1);
  display.print(F(" Hz"));

  // Band label
  display.setCursor(0, 31);
  display.print(BAND_LBL[gState < 6 ? gState : 0]);

  // Confidence bar
  display.setCursor(88, 31);
  char cbuf[6]; sprintf(cbuf, "%3d%%", gConf);
  display.print(cbuf);

  int barW = map(gConf, 0, 100, 0, 128);
  display.drawRect(0, 43, 128, 9, SSD1306_WHITE);
  display.fillRect(0, 43, barW, 9, SSD1306_WHITE);
}

void drawAlarmScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("Alarm Settings"));

  // Alarm time
  display.setCursor(0, 13);
  display.print(F("Wake: "));
  display.setTextSize(2);
  display.setCursor(36, 11);

  char hb[3], mb[3];
  sprintf(hb, "%02d", alarmHour);
  sprintf(mb, "%02d", alarmMin);

  if (alarmField == FLD_HOUR) {
    display.print("["); display.print(hb);
    display.print("]:"); display.print(mb);
  } else if (alarmField == FLD_MIN) {
    display.print(hb); display.print(":[");
    display.print(mb); display.print("]");
  } else {
    display.print(hb); display.print(":"); display.print(mb);
  }

  // Buffer
  display.setTextSize(1);
  display.setCursor(0, 32);
  if (alarmField == FLD_BUF) {
    display.print(F("Buffer:["));
    display.print(bufferMin);
    display.print(F("]min"));
  } else {
    display.print(F("Buffer: "));
    display.print(bufferMin);
    display.print(F(" min"));
  }

  display.setCursor(0, 44);
  display.print(F("UP/DN adj  SEL:next"));
}

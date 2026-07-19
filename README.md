# RhythmSleep — EEG Smart Alarm System

A **three-Arduino** EEG brainwave monitoring system with smart alarm functionality.
- The **Arduino UNO R3** owns the RTC and I2C crystal LCD, acting as the I2C master and alarm engine.
- The **R4 Minima** samples EEG signals, runs an optimized FFT sleep-state algorithm, and drives the OLED + buttons — all as an I2C slave.
- The **UNO Q** is an I2C slave that bridges data to the Python server via USB Serial for CSV logging and the web dashboard.

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Shared I2C Bus (3.3 V level)                 │
│  SDA ────────────────────────────────────────────────────────   │
│  SCL ────────────────────────────────────────────────────────   │
│    │                       │                        │           │
│    ▼                       ▼                        ▼           │
│ [R3 MASTER]          [R4 SLAVE 0x08]         [UNO Q SLAVE 0x09] │
│ PCF8563 RTC          FFT Engine              Python bridge      │
│ LCD display          OLED + Buttons          USB Serial         │
│ Alarm logic          EEG input A2            → Python server    │
└─────────────────────────────────────────────────────────────────┘
```

### Responsibility Split

| Board | I2C Role | Key Responsibility |
|---|---|---|
| **Arduino UNO R3** | Master | RTC · I2C LCD (time) · Smart alarm · Polls R4 every 2 s |
| **R4 Minima** | Slave `0x08` | EEG sampling · Optimized FFT · OLED (state+alarm) · Buttons |
| **UNO Q** | Slave `0x09` | Serial bridge: I2C packets → Python CSV/dashboard |

### Data Flow

```
EEG Sensor ──[A2]──► R4 Minima
                       │ Band-power FFT (8-frame window ~32 s)
                       │ Sleep state + confidence
                       │ Alarm settings (set via buttons)
                  ─────[I2C @ 0x08]─────
                       │
                       ▼
                    R3 Master (polls every 2 s)
                       │ Reads: freq, state, confidence, alarm H/M/buffer
                       │ Checks: is current time in alarm buffer window?
                       │ Checks: is sleep state ≥ Light Sleep?
                       ├── YES → Trigger buzzer + LED
                       │
                  ─────[I2C @ 0x09]─────
                       │
                       ▼
                    UNO Q (I2C slave)
                       │ Formats CSV line / WAKE / SET_ALARM strings
                  ─────[USB Serial]─────
                       │
                       ▼
                    Python Server
                       ├── Log to CSV (sleep_log_YYYY-MM-DD.csv)
                       └── Web dashboard on port 8000
```

---

## I2C Address Map

| Device | Address | Bus |
|---|---|---|
| PCF8563 RTC | `0x51` | Shared (on R3) |
| LCD I2C backpack (PCF8574) | `0x27` (try `0x3F`) | Shared (on R3) |
| R4 Minima slave | `0x08` | Shared |
| UNO Q slave | `0x09` | Shared |
| SSD1306 OLED | `0x3C` | R4 local Wire1 only |

---

## Complete Wiring

### Shared I2C Bus

All boards share **one I2C bus**. Connect SDA and SCL in parallel across all boards and devices.

```
                    4.7 kΩ   4.7 kΩ
3V3 (R4 3V3 pin) ──┤ R ├──┬──┤ R ├──┬──
                           │         │
SDA ───────────────────────┴─────────┴──── (all SDA pins)
SCL ───────────────────────────────────── (all SCL pins)
```

> ⚠️ **Voltage warning:** The R4 Minima operates at **3.3 V logic**. The UNO R3 and UNO Q operate at **5 V logic**. You **must** use I2C level-shifter modules between the 5 V boards and the R4 Minima on the SDA/SCL lines to avoid damaging the R4. Power the pull-up resistors from the R4's **3V3** pin.

```
R3 A4 (SDA) ──[level shift]── R4 Minima A4 (SDA / Wire slave)
R3 A5 (SCL) ──[level shift]── R4 Minima A5 (SCL / Wire slave)

R3 A4 (SDA) ─────────────── UNO Q A4 (SDA)    ← same 5 V side, no shifter needed
R3 A5 (SCL) ─────────────── UNO Q A5 (SCL)

R3 A4 (SDA) ─────────────── PCF8563 SDA
R3 A5 (SCL) ─────────────── PCF8563 SCL

R3 A4 (SDA) ─────────────── LCD backpack SDA
R3 A5 (SCL) ─────────────── LCD backpack SCL

GND ──────────────────────── GND (all boards and modules — common ground)
```

### R4 Minima — OLED via SPI (separate from I2C bus)

The R4 Minima Arduino core does not expose `Wire1`. To avoid a bus conflict (the OLED can't share `Wire` while `Wire` is a slave), the OLED is wired via **hardware SPI** instead of I2C. Rewire an I2C OLED module to SPI, or use a module that has both interfaces.

```
R4 Minima D11 (MOSI) ── OLED SDA/MOSI pin
R4 Minima D13 (SCK)  ── OLED SCL/CLK pin
R4 Minima D10        ── OLED CS  pin
R4 Minima D9         ── OLED DC  pin
R4 Minima D8         ── OLED RST pin
R4 Minima 3V3        ── OLED VCC
R4 Minima GND        ── OLED GND
```

> SSD1306 modules labelled "4-pin I2C" are I2C-only. Use a **7-pin SSD1306 SPI module** (has CS, DC, RST, MOSI, CLK, VCC, GND pins).

---

## Pin Reference

### Arduino UNO R3

```
                    ┌──────────────────┐
                    │   Arduino UNO R3 │
                    │                  │
       LCD SDA ◄───►│ A4  (SDA)        │◄───► PCF8563 SDA
       LCD SCL ◄───►│ A5  (SCL)        │◄───► PCF8563 SCL
                    │    (+ to R4/UNO Q via level shifter)
                    │                  │
        Buzzer ◄────│ D8               │
           LED ◄────│ D13              │
                    │                  │
                    └──────────────────┘
```

| Pin | Function | Connection |
|---|---|---|
| **A4 (SDA)** | I2C Data — Master | LCD · PCF8563 · R4 slave · UNO Q slave |
| **A5 (SCL)** | I2C Clock — Master | Same bus |
| **D8** | Buzzer output | Active buzzer (+) → D8, (−) → GND |
| **D13** | LED output | LED + 220 Ω → D13, cathode → GND |
| **5V** | Power | PCF8563 VCC · LCD VCC |
| **GND** | Ground | All components |

### R4 Minima

```
                    ┌──────────────────┐
                    │   Arduino R4     │
                    │     Minima       │
                    │                  │
          EEG In ──►│ A2               │
                    │                  │
  [Shared bus]      │                  │  [Local OLED bus]
   R3 Master ◄─────►│ A4/SDA  SDA1 ◄──►│ OLED SDA
                   ►│ A5/SCL  SCL1 ◄──►│ OLED SCL
                    │                  │
     BTN_MODE ─────►│ D2 (INPUT_PULLUP)│
       BTN_UP ─────►│ D3 (INPUT_PULLUP)│
     BTN_DOWN ─────►│ D4 (INPUT_PULLUP)│
    BTN_SELECT ────►│ D5 (INPUT_PULLUP)│
                    │                  │
                    └──────────────────┘
```

| Pin | Function | Connection | Notes |
|---|---|---|---|
| **A2** | EEG analog input | EEG sensor output | 12-bit ADC, 256 Hz sampling |
| **A4 / SDA** | I2C slave bus (Wire) | Shared bus ← R3 master | Slave address `0x08` |
| **A5 / SCL** | I2C slave bus (Wire) | Shared bus ← R3 master | — |
| **SDA1** | Local I2C master (Wire1) | OLED SDA | OLED only, not shared |
| **SCL1** | Local I2C master (Wire1) | OLED SCL | OLED only, not shared |
| **D2** | MODE button | Button → GND | Cycles State/Alarm screens |
| **D3** | UP button | Button → GND | Increment alarm value |
| **D4** | DOWN button | Button → GND | Decrement alarm value |
| **D5** | SELECT button | Button → GND | Next alarm field |
| **3V3** | 3.3 V output | I2C pull-up resistors | Use for pull-ups on shared bus |
| **GND** | Ground | All components | Common ground |

### Arduino UNO Q

| Pin | Function | Connection |
|---|---|---|
| **A4 (SDA)** | I2C slave (Wire) | Shared bus ← R3 master, slave `0x09` |
| **A5 (SCL)** | I2C slave (Wire) | Shared bus |
| **USB-C** | Serial to Python | Python server on host/Linux |
| **GND** | Ground | Common ground |

### PCF8563 RTC Module (wired to R3)

| RTC Pin | Connect to |
|---|---|
| VCC | R3 5V |
| GND | R3 GND |
| SDA | R3 A4 |
| SCL | R3 A5 |
| INT | Not connected |

### SSD1306 OLED 128×64 (wired to R4 Wire1)

| OLED Pin | Connect to |
|---|---|
| VCC | R4 3V3 |
| GND | R4 GND |
| SDA | R4 **SDA1** |
| SCL | R4 **SCL1** |

### I2C Crystal LCD 16×2 (wired to R3)

| LCD Pin | Connect to |
|---|---|
| VCC | R3 5V |
| GND | R3 GND |
| SDA | R3 A4 |
| SCL | R3 A5 |

> Default I2C address is `0x27`. If the display doesn't initialize, try `0x3F` (change in `r3.ino` line: `LiquidCrystal_I2C lcd(0x27, 16, 2);`).

### 4 Tactile Buttons (on R4 Minima)

```
  D2 ──┤ MODE   ├── GND    cycles: State ↔ Alarm screens
  D3 ──┤ UP     ├── GND    increment selected field
  D4 ──┤ DOWN   ├── GND    decrement selected field
  D5 ──┤ SELECT ├── GND    move to next editable field
```

No resistors needed — internal pull-ups are enabled.

---

## I2C Packet Protocol

### R3 → R4 Minima

| Byte | Value | Meaning |
|---|---|---|
| 0 | `0x01` | Request state packet |

### R4 Minima → R3 (10-byte response)

| Bytes | Field |
|---|---|
| 0–3 | `float` dominant EEG frequency (Hz) |
| 4 | Sleep state: `0`=Unknown `1`=Deep `2`=Light `3`=Relaxed `4`=Active `5`=Focused |
| 5 | Confidence 0–100 (%) |
| 6 | Alarm hour (0–23) |
| 7 | Alarm minute (0–59) |
| 8 | Buffer minutes (5–120) |
| 9 | Reserved |

### R3 → UNO Q

| Cmd byte | Payload | Effect |
|---|---|---|
| `0xA1` | float(4B) + state(1B) + conf(1B) | UNO Q sends CSV line to Python |
| `0xA2` | none | UNO Q sends `WAKE` to Python |
| `0xA3` | alarmH + alarmM + buf | UNO Q sends `SET_ALARM:HH:MM,buf` to Python |

### UNO Q → Python (USB Serial)

```
7.25,Light Sleep,72       ← freq, band, confidence
WAKE
SET_ALARM:06:00,30
```

---

## Optimized FFT Algorithm (R4 Minima)

The new algorithm replaces single-peak detection with a **weighted band-power accumulator**:

1. **Frame**: 1024 samples @ 256 Hz = **4 seconds** of EEG
2. **Hamming window** applied before FFT
3. **Artifact rejection**: frames where peak magnitude > 3500 ADC counts are discarded entirely
4. **Per-band power**: sum of squared magnitudes across all bins in each band

| Band | Hz | Bins | → State |
|---|---|---|---|
| Delta | 0.5–4 | 2–16 | Deep Sleep |
| Theta | 4–8 | 16–32 | Light Sleep |
| Alpha | 8–12 | 32–48 | Relaxed |
| Beta | 12–30 | 48–120 | Active |
| Gamma | 30–60 | 120–240 | Focused |

5. **Sliding window**: 8 frames (~32 s) of per-band power accumulated
6. **Winner**: band with highest cumulative power across the window
7. **Confidence**: winner's share of total power × 100 (%)
8. **Hysteresis gate**: state only changes after **3 consecutive matching windows** with confidence **≥ 40%**

---

## Smart Alarm Logic (R3)

1. User sets **alarm time** (HH:MM) and **buffer** (5–120 min) via OLED buttons on the R4 Minima
2. R3 polls R4 every 2 s for the current sleep state, confidence, and alarm settings
3. **Buffer window** = `[alarm_time − buffer, alarm_time]`
4. During the buffer window:
   - If `sleepState ≥ 2` (Light Sleep, Relaxed, Active, or Focused) **AND** `confidence ≥ 35%` → alarm fires immediately
   - If sleep state is Deep Sleep, alarm waits for a lighter moment
5. At **exact alarm time**: alarm fires unconditionally regardless of sleep state
6. Alarm auto-dismisses after **2 minutes**
7. On trigger: buzzer (D8) + LED (D13) alternate at 400 ms; `WAKE` sent to UNO Q → Python

---

## OLED Display Modes (R4 Minima)

> Time is no longer shown on the OLED — it is displayed on the R3 LCD.

Cycle screens using the **MODE** button (D2):

| Screen | Content | Button Actions |
|---|---|---|
| **STATE** | Dominant frequency (Hz) · Band label · Confidence bar | — |
| **ALARM** | Editable alarm HH:MM · Buffer minutes | UP/DOWN adjust · SELECT next field |

---

## LCD Display (R3 — always on)

```
┌────────────────┐
│ 06:32:14 A06:00│   ← current time + alarm time
│ LtSleep    72% │   ← sleep state + confidence
└────────────────┘
```

When alarm is ringing:
```
┌────────────────┐
│ 06:00:03  WAKE!│
│ Relaxed    81% │
└────────────────┘
```

---

## Brainwave Classification

| Band | Frequency | State | Description |
|---|---|---|---|
| **Gamma** | 30–60 Hz | Focused | High cognitive activity |
| **Beta** | 12–30 Hz | Active | Alert, engaged |
| **Alpha** | 8–12 Hz | Relaxed | Calm, resting |
| **Theta** | 4–8 Hz | Light Sleep | Drowsy, transitional |
| **Delta** | 0.5–4 Hz | Deep Sleep | Deep restorative sleep |

---

## Setup Instructions

### 1. Install Arduino Libraries

Via **Arduino Library Manager**, install:

| Library | Used by |
|---|---|
| `RTClib` (Adafruit) | R3 |
| `LiquidCrystal I2C` (Frank de Brabander) | R3 |
| `Adafruit SSD1306` | R4 Minima |
| `Adafruit GFX Library` | R4 Minima |
| `arduinoFFT` | R4 Minima |

### 2. Upload Firmware

| File | Upload to |
|---|---|
| `r3.ino` | Arduino UNO R3 |
| `r4Minina/r4minima.ino` | Arduino R4 Minima |
| `UNOQ/data_relayer.ino` | UNO Q |

### 3. Wire Hardware

Follow the pinout tables and wiring diagram above. Ensure:
- Common GND across all boards
- Level shifters on SDA/SCL between R3/UNO Q (5 V) and R4 Minima (3.3 V)
- Pull-up resistors (4.7 kΩ × 2) on SDA and SCL, tied to R4 Minima's **3V3**

### 4. Run Python Server

```bash
sudo python3 eeg_server.py
```

### 5. Open Dashboard

Navigate to `http://<device-ip>:8000` in your browser.

---

## CSV Log Format

Files are saved as `sleep_log_YYYY-MM-DD.csv`:

```csv
Timestamp,Frequency,State,Confidence
2026-07-19 23:15:01,2.50,Deep Sleep,78
2026-07-19 23:15:05,5.75,Light Sleep,64
2026-07-19 23:15:09,9.00,Relaxed,71
```

---

## API Endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/api/files` | GET | List all CSV log files |
| `/api/data?file=X` | GET | Get timestamps, frequencies, states from a CSV file |
| `/api/live` | GET | Current frequency, state, alarm status (real-time) |
| `/api/alarm` | GET | Get current alarm settings |
| `/api/alarm` | POST | Set alarm (`alarm_m`, `buffer_m` in JSON body) |

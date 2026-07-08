# RhythmSleep — EEG Smart Alarm System

A dual-Arduino EEG brainwave monitoring system with smart alarm functionality. The R4 Minima reads EEG signals, runs FFT analysis, and manages the OLED display. The UNO Q (STM32) classifies brainwave bands and its Linux environment runs the Python server for smart alarm logic, MP3 playback, CSV logging, and the web dashboard.

---

## Complete Pinout

### Arduino R4 Minima

```
                    ┌──────────────────┐
                    │   Arduino R4     │
                    │     Minima       │
                    │                  │
          EEG In ──►│ A2               │
                    │                  │
      RTC SDA ◄────►│ SDA (I2C)        │◄────► OLED SDA
      RTC SCL ◄────►│ SCL (I2C)        │◄────► OLED SCL
                    │                  │
    BTN_MODE ──────►│ D2 (INPUT_PULLUP)│ ◄── to GND via button
      BTN_UP ──────►│ D3 (INPUT_PULLUP)│ ◄── to GND via button
    BTN_DOWN ──────►│ D4 (INPUT_PULLUP)│ ◄── to GND via button
  BTN_SELECT ──────►│ D5 (INPUT_PULLUP)│ ◄── to GND via button
                    │                  │
      UNO Q RX ◄────│ TX1 (Serial1)    │
      UNO Q TX ────►│ RX1 (Serial1)    │
                    │                  │
       POWER USB ◄──│                  │ 
                    └──────────────────┘
```

### Pin Table — R4 Minima

| Pin | Function | Connection | Notes |
|-----|----------|-----------|-------|
| **A2** | Analog Input | EEG Sensor output | 12-bit ADC, 256 Hz sampling |
| **SDA** | I2C Data | PCF8563 RTC + SSD1306 OLED | Shared I2C bus |
| **SCL** | I2C Clock | PCF8563 RTC + SSD1306 OLED | Shared I2C bus |
| **D2** | Digital Input | MODE button → GND | INPUT_PULLUP, cycles Time/State/Alarm |
| **D3** | Digital Input | UP button → GND | INPUT_PULLUP, increments value |
| **D4** | Digital Input | DOWN button → GND | INPUT_PULLUP, decrements value |
| **D5** | Digital Input | SELECT button → GND | INPUT_PULLUP, toggles field |
| **TX1** | Hardware Serial1 TX | UNO Q RX | 115200 baud |
| **RX1** | Hardware Serial1 RX | UNO Q TX | 115200 baud |
| **USB** | Hardware Serial | PC / Debug Monitor | 115200 baud |
| **5V** | Power | RTC VCC, OLED VCC | — |
| **3.3V** | Power (alt) | Can power OLED if 3.3V variant | — |
| **GND** | Ground | All components GND | Common ground |

### Pin Table — Arduino UNO Q (STM32)

| Pin | Function | Connection | Notes |
|-----|----------|-----------|-------|
| **RX** | Hardware Serial1 RX | R4 Minima TX1 | 115200 baud |
| **TX** | Hardware Serial1 TX | R4 Minima RX1 | 115200 baud |
| **USB-C** | Serial to Linux | Python server (internal bridge) | — |
| **Audio Out** | Speaker / 3.5mm | MP3 playback via Linux `mpg123` | — |

### PCF8563 RTC Module

| RTC Pin | Connection |
|---------|-----------|
| VCC | Minima 5V (or 3.3V) |
| GND | Minima GND |
| SDA | Minima SDA |
| SCL | Minima SCL |
| INT | Not connected (optional) |

### SSD1306 OLED (128×64, I2C)

| OLED Pin | Connection |
|----------|-----------|
| VCC | Minima 5V (or 3.3V) |
| GND | Minima GND |
| SDA | Minima SDA |
| SCL | Minima SCL |

> **Note:** RTC and OLED share the same I2C bus. Default addresses: OLED = `0x3C`, RTC = `0x51`.

### 4 Tactile Buttons

Each button has one leg connected to the designated pin and the other leg to GND. No external resistors needed (internal pull-ups used).

```
  Pin D2 ──┤ BTN_MODE   ├── GND     (Cycle: Time → State → Alarm)
  Pin D3 ──┤ BTN_UP     ├── GND     (Increment value in Alarm mode)
  Pin D4 ──┤ BTN_DOWN   ├── GND     (Decrement value in Alarm mode)
  Pin D5 ──┤ BTN_SELECT ├── GND     (Toggle alarm field: Hour/Min/Buffer)
```

---

## Architecture & Data Flow

```
EEG Sensor ──[A2]──► R4 Minima (data acquisition only)
                      │
                      ├── FFT (1024 samples, 256 Hz)
                      ├── Extract dominant frequency (0.5–100 Hz)
                      ├── Send "FREQ:XX.XX" via Serial1
                      ├── Display on OLED (3 modes)
                      └── Forward alarm settings from buttons
                      │
              ────[TX1/RX1]────
                      │
                      ▼
                   UNO Q STM32 (classifier + relay)
                      │
                      ├── Parse FREQ:XX.XX
                      ├── Classify brainwave band
                      ├── Output "FREQ,Band" to Linux Serial
                      └── Relay commands both directions
                      │
              ────[USB-C Internal]────
                      │
                      ▼
               Python Server (on UNO Q Linux)
                      │
                      ├── Log to CSV (sleep_log_YYYY-MM-DD.csv)
                      ├── Smart alarm logic (rolling 30s window)
                      ├── MP3 playback via mpg123
                      ├── Sends WAKE → UNO Q → Minima OLED
                      └── Web dashboard on port 8000
```

### Responsibility Split

| Component | Role |
|-----------|------|
| **R4 Minima** | FFT + dominant frequency + OLED display + button input. No alarm logic. |
| **UNO Q STM32** | Classify frequency into brainwave band + bidirectional relay |
| **Python Server (UNO Q Linux)** | CSV logging + smart alarm + MP3 playback + web dashboard |

---

## Brainwave Classification

| Band | Frequency Range | State Label | Description |
|------|----------------|-------------|-------------|
| **Gamma** | 30–100 Hz | Focused | High cognitive activity |
| **Beta** | 12–30 Hz | Active | Alert, engaged |
| **Alpha** | 8–12 Hz | Relaxed | Calm, resting |
| **Theta** | 4–8 Hz | Light Sleep | Drowsy, light sleep |
| **Delta** | 0.5–4 Hz | Deep Sleep | Deep restorative sleep |

---

## OLED Display Modes

Cycle through modes using the **MODE** button (D2):

| Mode | Screen Content | Button Actions |
|------|---------------|----------------|
| **Time** | Large clock (HH:MM:SS), date, alarm time | — |
| **State** | Dominant frequency (Hz), band name, visual bar | — |
| **Alarm** | Alarm hour/minute editor, buffer editor | UP/DOWN adjust, SELECT toggles field |

---

## Smart Alarm Logic (runs on Python server)

1. User sets **alarm time** (hour:minute) and **buffer** (minutes) via OLED buttons or web UI
2. During the alarm window `[alarm - buffer, alarm + buffer]`:
   - System tracks the **highest frequency** over a rolling 30-second window
   - When dominant freq matches the highest (within ±5 Hz) for **≥5 seconds** → plays `openingtone.mp3` via `mpg123`
   - If frequency remains stable (±5 Hz) → plays `alarm.mp3` on loop
3. **Failsafe**: If the end of the buffer window is reached, alarm fires regardless
4. When alarm fires, Python sends `WAKE` to Minima → OLED shows "RING!"

---

## Setup Instructions

### 1. Install Arduino Libraries
Via Arduino Library Manager, install:
- `RTClib` (Adafruit)
- `arduinoFFT`
- `Adafruit SSD1306`
- `Adafruit GFX Library`

### 2. Upload Firmware
1. Upload `r4Minina/EECreader.ino` to the Arduino R4 Minima
2. Upload `UNOQ/data_relayer.ino` to the UNO Q STM32 (via Arduino IDE)

### 3. Connect Hardware
Wire all components per the pinout tables above. Ensure common GND between all.

### 4. Place MP3 Files on UNO Q
Copy to the RhythmSleep directory on UNO Q's Linux filesystem:
- `openingtone.mp3` — played after 5 seconds of high frequency
- `alarm.mp3` — played on loop when alarm triggers

### 5. Install mpg123 on UNO Q
```bash
apt-get install mpg123
```

### 6. Run Server
```bash
# Connect to UNO Q via ADB or SSH
adb shell
cd RhythmSleep
sudo python3 eeg_server.py
```

### 7. Open Dashboard
Navigate to `http://<device-ip>:8000` in your browser.

---

## CSV Log Format

Files are saved as `sleep_log_YYYY-MM-DD.csv`:

```csv
Timestamp,Frequency,State
2026-06-28 23:15:01,2.50,Deep Sleep
2026-06-28 23:15:05,5.75,Light Sleep
2026-06-28 23:15:09,9.00,Relaxed
```

---

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/files` | GET | List all CSV log files |
| `/api/data?file=X` | GET | Get timestamps, frequencies, states from a CSV file |
| `/api/live` | GET | Current frequency, state, alarm status (real-time) |
| `/api/alarm` | GET | Get current alarm settings |
| `/api/alarm` | POST | Set alarm (`alarm_m`, `buffer_m` in JSON body) |

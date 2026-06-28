// ============================================================================
// RhythmSleep — UNO Q (STM32) Classifier & Data Relay
// ============================================================================
// Receives dominant frequency from R4 Minima via Serial1,
// classifies into brainwave band, formats CSV-ready data,
// and relays to the Python server running on the Linux environment.
//
// Also relays SET_ALARM commands from Minima buttons → Python server,
// and WAKE/SET_TIME commands from Python server → Minima.
//
// Serial  = Bridge to internal Debian Linux (Python server)
// Serial1 = Bridge to R4 Minima physical TX/RX pins
// ============================================================================

// Brainwave band thresholds
#define GAMMA_MIN  30.0   // Focused: 30-100 Hz
#define BETA_MIN   12.0   // Active:  12-30 Hz
#define ALPHA_MIN   8.0   // Relaxed: 8-12 Hz
#define THETA_MIN   4.0   // Light Sleep: 4-8 Hz
#define DELTA_MIN   0.5   // Deep Sleep: 0.5-4 Hz

void setup() {
  // Bridge to the internal Debian Linux environment
  Serial.begin(115200);
  
  // Bridge to the external Arduino R4 Minima physical RX/TX pins
  Serial1.begin(115200);

  Serial.println("UNOQ BOOT: Classifier + Relay active.");
}

void loop() {
  // ── 1. Receive data from Minima ──────────────────────────────────────────
  if (Serial1.available() > 0) {
    String sensorData = Serial1.readStringUntil('\n');
    sensorData.trim();

    // Frequency reading: "FREQ:XX.XX"
    if (sensorData.startsWith("FREQ:")) {
      float frequency = sensorData.substring(5).toFloat();
      String band = classifyBand(frequency);

      // Send classified data to Python: "FREQ,Band"
      Serial.print(frequency, 2);
      Serial.print(",");
      Serial.println(band);
    }
    // Alarm settings from Minima buttons: "SET_ALARM:alarm_m,buffer_m"
    else if (sensorData.startsWith("SET_ALARM:")) {
      Serial.println(sensorData);  // Forward to Python server
    }
    // Forward other messages (ERROR, etc.) as-is
    else {
      Serial.println(sensorData);
    }
  }

  // ── 2. Forward Commands: Linux → Minima ──────────────────────────────────
  // Relay WAKE, SET_TIME, SET_ALARM commands from Python to Minima
  if (Serial.available() > 0) {
    String linuxCommand = Serial.readStringUntil('\n');
    Serial1.println(linuxCommand);
  }
}

// ============================================================================
// BRAINWAVE CLASSIFICATION
// ============================================================================
String classifyBand(float freq) {
  if (freq >= GAMMA_MIN) return "Focused";       // Gamma: 30-100 Hz
  if (freq >= BETA_MIN)  return "Active";         // Beta:  12-30 Hz
  if (freq >= ALPHA_MIN) return "Relaxed";        // Alpha: 8-12 Hz
  if (freq >= THETA_MIN) return "Light Sleep";    // Theta: 4-8 Hz
  if (freq >= DELTA_MIN) return "Deep Sleep";     // Delta: 0.5-4 Hz
  return "Unknown";
}

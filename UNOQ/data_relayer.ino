void setup() {
  // Bridge to the internal Debian Linux environment
  Serial.begin(115200);
  
  // Bridge to the external Arduino R4 Minima physical RX/TX pins
  Serial1.begin(115200);
}

void loop() {
  // 1. Forward Data: Minima -> Linux
  if (Serial1.available() > 0) {
    String sensorData = Serial1.readStringUntil('\n');
    Serial.println(sensorData);
  }

  // 2. Forward Commands: Linux -> Minima
  // If the Python server triggers the "WAKE" command
  if (Serial.available() > 0) {
    String linuxCommand = Serial.readStringUntil('\n');
    Serial1.println(linuxCommand);
  }
}

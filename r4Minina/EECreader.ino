#include <Wire.h>
#include "RTClib.h"
#include "arduinoFFT.h"

#define SAMPLES 1024            
#define SAMPLING_FREQUENCY 256  
#define EEG_PIN A2

arduinoFFT FFT = arduinoFFT();
RTC_PCF8563 rtc;

unsigned int sampling_period_us;
unsigned long microseconds;
double vReal[SAMPLES];
double vImag[SAMPLES];
bool alarmTriggered = false;

void setup() {
  Serial1.begin(115200);
  Wire.begin();
  
  if (!rtc.begin()) {
    Serial1.println("ERROR: RTC_NOT_FOUND");
    while (1); // Halt if I2C matrix fails to initialize
  }

  sampling_period_us = round(1000000.0 / SAMPLING_FREQUENCY);
  analogReadResolution(12);
}

void loop() {
  if (alarmTriggered) {
    delay(10000);
    return; 
  }

  // 1. Acquire Biological Data
  for (int i = 0; i < SAMPLES; i++) {
    microseconds = micros();
    vReal[i] = analogRead(EEG_PIN);
    vImag[i] = 0; 
    while (micros() - microseconds < sampling_period_us) {}
  }

  FFT.Windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.Compute(vReal, vImag, SAMPLES, FFT_FORWARD);
  FFT.ComplexToMagnitude(vReal, vImag, SAMPLES);

  double delta = 0, theta = 0, alpha = 0, beta = 0;
  for (int i = 1; i < (SAMPLES / 2); i++) {
    double freq = (i * 1.0 * SAMPLING_FREQUENCY) / SAMPLES;
    if (freq >= 0.5 && freq <= 4.0) delta += vReal[i];
    else if (freq > 4.0 && freq <= 8.0) theta += vReal[i];
    else if (freq > 8.0 && freq <= 13.0) alpha += vReal[i];
    else if (freq > 13.0 && freq <= 30.0) beta += vReal[i];
  }

  // 2. Fetch Absolute Hardware Coordinate
  DateTime now = rtc.now();
  char timeBuffer[25];
  sprintf(timeBuffer, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());

  // 3. Transmit Fused Payload
  Serial1.print(timeBuffer);
  Serial1.print(",");
  
  if (delta > theta && delta > alpha && delta > beta) Serial1.println("Deep");
  else if (theta > delta && theta > alpha && theta > beta) Serial1.println("Core"); 
  else if (alpha > theta && alpha > beta) Serial1.println("REM");  
  else Serial1.println("Awake"); 

  // 4. Process Incoming Routing Commands
  if (Serial1.available() > 0) {
    String command = Serial1.readStringUntil('\n');
    command.trim();
    
    if (command.indexOf("WAKE") >= 0) {
      alarmTriggered = true;
    } 
    else if (command.indexOf("SET_TIME:") >= 0) {
      int y, m, d, h, mn, s;
      // Extract specific integer variables from the string vector
      sscanf(command.substring(9).c_str(), "%d-%d-%d %d:%d:%d", &y, &m, &d, &h, &mn, &s);
      rtc.adjust(DateTime(y, m, d, h, mn, s));
    }
  }

  delay(1000); 
}

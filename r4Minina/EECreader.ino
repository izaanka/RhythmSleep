#include <Wire.h>
#include "RTClib.h"
#include "arduinoFFT.h"

#define SAMPLES 1024            
#define SAMPLING_FREQUENCY 256  
#define EEG_PIN A2

RTC_PCF8563 rtc;

unsigned int sampling_period_us;
unsigned long microseconds;

double vReal[SAMPLES];
double vImag[SAMPLES];

ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQUENCY);

bool alarmTriggered = false;

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  
  // 1. Force the processor to wait for the PC to open the terminal
  while (!Serial) {
    ; // Do nothing until the USB bridge is fully established
  }

  Serial.println("SYSTEM BOOT: Initializing I2C Matrix...");
  Wire.begin();
  
  // 2. Hardware Verification
  if (!rtc.begin()) {
    Serial.println("CRITICAL ERROR: PCF8563 RTC NOT FOUND ON I2C BUS.");
    Serial.println("HALTING PROCESSOR.");
    Serial1.println("ERROR: RTC_NOT_FOUND");
    while (1); // The infinite logic trap
  }

  Serial.println("RTC ALIGNED. Initializing Biological ADC...");
  sampling_period_us = round(1000000.0 / SAMPLING_FREQUENCY);
  analogReadResolution(12);
}

void loop() {
  if (alarmTriggered) {
    delay(10000);
    return; 
  }

  // Acquire Biological Data
  for (int i = 0; i < SAMPLES; i++) {
    microseconds = micros();
    vReal[i] = analogRead(EEG_PIN);
    vImag[i] = 0; 
    while (micros() - microseconds < sampling_period_us) {}
  }

  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  double delta = 0, theta = 0, alpha = 0, beta = 0;
  for (int i = 1; i < (SAMPLES / 2); i++) {
    double freq = (i * 1.0 * SAMPLING_FREQUENCY) / SAMPLES;
    if (freq >= 0.5 && freq <= 4.0) delta += vReal[i];
    else if (freq > 4.0 && freq <= 8.0) theta += vReal[i];
    else if (freq > 8.0 && freq <= 13.0) alpha += vReal[i];
    else if (freq > 13.0 && freq <= 30.0) beta += vReal[i];
  }

  // Fetch Absolute Hardware Coordinate
  DateTime now = rtc.now();
  char timeBuffer[25];
  sprintf(timeBuffer, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());

  // Determine Neurological String
  String currentState;
  if (delta > theta && delta > alpha && delta > beta) currentState = "Deep";
  else if (theta > delta && theta > alpha && theta > beta) currentState = "Core"; 
  else if (alpha > theta && alpha > beta) currentState = "REM";  
  else currentState = "Awake"; 

  // Route 1: Transmit Fused Payload strictly to UNO Q hardware pins
  Serial1.print(timeBuffer);
  Serial1.print(",");
  Serial1.println(currentState);

  // Route 2: Transmit Diagnostic Payload strictly to PC Serial Monitor
  Serial.print("PC MONITOR -> Timestamp: ");
  Serial.print(timeBuffer);
  Serial.print(" | State: ");
  Serial.println(currentState);

  // Process Incoming Routing Commands from UNO Q
  if (Serial1.available() > 0) {
    String command = Serial1.readStringUntil('\n');
    command.trim();
    
    // Echo received commands to PC Monitor
    Serial.print("RECEIVED FROM UNO Q: ");
    Serial.println(command);
    
    if (command.indexOf("WAKE") >= 0) {
      alarmTriggered = true;
      Serial.println("ALARM TRIGGERED: Halting execution loop.");
    } 
    else if (command.indexOf("SET_TIME:") >= 0) {
      int y, m, d, h, mn, s;
      sscanf(command.substring(9).c_str(), "%d-%d-%d %d:%d:%d", &y, &m, &d, &h, &mn, &s);
      rtc.adjust(DateTime(y, m, d, h, mn, s));
      Serial.println("RTC CALIBRATED VIA NETWORK TIME PROTOCOL.");
    }
  }

  delay(1000); 
}

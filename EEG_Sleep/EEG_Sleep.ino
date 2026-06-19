#include "arduinoFFT.h"

#define SAMPLES 1024            
#define SAMPLING_FREQUENCY 256  
#define EEG_PIN A0

arduinoFFT FFT = arduinoFFT();

unsigned int sampling_period_us;
unsigned long microseconds;

double vReal[SAMPLES];
double vImag[SAMPLES];

// Flag to control the processing loop
bool alarmTriggered = false;

void setup() {
  Serial.begin(115200);
  sampling_period_us = round(1000000.0 / SAMPLING_FREQUENCY);
  analogReadResolution(12); 
}

void loop() {
  // Halt all execution if the Linux MPU sent the wake command
  if (alarmTriggered) {
    delay(10000);
    return; 
  }

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

  Serial.print("State: ");
  if (delta > theta && delta > alpha && delta > beta) {
    Serial.println("Deep Sleep");
  } else if (theta > delta && theta > alpha && theta > beta) {
    Serial.println("Light Sleep / REM");
  } else if (alpha > theta && alpha > beta) {
    Serial.println("Relaxed Awake");
  } else {
    Serial.println("Active / Focused");
  }

  // Check for incoming commands from the Python script
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    if (command.indexOf("WAKE") >= 0) {
      alarmTriggered = true; // Stop processing
    }
  }

  delay(1000); 
}

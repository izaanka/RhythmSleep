#include "arduinoFFT.h"

#define SAMPLES 1024            
#define SAMPLING_FREQUENCY 256  
#define EEG_PIN A2

arduinoFFT FFT = arduinoFFT();
unsigned int sampling_period_us;
unsigned long microseconds;

double vReal[SAMPLES];
double vImag[SAMPLES];

bool alarmTriggered = false;

void setup() {
  // Serial1 specifically targets the physical TX/RX pins on the board edge
  Serial1.begin(115200);
  sampling_period_us = round(1000000.0 / SAMPLING_FREQUENCY);
  analogReadResolution(12);
}

void loop() {
  if (alarmTriggered) {
    delay(10000);
    return; 
  }

  // BioAmp EXG Pill Data Acquisition
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

  // Transmit the payload over the hardware TX pin
  Serial1.print("State: ");
  if (delta > theta && delta > alpha && delta > beta) {
    Serial1.println("Deep");
  } else if (theta > delta && theta > alpha && theta > beta) {
    Serial1.println("Core"); 
  } else if (alpha > theta && alpha > beta) {
    Serial1.println("REM");  
  } else {
    Serial1.println("Awake"); 
  }

  // Listen on the RX pin for the WAKE command sent back from the UNO Q
  if (Serial1.available() > 0) {
    String command = Serial1.readStringUntil('\n');
    if (command.indexOf("WAKE") >= 0) {
      alarmTriggered = true;
    }
  }

  delay(1000); 
}

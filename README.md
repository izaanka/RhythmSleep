CONNECTIONS

A2 - EEG input


TO RUN

first upload code to STM32 using arduino app lab or arduino IDE (prefered) 
analog pin A2 is connected to eeg sensor for input

install adb (android debug bridge) on your machine 

connect arduino uno q to your machine via usb c (data cable)

run commands 
1. adb shell
   or just connect to the uno q using any CLI
2. cd RhythmSleep
3. python3 eeg_server.py
   or just directly python3 RhythmSleep/eeg_server.py

the data is collected by pins on the uno Q


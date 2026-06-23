CONNECTIONS

1. A2 - EEG input
2. Minima TX (Transmit) - UNO Q RX (Receive)
3. Minima RX (Receive) - UNO Q TX (Transmit)
4. RTC SCL - minima SCL
5. RTC SDA - minima SDA

TO RUN
1. upload EEGreader.ino to minima
2. then upload code to STM32 using arduino app lab or arduino IDE (prefered) data_relayer.ino
3. analog pin A2 is connected to eeg sensor for input
4. install adb (android debug bridge) on your machine
5. connect arduino uno q to your machine via usb c (data cable)

run commands 
1. adb shell
   or just connect to the uno q using any CLI
2. cd RhythmSleep
3. sudo python3 eeg_server.py
   or just directly sudo python3 RhythmSleep/eeg_server.py
   (sudo is to grant the environment kernel-level permissions to modify the system clock via the 'date' command if the network time protocol fails.)
the data is collected by pins on the mimima


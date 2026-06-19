import serial
import csv
from datetime import datetime, timedelta
import subprocess
import time

SERIAL_PORT = '/dev/ttyACM0' 
BAUD_RATE = 115200

# User Settings
TARGET_WAKE_TIME = "05:30" 
BUFFER_MINUTES = 30
ALARM_FILE_PATH = "/home/user/alarm.mp3" # Ensure this path is correct for your Debian environment

def trigger_alarm(ser, reason):
    print(f"Triggering Alarm! Reason: {reason}")
    # Send halt command to MCU
    ser.write(b"WAKE\n")
    # Play the MP3 using the Debian OS audio subsystem
    subprocess.run(['mpg123', ALARM_FILE_PATH])

def log_and_monitor():
    # Calculate time boundaries
    now = datetime.now()
    target_time = datetime.strptime(f"{now.strftime('%Y-%m-%d')} {TARGET_WAKE_TIME}", "%Y-%m-%d %H:%M")
    
    window_start = target_time - timedelta(minutes=BUFFER_MINUTES)
    window_end = target_time + timedelta(minutes=BUFFER_MINUTES)
    
    alarm_rung = False

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        
        with open('sleep_log.csv', mode='a', newline='') as file:
            writer = csv.writer(file)
            if file.tell() == 0:
                writer.writerow(['Timestamp', 'Sleep State'])
            
            while not alarm_rung:
                current_time = datetime.now()
                
                # FAILSAFE: If we reach the absolute end of the buffer, wake up regardless of state
                if current_time >= window_end:
                    trigger_alarm(ser, "Failsafe - Reached end of buffer window.")
                    alarm_rung = True
                    break

                if ser.in_waiting > 0:
                    raw_line = ser.readline().decode('utf-8').strip()
                    
                    if "State:" in raw_line:
                        current_state = raw_line.replace("State: ", "")
                        writer.writerow([current_time.strftime('%Y-%m-%d %H:%M:%S'), current_state])
                        file.flush() 
                        
                        # SMART ALARM LOGIC: Check if inside the set window
                        if window_start <= current_time <= window_end:
                            # Check if the user is in an optimal waking state
                            if current_state == "Light Sleep / REM" or current_state == "Relaxed Awake":
                                trigger_alarm(ser, f"Optimal State Detected: {current_state}")
                                alarm_rung = True
                                break

    except KeyboardInterrupt:
        ser.close()

if __name__ == '__main__':
    log_and_monitor()
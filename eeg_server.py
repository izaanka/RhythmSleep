import http.server
import socketserver
import json
import threading
import serial
import csv
from datetime import datetime, timedelta
import subprocess
import os
import urllib.parse
import glob

# Hardware Routing Variables
SERIAL_PORT = '/dev/ttyACM0' 
BAUD_RATE = 115200

# User Analytics Configuration
TARGET_WAKE_TIME = "05:30" 
BUFFER_MINUTES = 30
ALARM_FILE_PATH = "/home/user/alarm.mp3"
PORT = 8000

class SleepDataServer(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        
        # Endpoint 1: File Menu Matrix
        if parsed_url.path == '/api/files':
            csv_files = sorted(glob.glob("sleep_log_*.csv"), reverse=True)
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(csv_files).encode('utf-8'))
            
        # Endpoint 2: Cartesian Coordinate Extraction
        elif parsed_url.path == '/api/data':
            query_params = urllib.parse.parse_qs(parsed_url.query)
            target_file = query_params.get('file', [None])[0]
            
            payload = {"timestamps": [], "states": []}
            if target_file and os.path.exists(target_file):
                with open(target_file, mode='r') as file:
                    reader = csv.reader(file)
                    next(reader, None) 
                    for row in reader:
                        if len(row) == 2:
                            payload["timestamps"].append(row[0])
                            payload["states"].append(row[1])
                            
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(payload).encode('utf-8'))
        else:
            super().do_GET()

def trigger_alarm(ser, reason):
    print(f"Triggering Alarm! Reason: {reason}")
    ser.write(b"WAKE\n")
    subprocess.run(['mpg123', ALARM_FILE_PATH])

def log_and_monitor():
    alarm_rung = False
    linux_time_synced = False
    
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        
        # Phase 1: NTP Wi-Fi Synchronization Check
        ntp_check = subprocess.run(['timedatectl', 'show', '-p', 'NTPSynchronized', '--value'], capture_output=True, text=True)
        if "yes" in ntp_check.stdout.strip():
            print("NTP Active: Pushing Linux system time to Hardware RTC...")
            current_time = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
            ser.write(f"SET_TIME:{current_time}\n".encode('utf-8'))
            linux_time_synced = True 
        else:
            print("NTP Offline: Awaiting PCF8563 RTC baseline injection...")

        while not alarm_rung:
            if ser.in_waiting > 0:
                raw_line = ser.readline().decode('utf-8').strip()
                
                # Verify matrix format matches "YYYY-MM-DD HH:MM:SS,State"
                if "," in raw_line and len(raw_line) > 18:
                    hardware_time_str, current_state = raw_line.split(",", 1)
                    
                    try:
                        hardware_time = datetime.strptime(hardware_time_str, '%Y-%m-%d %H:%M:%S')
                    except ValueError:
                        continue # Skip malformed serial artifacts
                    
                    # Phase 2: Force Linux to mirror the RTC if Wi-Fi is down
                    if not linux_time_synced:
                        print(f"Syncing Linux MPU to hardware clock: {hardware_time_str}")
                        subprocess.run(['date', '-s', hardware_time_str])
                        linux_time_synced = True

                    # Generate dynamic Cartesian logging files
                    dynamic_filename = f"sleep_log_{hardware_time.strftime('%Y-%m-%d')}.csv"
                    file_exists = os.path.isfile(dynamic_filename)
                    
                    with open(dynamic_filename, mode='a', newline='') as file:
                        writer = csv.writer(file)
                        if not file_exists:
                            writer.writerow(['Timestamp', 'Sleep State'])
                        
                        writer.writerow([hardware_time_str, current_state])
                        file.flush() 

                        # Smart Alarm Limit Evaluation
                        target_time = datetime.strptime(f"{hardware_time.strftime('%Y-%m-%d')} {TARGET_WAKE_TIME}", "%Y-%m-%d %H:%M")
                        window_start = target_time - timedelta(minutes=BUFFER_MINUTES)
                        window_end = target_time + timedelta(minutes=BUFFER_MINUTES)

                        if hardware_time >= window_end:
                            trigger_alarm(ser, "Failsafe - Reached end of buffer window.")
                            alarm_rung = True
                            break
                        
                        if window_start <= hardware_time <= window_end:
                            if current_state in ["Core", "REM"]:
                                trigger_alarm(ser, f"Optimal State Detected: {current_state}")
                                alarm_rung = True
                                break

    except Exception as e:
        print(f"Hardware monitoring error: {e}")

if __name__ == '__main__':
    # Initialize hardware monitoring on a background thread
    monitor_thread = threading.Thread(target=log_and_monitor, daemon=True)
    monitor_thread.start()

    # Initialize the web server on the main thread
    with socketserver.TCPServer(("", PORT), SleepDataServer) as httpd:
        print(f"Unified EEG Server active on port {PORT}")
        httpd.serve_forever()

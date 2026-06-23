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

SERIAL_PORT = '/dev/ttyACM0' 
BAUD_RATE = 115200
TARGET_WAKE_TIME = "05:30" 
BUFFER_MINUTES = 30
ALARM_FILE_PATH = "/home/user/alarm.mp3"
PORT = 8000

class SleepDataServer(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        
        # Endpoint to supply the menu with all available CSV files
        if parsed_url.path == '/api/files':
            csv_files = sorted(glob.glob("sleep_log_*.csv"), reverse=True)
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(csv_files).encode('utf-8'))
            
        # Endpoint to supply data for a specific requested CSV
        elif parsed_url.path == '/api/data':
            query_params = urllib.parse.parse_qs(parsed_url.query)
            target_file = query_params.get('file', [None])[0]
            
            payload = {"timestamps": [], "states": []}
            if target_file and os.path.exists(target_file):
                with open(target_file, mode='r') as file:
                    reader = csv.reader(file)
                    next(reader, None) # Skip header
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
    now = datetime.now()
    target_time = datetime.strptime(f"{now.strftime('%Y-%m-%d')} {TARGET_WAKE_TIME}", "%Y-%m-%d %H:%M")
    window_start = target_time - timedelta(minutes=BUFFER_MINUTES)
    window_end = target_time + timedelta(minutes=BUFFER_MINUTES)
    alarm_rung = False

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        
        while not alarm_rung:
            current_time = datetime.now()
            # Dynamically generate filename based on current date
            current_date_str = current_time.strftime('%Y-%m-%d')
            dynamic_filename = f"sleep_log_{current_date_str}.csv"
            
            file_exists = os.path.isfile(dynamic_filename)
            
            with open(dynamic_filename, mode='a', newline='') as file:
                writer = csv.writer(file)
                if not file_exists:
                    writer.writerow(['Timestamp', 'Sleep State'])
                
                if current_time >= window_end:
                    trigger_alarm(ser, "Failsafe - Reached end of buffer window.")
                    break

                if ser.in_waiting > 0:
                    raw_line = ser.readline().decode('utf-8').strip()
                    if "State:" in raw_line:
                        current_state = raw_line.replace("State: ", "")
                        writer.writerow([current_time.strftime('%Y-%m-%d %H:%M:%S'), current_state])
                        file.flush() 
                        
                        if window_start <= current_time <= window_end:
                            if current_state in ["Light Sleep / REM", "Relaxed Awake"]:
                                trigger_alarm(ser, f"Optimal State Detected: {current_state}")
                                alarm_rung = True
                                break
    except Exception as e:
        print(f"Hardware monitoring error: {e}")

if __name__ == '__main__':
    monitor_thread = threading.Thread(target=log_and_monitor, daemon=True)
    monitor_thread.start()

    with socketserver.TCPServer(("", PORT), SleepDataServer) as httpd:
        print(f"Unified EEG Server active on port {PORT}")
        httpd.serve_forever()

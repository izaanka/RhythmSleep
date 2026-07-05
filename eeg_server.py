import http.server
import socketserver
import json
import threading
import serial
import csv
from datetime import datetime, timedelta
from collections import deque
import subprocess
import os
import urllib.parse
import glob
import time

# ═══════════════════════════════════════════════════════════════════════════════
# Hardware Routing Variables
# ═══════════════════════════════════════════════════════════════════════════════
SERIAL_PORT = '/dev/ttyACM0' 
BAUD_RATE = 115200

# ═══════════════════════════════════════════════════════════════════════════════
# Alarm Configuration
# ═══════════════════════════════════════════════════════════════════════════════
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OPENING_FILE = os.path.join(SCRIPT_DIR, "openingtone.mp3")
ALARM_FILE   = os.path.join(SCRIPT_DIR, "alarm.mp3")
PORT = 8000

# ═══════════════════════════════════════════════════════════════════════════════
# Shared State (thread-safe via lock)
# ═══════════════════════════════════════════════════════════════════════════════
state_lock = threading.Lock()
shared_state = {
    "frequency": 0.0,
    "state": "---",
    "timestamp": "",
    "alarm_m": 330,          # Alarm time: minutes from midnight (default 05:30)
    "buffer_m": 30,          # Buffer window in minutes
    "alarm_triggered": False,
    "opening_played": False,
}

# Serial connection reference
serial_connection = None
serial_lock = threading.Lock()

# Serial logs for debug tab
serial_logs = deque(maxlen=500)
serial_logs_lock = threading.Lock()

# ═══════════════════════════════════════════════════════════════════════════════
# Smart Alarm State (only accessed from monitor thread)
# ═══════════════════════════════════════════════════════════════════════════════
HISTORY_SIZE = 30            # 30 readings ≈ 30 seconds at ~1 Hz
freq_history = deque(maxlen=HISTORY_SIZE)
high_freq_tracking = False
high_freq_start_time = 0.0   # time.time() when high freq tracking began
opening_played = False
opening_time = 0.0
alarm_process = None          # subprocess for alarm playback


# ═══════════════════════════════════════════════════════════════════════════════
# Web Server Handler
# ═══════════════════════════════════════════════════════════════════════════════
class SleepDataServer(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        
        if parsed_url.path == '/api/files':
            csv_files = sorted(glob.glob("sleep_log_*.csv"), reverse=True)
            self._json_response(csv_files)
            
        elif parsed_url.path == '/api/data':
            query_params = urllib.parse.parse_qs(parsed_url.query)
            target_file = query_params.get('file', [None])[0]
            
            payload = {"timestamps": [], "frequencies": [], "states": []}
            if target_file and os.path.exists(target_file):
                with open(target_file, mode='r') as file:
                    reader = csv.reader(file)
                    next(reader, None)
                    for row in reader:
                        if len(row) >= 3:
                            payload["timestamps"].append(row[0])
                            payload["frequencies"].append(float(row[1]))
                            payload["states"].append(row[2])
            self._json_response(payload)

        elif parsed_url.path == '/api/live':
            with state_lock:
                payload = dict(shared_state)
            self._json_response(payload)

        elif parsed_url.path == '/api/alarm':
            with state_lock:
                payload = {
                    "alarm_m": shared_state["alarm_m"],
                    "buffer_m": shared_state["buffer_m"],
                    "alarm_hour": shared_state["alarm_m"] // 60,
                    "alarm_min": shared_state["alarm_m"] % 60
                }
            self._json_response(payload)

        elif parsed_url.path == '/api/serial':
            with serial_logs_lock:
                payload = list(serial_logs)
            self._json_response(payload)
        else:
            super().do_GET()

    def do_POST(self):
        parsed_url = urllib.parse.urlparse(self.path)
        
        if parsed_url.path == '/api/alarm':
            content_length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_length).decode('utf-8')
            
            try:
                data = json.loads(body)
                new_alarm_m = max(0, min(1439, int(data.get('alarm_m', 330))))
                new_buffer_m = max(5, min(120, int(data.get('buffer_m', 30))))

                with state_lock:
                    shared_state["alarm_m"] = new_alarm_m
                    shared_state["buffer_m"] = new_buffer_m

                # Forward to Minima via UNO Q relay so OLED updates
                with serial_lock:
                    if serial_connection and serial_connection.is_open:
                        cmd = f"SET_ALARM:{new_alarm_m},{new_buffer_m}\n"
                        serial_connection.write(cmd.encode('utf-8'))

                h, m = new_alarm_m // 60, new_alarm_m % 60
                print(f"Alarm set → {h:02d}:{m:02d} ±{new_buffer_m}m")

                self._json_response({
                    "status": "ok",
                    "alarm_m": new_alarm_m,
                    "buffer_m": new_buffer_m
                })

            except (json.JSONDecodeError, ValueError) as e:
                self._json_response({"error": str(e)}, 400)
        else:
            self.send_response(404)
            self.end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def _json_response(self, data, status=200):
        self.send_response(status)
        self.send_header('Content-type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode('utf-8'))

    def log_message(self, format, *args):
        pass  # Suppress per-request logs


# ═══════════════════════════════════════════════════════════════════════════════
# MP3 Playback (via mpg123 on Linux)
# ═══════════════════════════════════════════════════════════════════════════════
def play_mp3(filepath, loop=False):
    """Play an MP3 file using mpg123. Returns the subprocess."""
    if not os.path.exists(filepath):
        print(f"WARNING: MP3 file not found: {filepath}")
        return None
    
    try:
        args = ['mpg123']
        if loop:
            args.append('--loop')
            args.append('-1')  # Infinite loop
        args.append(filepath)
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return proc
    except FileNotFoundError:
        print("ERROR: mpg123 not installed. Run: apt-get install mpg123")
        return None


def stop_playback(proc):
    """Stop an active mpg123 process."""
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


# ═══════════════════════════════════════════════════════════════════════════════
# Smart Alarm Logic
# ═══════════════════════════════════════════════════════════════════════════════
def check_smart_alarm(frequency):
    """
    Called every time a new frequency reading arrives.
    Implements the smart alarm:
      1. Check if current time is inside the alarm window
      2. Track the highest frequency over a 30-second rolling window
      3. If dominant freq ≈ highest freq for ≥5s → play opening tone
      4. If frequency stays stable (±5 Hz) → play alarm
      5. Failsafe: force alarm at end of buffer window
    """
    global high_freq_tracking, high_freq_start_time
    global opening_played, opening_time, alarm_process

    with state_lock:
        if shared_state["alarm_triggered"]:
            return
        alarm_m = shared_state["alarm_m"]
        buffer_m = shared_state["buffer_m"]

    now = datetime.now()
    current_minutes = now.hour * 60 + now.minute if hasattr(now, 'hour') else 0
    current_minutes = now.hour * 60 + now.minute

    window_start = alarm_m - buffer_m
    window_end = alarm_m + buffer_m

    # Check if inside alarm window (handle midnight wrap)
    in_window = False
    if window_start < 0:
        in_window = (current_minutes >= (window_start + 1440)) or (current_minutes <= window_end)
    elif window_end >= 1440:
        in_window = (current_minutes >= window_start) or (current_minutes <= (window_end - 1440))
    else:
        in_window = (window_start <= current_minutes <= window_end)

    if not in_window:
        return

    # Failsafe: past end of window → force alarm
    past_end = False
    if window_end >= 1440:
        past_end = (current_minutes >= (window_end - 1440)) and (current_minutes < window_start)
    else:
        past_end = (current_minutes >= window_end)

    if past_end:
        print("ALARM: Failsafe — end of buffer window reached.")
        trigger_alarm()
        return

    # Need at least a full 30-second history
    if len(freq_history) < HISTORY_SIZE:
        return

    # Find the highest frequency in the rolling window
    highest_freq = max(freq_history)

    # Is current frequency near the highest? (within ±5 Hz)
    at_highest = abs(frequency - highest_freq) <= 5.0

    if at_highest:
        if not high_freq_tracking:
            high_freq_tracking = True
            high_freq_start_time = time.time()
            print(f"ALARM: High freq {frequency:.1f} Hz detected, tracking...")

        elapsed = time.time() - high_freq_start_time

        # After 5 seconds → play opening tone
        if elapsed >= 5.0 and not opening_played:
            print(f"ALARM: Opening tone (freq stable at ~{frequency:.1f} Hz for {elapsed:.0f}s)")
            play_mp3(OPENING_FILE)
            opening_played = True
            opening_time = time.time()

        # After opening tone, check stability → play alarm
        if opening_played and not alarm_process:
            # Verify all readings are within ±5 Hz of highest
            stable = all(abs(f - highest_freq) <= 5.0 for f in freq_history)

            if stable and (time.time() - opening_time >= 3.0):
                print(f"ALARM: Stable freq confirmed. Triggering alarm.")
                trigger_alarm()
    else:
        # Frequency dropped — reset tracking
        if high_freq_tracking:
            print(f"ALARM: Freq dropped to {frequency:.1f} Hz, resetting.")
        high_freq_tracking = False
        opening_played = False


def trigger_alarm():
    """Fire the alarm: play MP3, notify Minima, update state."""
    global alarm_process

    with state_lock:
        shared_state["alarm_triggered"] = True

    # Play alarm.mp3 on loop
    alarm_process = play_mp3(ALARM_FILE, loop=True)

    # Send WAKE to Minima via UNO Q relay (updates OLED display)
    with serial_lock:
        if serial_connection and serial_connection.is_open:
            serial_connection.write(b"WAKE\n")

    print(">>> ALARM TRIGGERED <<<")


# ═══════════════════════════════════════════════════════════════════════════════
# Serial Monitor & Logger
# ═══════════════════════════════════════════════════════════════════════════════
def log_and_monitor():
    global serial_connection
    linux_time_synced = False
    
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        with serial_lock:
            serial_connection = ser
        
        print(f"Serial connected: {SERIAL_PORT} @ {BAUD_RATE}")

        # NTP sync check — push system time to RTC if available
        try:
            ntp_check = subprocess.run(
                ['timedatectl', 'show', '-p', 'NTPSynchronized', '--value'],
                capture_output=True, text=True, timeout=5
            )
            if "yes" in ntp_check.stdout.strip():
                print("NTP Active: Pushing system time to RTC...")
                current_time = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                ser.write(f"SET_TIME:{current_time}\n".encode('utf-8'))
                linux_time_synced = True
            else:
                print("NTP Offline: Using hardware RTC time.")
        except Exception:
            print("NTP check failed. Using hardware RTC time.")

        while True:
            if ser.in_waiting > 0:
                raw_line = ser.readline().decode('utf-8', errors='replace').strip()
                
                if not raw_line:
                    continue

                with serial_logs_lock:
                    serial_logs.append(raw_line)

                # Skip boot/debug messages from UNO Q
                if raw_line.startswith("UNOQ"):
                    continue

                # Handle alarm settings from Minima buttons (via UNO Q relay)
                if raw_line.startswith("SET_ALARM:"):
                    try:
                        parts = raw_line[10:].split(",")
                        new_alarm_m = max(0, min(1439, int(parts[0])))
                        new_buffer_m = max(5, min(120, int(parts[1])))
                        with state_lock:
                            shared_state["alarm_m"] = new_alarm_m
                            shared_state["buffer_m"] = new_buffer_m
                        h, m = new_alarm_m // 60, new_alarm_m % 60
                        print(f"Alarm updated from Minima → {h:02d}:{m:02d} ±{new_buffer_m}m")
                    except (ValueError, IndexError):
                        pass
                    continue

                # Handle error messages
                if raw_line.startswith("ERROR:"):
                    print(f"Minima Error: {raw_line}")
                    continue

                # Parse classified data from UNO Q: "FREQ,Band"
                if "," in raw_line:
                    parts = raw_line.split(",", 1)
                    if len(parts) == 2:
                        try:
                            frequency = float(parts[0])
                            band = parts[1].strip()

                            valid_bands = ["Focused", "Active", "Relaxed", "Light Sleep", "Deep Sleep", "Unknown"]
                            if band not in valid_bands:
                                continue

                            now = datetime.now()
                            timestamp = now.strftime('%Y-%m-%d %H:%M:%S')

                            # Update shared state for live API
                            with state_lock:
                                shared_state["frequency"] = frequency
                                shared_state["state"] = band
                                shared_state["timestamp"] = timestamp

                            if not linux_time_synced:
                                linux_time_synced = True

                            # Log to daily CSV
                            dynamic_filename = f"sleep_log_{now.strftime('%Y-%m-%d')}.csv"
                            file_exists = os.path.isfile(dynamic_filename)
                            
                            with open(dynamic_filename, mode='a', newline='') as file:
                                writer = csv.writer(file)
                                if not file_exists:
                                    writer.writerow(['Timestamp', 'Frequency', 'State'])
                                writer.writerow([timestamp, f"{frequency:.2f}", band])
                                file.flush()

                            # Add to frequency history for smart alarm
                            freq_history.append(frequency)

                            # Run smart alarm check
                            check_smart_alarm(frequency)

                            print(f"{timestamp} | {frequency:6.2f} Hz | {band}")

                        except ValueError:
                            continue

    except serial.SerialException as e:
        print(f"Serial error: {e}")
    except Exception as e:
        print(f"Monitor error: {e}")


# ═══════════════════════════════════════════════════════════════════════════════
# Entry Point
# ═══════════════════════════════════════════════════════════════════════════════
if __name__ == '__main__':
    monitor_thread = threading.Thread(target=log_and_monitor, daemon=True)
    monitor_thread.start()

    with socketserver.TCPServer(("", PORT), SleepDataServer) as httpd:
        print(f"RhythmSleep Server active on http://0.0.0.0:{PORT}")
        httpd.serve_forever()

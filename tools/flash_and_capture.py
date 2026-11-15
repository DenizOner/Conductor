#!/usr/bin/env python3
"""
flash_and_capture.py — Upload PlatformIO test binary + capture Unity serial output.

FIX: Opens the serial port BEFORE triggering esptool upload (in background),
so we catch the very first Unity bytes that appear right after hard reset.
If the first boot finishes before we can read it, we trigger a second reset
via esptool --after hard_reset to repeat it.
"""

import sys
import subprocess
import threading
import queue
import time
import re
import os

PLATFORMIO = os.path.join(os.environ["USERPROFILE"], ".platformio", "penv", "Scripts", "pio.exe")
ESPTOOL    = os.path.join(os.environ["USERPROFILE"], ".platformio", "packages",
                          "tool-esptoolpy", "esptool.py")
PORT       = "COM6"
BAUD       = 115200
TIMEOUT_S  = 60

# ---------- Serial reader thread ----------
serial_q = queue.Queue()
stop_event = threading.Event()

def serial_reader():
    import serial
    import serial.tools.list_ports
    # Wait until the port appears (it disappears briefly after esptool reset)
    for _ in range(20):
        if stop_event.is_set():
            return
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if PORT in ports:
            break
        time.sleep(0.3)
    try:
        with serial.Serial(PORT, BAUD, timeout=0.5) as ser:
            while not stop_event.is_set():
                raw = ser.readline()
                if raw:
                    try:
                        line = raw.decode("utf-8", errors="replace").rstrip()
                    except Exception:
                        line = repr(raw)
                    serial_q.put(line)
    except Exception as e:
        serial_q.put(f"[SERIAL ERROR] {e}")

def collect_unity_output(timeout_s):
    """Drain queue until Unity footer found or timeout."""
    lines = []
    deadline = time.time() + timeout_s
    found_footer = False
    while time.time() < deadline:
        try:
            line = serial_q.get(timeout=1)
        except queue.Empty:
            continue
        print(line)
        lines.append(line)
        if re.search(r"\d+ Tests \d+ Failures \d+ Ignored", line):
            found_footer = True
            time.sleep(0.3)
            # Drain remaining lines
            while True:
                try:
                    el = serial_q.get_nowait()
                    print(el)
                    lines.append(el)
                except queue.Empty:
                    break
            break
    return lines, found_footer

def upload(test_filter):
    print(f"[*] Building + uploading: {test_filter}")
    cmd = [PLATFORMIO, "test", "-e", "esp32-c6-devkitc-1",
           "--filter", test_filter, "--without-testing"]
    result = subprocess.run(cmd, cwd=r"C:\Conductor",
                            capture_output=True, text=True, timeout=300)
    output = result.stdout + result.stderr
    if "error" in output.lower() and "SKIPPED" not in output:
        print("[ERROR] Upload failed:")
        print(output[-3000:])
        return False
    return True

def hard_reset_via_esptool():
    """Use esptool to reset the board without reflashing."""
    print("[*] Triggering hard reset via esptool...")
    py = sys.executable
    cmd = [py, ESPTOOL, "--chip", "esp32c6", "--port", PORT, "--baud", "115200",
           "--before", "default_reset", "--after", "hard_reset", "read_mac"]
    subprocess.run(cmd, capture_output=True, timeout=15)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python flash_and_capture.py <test_filter>")
        sys.exit(1)
    filter_arg = sys.argv[1]

    # Start serial reader thread BEFORE upload so we don't miss early output
    t = threading.Thread(target=serial_reader, daemon=True)
    t.start()

    # Upload (esptool will hard-reset board after flash — tests start immediately)
    ok = upload(filter_arg)
    if not ok:
        stop_event.set()
        sys.exit(1)

    print(f"[*] Listening on {PORT} for Unity output ({TIMEOUT_S}s timeout)...")
    lines, found = collect_unity_output(TIMEOUT_S // 2)  # First attempt: 30s

    if not found:
        # Tests may have already run before serial was ready — trigger a second boot
        print("[!] First boot output not captured. Triggering hard reset for second boot...")
        hard_reset_via_esptool()
        lines2, found = collect_unity_output(TIMEOUT_S // 2)
        lines += lines2

    stop_event.set()

    if not found:
        print("[ERROR] Timed out — no Unity output received on either boot.")
        sys.exit(1)

    # Parse result
    full = "\n".join(lines)
    m = re.search(r"(\d+) Tests (\d+) Failures (\d+) Ignored", full)
    if m:
        tests, failures, ignored = int(m.group(1)), int(m.group(2)), int(m.group(3))
        print(f"\n[RESULT] {tests} Tests  {failures} Failures  {ignored} Ignored")
        sys.exit(0 if failures == 0 else 1)
    else:
        print("[ERROR] Could not parse Unity result line.")
        sys.exit(1)

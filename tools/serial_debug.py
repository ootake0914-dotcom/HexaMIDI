#!/usr/bin/env python3
"""
Spresense NSH Serial Monitor & Real-Time Debugger
"""

import sys
import time
import argparse
import serial

def main():
    parser = argparse.ArgumentParser(description="Spresense Serial Monitor & Debugger")
    parser.add_argument("--port", "-p", default="COM6", help="Serial port")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate")
    parser.add_argument("--duration", "-d", type=float, default=35.0, help="Monitoring duration in seconds")
    args = parser.parse_args()

    port = args.port
    baud = args.baud
    monitor_duration = args.duration

    print(f"[DEBUGGER] Connecting to Spresense on {port} @ {baud} bps (duration={monitor_duration}s)...")
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
    except Exception as e:
        print(f"[DEBUGGER] Failed to open {port}: {e}")
        sys.exit(1)

    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Wake up NSH
    print("[DEBUGGER] Sending Enter to wake up NSH...")
    ser.write(b"\r\n")
    time.sleep(0.5)
    
    # Read initial NSH banner/prompt
    if ser.in_waiting:
        resp = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
        print(f"[NSH RESPONSE]:\n{resp}")

    # Send synth command
    print("[DEBUGGER] Launching 'synth' on Spresense...")
    ser.write(b"synth\r\n")
    
    start_time = time.time()

    while (time.time() - start_time) < monitor_duration:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
            sys.stdout.write(chunk)
            sys.stdout.flush()
        time.sleep(0.02)

    # Send SIGINT (Ctrl+C) to terminate cleanly
    print("\n[DEBUGGER] Sending Ctrl+C to stop synth...")
    ser.write(b"\x03\r\n")
    time.sleep(0.5)

    if ser.in_waiting:
        chunk = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
        sys.stdout.write(chunk)
        sys.stdout.flush()

    ser.close()
    print("[DEBUGGER] Debug session completed.")

if __name__ == "__main__":
    main()

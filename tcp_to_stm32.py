#!/usr/bin/env python3
"""
tcp_to_stm32.py
Run NS-3 TCP sim and send results to STM32 over serial.

Usage:
  python3 tcp_to_stm32.py --dryrun
  python3 tcp_to_stm32.py --port /dev/ttyS5
"""

import subprocess, sys, csv, time, argparse, os

try:
    import serial
    SERIAL_OK = True
except ImportError:
    SERIAL_OK = False

NS3_SCRIPT = "tcp-congestion-sim"
SIM_ARGS   = ["--bw=10", "--delay=20", "--loss=3", "--simTime=20"]

MIN_DELAY = 80
MAX_DELAY = 1000

def tput_to_blink(tput, min_t, max_t):
    if max_t == min_t:
        return 400
    ratio = (tput - min_t) / (max_t - min_t)
    delay = int(MAX_DELAY - ratio * (MAX_DELAY - MIN_DELAY))
    return max(MIN_DELAY, min(MAX_DELAY, delay))

def run_ns3(ns3_path):
    print("[*] Running NS-3 simulation (~20 seconds)...")
    cmd = [f"{ns3_path}/ns3", "run", f"scratch/{NS3_SCRIPT}", "--"] + SIM_ARGS
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("[!] NS-3 failed:\n", r.stderr[-3000:])
        sys.exit(1)

    for line in r.stderr.splitlines():
        if any(x in line for x in ["->", "Throughput", "Latency", "Blink"]):
            print(" ", line.strip())

    results = []
    lines = [l for l in r.stdout.strip().splitlines() if l.strip()]
    if not lines:
        print("[!] No CSV data from NS-3 stdout")
        sys.exit(1)

    # simple header check
    header = lines[0]
    if not all(col in header for col in ["ALGO","THROUGHPUT_MBPS","LATENCY_MS","BLINK_DELAY_MS"]):
        print("[!] Unexpected CSV header:", header)
        sys.exit(1)

    reader = csv.DictReader(lines)
    for row in reader:
        results.append({
            "algo":    row["ALGO"],
            "tput":    float(row["THROUGHPUT_MBPS"]),
            "latency": float(row["LATENCY_MS"]),
            "blink":   int(row["BLINK_DELAY_MS"]),
        })
    return results

def assign_blink_delays(results):
    tputs = [r["tput"] for r in results]
    min_t = min(tputs)
    max_t = max(tputs)

    for r in results:
        r["blink"] = tput_to_blink(r["tput"], min_t, max_t)

    order = {"Tahoe": 0, "Reno": 1, "Swift": 2}
    results_sorted = sorted(results, key=lambda x: order.get(x["algo"], 99))

    for i in range(len(results_sorted) - 1, 0, -1):
        if results_sorted[i-1]["blink"] < results_sorted[i]["blink"] + 150:
            results_sorted[i-1]["blink"] = results_sorted[i]["blink"] + 150

    for r in results_sorted:
        r["blink"] = max(MIN_DELAY, min(MAX_DELAY, r["blink"]))

    return results

def print_table(results):
    print()
    print("+---------+------------------+--------------+------------------+")
    print("| Algo    | Throughput (Mbps)| Latency (ms) | Blink delay (ms) |")
    print("+---------+------------------+--------------+------------------+")
    order = {"Tahoe": 0, "Reno": 1, "Swift": 2}
    for r in sorted(results, key=lambda x: order.get(x["algo"], 99)):
        print(f"| {r['algo']:<7} | {r['tput']:>16.3f} | {r['latency']:>12.1f} | {r['blink']:>16} |")
    print("+---------+------------------+--------------+------------------+")
    print()
    print("  LD1 blink: Tahoe=slow  Reno=medium  Swift=rapid flicker")
    print("  LD3: solid ON each phase, 200ms reference in compare mode")
    print()

def send_to_stm32(port, baud, results):
    if not SERIAL_OK:
        print("[!] Run: pip install pyserial --break-system-packages")
        sys.exit(1)
    print(f"[*] Opening {port} at {baud} baud...")
    try:
        # increased timeout so it can wait for ACKs
        ser = serial.Serial(port, baud, timeout=5)
    except serial.SerialException as e:
        print(f"[!] Cannot open port: {e}")
        sys.exit(1)

    time.sleep(2)

    # flush any startup garbage
    while ser.in_waiting:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            print(f"  STM32: {line}")

    order    = {"Tahoe": 0, "Reno": 1, "Swift": 2}
    tag_map  = {"Tahoe": "TAHOE", "Reno": "RENO", "Swift": "SWIFT"}
    sorted_r = sorted(results, key=lambda x: order.get(x["algo"], 99))

    print("\n[*] Sending to STM32...")
    for r in sorted_r:
        tag = tag_map.get(r["algo"], r["algo"].upper())
        cmd = f"{tag}:{r['blink']}\n"
        print(f"  Sending: {cmd.strip()}")

        ser.write(cmd.encode())
        time.sleep(0.2)

        # read and print raw ACK byte stream
        raw = ser.read_until(b"\n")
        print("  RAW ACK:", repr(raw))   # shows exactly what STM32 sent
        ack = raw.decode(errors="replace").strip()
        if ack:
            print(f"  STM32: {ack}")

    print("\n[*] Done! Watch the board:")
    print("    LD1 cycles: VERY SLOW (Tahoe) -> MEDIUM (Reno) -> FAST FLICKER (Swift)")
    print("    LD3 stays solid during each phase")
    ser.close()

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--ns3path", default="~/ns-allinone-3.43/ns-3.43")
    ap.add_argument("--port",    default="/dev/ttyS5")
    ap.add_argument("--baud",    default=115200, type=int)
    ap.add_argument("--dryrun",  action="store_true",
                    help="Skip serial, just show table")
    args = ap.parse_args()

    ns3_path = os.path.expanduser(args.ns3path)
    results  = run_ns3(ns3_path)
    results  = assign_blink_delays(results)
    print_table(results)

    if args.dryrun:
        print("[*] Dry run complete -- serial skipped.")
    else:
        send_to_stm32(args.port, args.baud, results)

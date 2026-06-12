#!/usr/bin/env python3
"""
aer_probe — feasibility probe for Alternate Eye Rendering through X4's
OpenTrack UDP listener.

Sends synthetic pose packets (OpenTrack wire format: six little-endian
doubles x_cm, y_cm, z_cm, yaw_deg, pitch_deg, roll_deg) to X4 on UDP :4242
and lets you measure, frame by frame, how the in-game camera responds.

The go/no-go questions for AER via this channel:

  1. RESPONSIVENESS  — does the camera follow a per-frame square wave at all,
     or does X4's head-tracking filter/deadzone flatten it?
     -> --mode aer at your real fps; set in-game filter strength & deadzones
        to minimum first. If the cockpit visibly "vibrates" laterally, the
        signal survives.

  2. AMPLITUDE       — is the resulting camera offset the full +-IPD/2, or
     attenuated? -> --mode step: alternate slowly (1 Hz) and compare the two
     camera positions against a fast alternation.

  3. PHASE STABILITY — does pose->frame latency stay constant?
     -> --mode square with --axis yaw and a high-speed phone camera /
        OBS 120fps capture of the screen; count frames between packet flip
        (logged with timestamps) and visible motion. Repeat; jitter > 1 frame
        means parity cannot be trusted (eye-swap risk).

Examples:
    ./aer_probe.py --mode aer --fps 72 --ipd 6.4      # alternate eyes at 72 Hz
    ./aer_probe.py --mode step --period 1.0 --ipd 6.4 # slow A/B for amplitude
    ./aer_probe.py --mode sine --axis yaw --amp 10 --hz 0.5   # sanity check
"""

import argparse
import math
import signal
import socket
import struct
import sys
import time

PACK = struct.Struct("<6d")  # x_cm, y_cm, z_cm, yaw, pitch, roll


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4242)
    ap.add_argument("--mode", choices=["aer", "square", "step", "sine", "zero"],
                    default="aer")
    ap.add_argument("--axis", choices=["x", "y", "z", "yaw", "pitch", "roll"],
                    default="x", help="axis driven in square/step/sine modes")
    ap.add_argument("--amp", type=float, default=3.2,
                    help="amplitude: cm for x/y/z, degrees for yaw/pitch/roll")
    ap.add_argument("--ipd", type=float, default=6.4,
                    help="aer/step modes: full IPD in cm (offset = +-ipd/2)")
    ap.add_argument("--fps", type=float, default=72.0,
                    help="aer/square modes: alternation rate, packets/s "
                         "(match the game frame rate)")
    ap.add_argument("--hz", type=float, default=0.5,
                    help="sine mode: wave frequency")
    ap.add_argument("--period", type=float, default=1.0,
                    help="step mode: seconds per side")
    ap.add_argument("--log", action="store_true",
                    help="print a timestamped line on every flip (for phase "
                         "measurement against captured video)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (args.host, args.port)
    idx = {"x": 0, "y": 1, "z": 2, "yaw": 3, "pitch": 4, "roll": 5}[args.axis]

    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    if args.mode in ("aer", "square"):
        dt = 1.0 / args.fps
    elif args.mode == "step":
        dt = args.period
    else:
        dt = 1.0 / 120.0  # smooth sine / steady zero

    print(f"mode={args.mode} -> {dst[0]}:{dst[1]}  (Ctrl-C to stop)",
          file=sys.stderr)

    side = 1
    n = 0
    t0 = time.perf_counter()
    next_t = t0

    while running:
        pose = [0.0] * 6
        now = time.perf_counter()

        if args.mode == "aer":
            pose[0] = side * args.ipd / 2.0          # lateral eye offset
            side = -side
        elif args.mode == "square":
            pose[idx] = side * args.amp
            side = -side
        elif args.mode == "step":
            pose[idx if args.axis != "x" else 0] = side * (
                args.ipd / 2.0 if args.axis == "x" else args.amp)
            side = -side
        elif args.mode == "sine":
            pose[idx] = args.amp * math.sin(2 * math.pi * args.hz * (now - t0))
        # zero: all zeros — a "recenter" stream

        sock.sendto(PACK.pack(*pose), dst)
        n += 1

        if args.log and args.mode in ("aer", "square", "step"):
            print(f"{now - t0:10.6f}  flip -> {pose[idx]:+.2f}", flush=True)
        elif n % max(int(1.0 / dt), 1) == 0:
            print(f"\r{n} packets", end="", file=sys.stderr, flush=True)

        next_t += dt
        # hybrid wait: coarse sleep, then spin for the last ms — at 144 Hz the
        # period is ~6.9 ms, so plain sleep() jitter would be real phase noise
        delay = next_t - time.perf_counter()
        if delay > 0:
            if delay > 0.0015:
                time.sleep(delay - 0.001)
            while time.perf_counter() < next_t:
                pass
        else:
            next_t = time.perf_counter()  # fell behind; resync

    print(f"\nsent {n} packets", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

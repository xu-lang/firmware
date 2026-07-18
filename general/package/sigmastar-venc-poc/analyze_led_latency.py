#!/usr/bin/env python3
"""Single-pass LED detection: build mask from first cycle, track brightness, compute latency."""

import subprocess
import sys
import os
import numpy as np

CLEAN_MP4 = "/home/xulang/github/firmware/camera-test-clean.mp4"
TSV_PATH = "/home/xulang/github/firmware/camera-test.h265.tsv"
W, H = 1280, 720

LED_R_DIFF_THRESH = 100
LED_MASK_MIN = 50


def load_tsv(path):
    sw = {}
    on_ev = []
    off_ev = []
    with open(path) as f:
        for line in f:
            p = line.strip().split("\t")
            if p[0] == "frame":
                sw[int(p[1])] = int(p[7])
            elif p[0] == "led-on":
                on_ev.append(int(p[1]))
            elif p[0] == "led-off":
                off_ev.append(int(p[1]))
    return sw, on_ev, off_ev


def main():
    if not os.path.exists(CLEAN_MP4):
        print("Generate: ffmpeg -r 120 -i camera-test.h265 -c:v libx264 "
              "-bf 0 -g 120 -pix_fmt yuv420p -r 120 camera-test-clean.mp4",
              file=sys.stderr)
        sys.exit(1)

    sw_led, sw_on, sw_off = load_tsv(TSV_PATH)
    max_fn = max(sw_led.keys())

    cmd = ["ffmpeg", "-v", "error", "-r", "120", "-i", CLEAN_MP4,
           "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    fs = W * H * 3

    # --- single pass: read all frames, collect refs for mask ---
    all_rgb = []
    off_sum = np.zeros(H * W * 3, dtype=np.float64)
    off_cnt = 0
    on_sum = np.zeros(H * W * 3, dtype=np.float64)
    on_cnt = 0

    print("Reading frames...", file=sys.stderr)
    for fn in range(max_fn + 1):
        data = proc.stdout.read(fs)
        if len(data) < fs:
            break
        rgb = np.frombuffer(data, dtype=np.uint8)
        all_rgb.append(rgb)

        # First cycle: off=0..29, on=40..60 for mask
        if fn < 30:
            off_sum += rgb.astype(np.float64)
            off_cnt += 1
        elif 40 <= fn < 60:
            on_sum += rgb.astype(np.float64)
            on_cnt += 1

    proc.terminate()
    proc.wait()

    # --- build LED mask from first cycle ---
    off_avg = off_sum / off_cnt
    on_avg = on_sum / on_cnt
    off_r = off_avg.reshape(H, W, 3)[:, :, 0]
    on_r = on_avg.reshape(H, W, 3)[:, :, 0]
    r_diff = np.abs(on_r - off_r)
    mask = r_diff > LED_R_DIFF_THRESH

    if mask.sum() < LED_MASK_MIN:
        print(f"ERROR: LED mask too small: {mask.sum()} pixels", file=sys.stderr)
        sys.exit(1)
    ys, xs = np.nonzero(mask)
    cy, cx = int(ys.mean()), int(xs.mean())
    print(f"LED mask: {mask.sum()} pixels, center=({cx},{cy})", file=sys.stderr)

    # --- track brightness across all frames ---
    mask_flat = mask.flatten()
    brightness = []
    for rgb in all_rgb:
        r = rgb.reshape(H, W, 3)[:, :, 0].astype(np.float32)
        b = r[mask].mean()
        brightness.append(b)

    b_arr = np.array(brightness)
    off_mean = b_arr[0:30].mean()
    on_mean = b_arr[40:60].mean()
    threshold = (off_mean + on_mean) / 2
    print(f"Brightness: off={off_mean:.1f}  on={on_mean:.1f}  thresh={threshold:.1f}", file=sys.stderr)

    # --- find visual edges with debounce ---
    vis_on = b_arr > threshold
    raw_edges = []
    for i in range(1, len(vis_on)):
        if vis_on[i] != vis_on[i - 1]:
            raw_edges.append((i, bool(vis_on[i])))

    # debounce: keep edges only if stable for >= 3 frames
    vis_edges = []
    if raw_edges:
        vis_edges.append(raw_edges[0])
        for i in range(1, len(raw_edges) - 1):
            fn, state = raw_edges[i]
            prev_fn, _ = raw_edges[i - 1]
            next_fn, _ = raw_edges[i + 1]
            gap_before = fn - prev_fn
            gap_after = next_fn - fn
            if gap_before >= 3 and gap_after >= 3:
                vis_edges.append((fn, state))
        if len(raw_edges) >= 2:
            last_fn, last_state = raw_edges[-1]
            prev_fn, _ = raw_edges[-2]
            if last_fn - prev_fn >= 3:
                vis_edges.append((last_fn, last_state))

    # --- report ---
    print()
    print("=== Software LED events ===")
    for fn in sw_on:
        print(f"  led-on   at frame {fn}")
    for fn in sw_off:
        print(f"  led-off  at frame {fn}")

    print("\n=== Visual LED edges (debounced) ===")
    for fn, is_on in vis_edges:
        print(f"  frame {fn:5d}  LED -> {'ON' if is_on else 'OFF'}")

    print("\n=== Latency: sw edge -> first visual edge after ===")
    sw_pairs = sorted([(fn, 1) for fn in sw_on] + [(fn, 0) for fn in sw_off])
    results_on = []
    results_off = []

    for sw_fn, sw_state in sw_pairs:
        best = None
        best_dist = 9999
        for v_fn, v_state in vis_edges:
            if v_state == sw_state:
                dist = v_fn - sw_fn
                if 0 <= dist < best_dist:
                    best_dist = dist
                    best = v_fn
        if best is not None:
            label = "led-on" if sw_state == 1 else "led-off"
            lat = best - sw_fn
            print(f"  {label:7s}  sw={sw_fn:5d}  vis={best:5d}  "
                  f"lat={lat:4d} frames  {lat/120*1000:6.1f} ms  b={b_arr[best]:.0f}")
            if sw_state == 1:
                results_on.append(lat)
            else:
                results_off.append(lat)
        else:
            label = "led-on" if sw_state == 1 else "led-off"
            print(f"  {label:7s}  sw={sw_fn:5d}  NO MATCH")

    print()
    if results_on:
        a = np.array(results_on)
        print(f"led-on  latency: min={a.min()} max={a.max()} mean={a.mean():.1f} "
              f"median={np.median(a):.0f} frames  ({a.mean()/120*1000:.1f} ms)  n={len(a)}")
    if results_off:
        a = np.array(results_off)
        print(f"led-off latency: min={a.min()} max={a.max()} mean={a.mean():.1f} "
              f"median={np.median(a):.0f} frames  ({a.mean()/120*1000:.1f} ms)  n={len(a)}")


if __name__ == "__main__":
    main()

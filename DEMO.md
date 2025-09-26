## Minimal stereo-diagnosis demo

A tiny OpenGL program to help isolate timing/transform issues independent of glxgears. It renders a single rotating triangle and can mimic behaviors that commonly cause stereo drift.

### Build

```
make demo
```

This produces a `demo` executable (and the shared libs `libstereo32.so` / `libstereo64.so`).

### Run

```
./demo
./demo --use-lists         # use display lists
./demo --extra-identity    # do an extra glLoadIdentity mid-frame
./demo --extra-clear       # do an extra glClear mid-frame
./demo --static            # no animation (triangle stays in place)
```

Interactive toggles while running:

- L: toggle display lists
- I: toggle extra glLoadIdentity mid-frame
- C: toggle extra glClear mid-frame
- A: toggle animation
- Esc: quit

### With and without the stereo library

To run through the stereo interposer:

```
LD_PRELOAD=./libstereo64.so ./demo
```

Run both with and without `LD_PRELOAD` to compare behavior.

### What to test/observe

- Drag/move/resize the demo window and watch for any “warping”/jumping between two positions.
- Try the demo modes one at a time:
  - `--use-lists` (display lists)
  - `--extra-identity` (mid-frame matrix reset)
  - `--extra-clear` (mid-frame clear+redraw)
- If the issue does NOT reproduce here but DOES in glxgears, it’s likely glxgears-specific (display list usage, mid-frame matrix changes, redraw cadence).
- If the issue DOES reproduce here, the problem is likely in the stereo injection/timing path.

### OSD modes to try (when running with LD_PRELOAD)

- Per-primitive Inject (glBegin): On/Off
- Inject in glClear: On/Off
- Swap on Clear (advanced): On/Off
- Lock to VBlank Parity (SGI): On/Off (+ Invert parity)
- Bypass Compositor (main/OSD): On

Toggle these in the OSD and repeat the move/resize test.

### Diagnosis guide

- Demo stable, glxgears warps:
  - Use Per-primitive Inject (glBegin) (also interposed `glCallList`) for glxgears.
  - Keep compositor bypass ON.
- Demo warps too:
  - Try Swap on Clear and Lock to VBlank Parity (with parity invert) together.
  - If still present, try Per-primitive Inject and disable Inject in glClear.

### Notes

- If `fmod` link errors occur when building `demo`, ensure `-lm` is linked (Makefile demo target already includes it).
- You can set environment variables like `STEREO_DEBUG=1` to get verbose logs from the interposer.



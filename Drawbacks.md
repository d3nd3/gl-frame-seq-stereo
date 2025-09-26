# Drawbacks and Timing Considerations

This library hooks GL/GLX and coordinates OpenGL buffer swaps with the NVIDIA 3D Vision IR emitter. There are inherent timing constraints on X11 windowed applications under a compositor that can cause left/right phase drift or brief inversion. This document explains why and how we mitigate it.

## Root cause: compositor and event-time variability

- Window dragging/resizing: The window manager/compositor may redirect the window to an offscreen pixmap and add extra copies. The on-screen presentation can be delayed relative to the app's `glXSwapBuffers` call.
- Extra X11 round trips: During cursor motion, OSD drawing, and input polling, additional client/server messages can alter when the compositor applies the final frame.
- If the IR emitter is toggled assuming a specific vblank/parity, but the compositor applies the buffer later (or on the next vblank), the IR shutter phase can drift or flip.

## Previously problematic sequencing

- Swap-on-clear path (`glClear`) could issue a real swap and emitter toggle early, and later `glXSwapBuffers` attempted to sequence again in the same frame, doubling actions.
- During window move/resize, parity waits and emitter toggles could be misaligned by compositor timing.

## Mitigations implemented

- Single sequencing per frame:
  - If swap-on-clear is enabled and we already swapped in `glClear`, `glXSwapBuffers` skips sequencing and only performs cleanup.
- Drag/move stabilization:
  - While dragging/resizing a window, emitter sequencing is paused; only raw swaps occur. After movement ends, a resync to LEFT occurs to reestablish a known phase.
- Fullscreen option:
  - EWMH `_NET_WM_STATE_FULLSCREEN` request, with override-redirect fallback. In fullscreen, compositor interference is minimized.
- USB thread mode integration:
  - When enabled, the main thread performs raw swaps; the background thread drives the emitter. Toggling off forces a resync.
- Key polling/device drain:
  - Regularly drains emitter key events to prevent device stalls.

## Remaining limitations

- On some WMs/compositors, there may still be rare frame-timing anomalies during aggressive window management animations.
- Multi-monitor systems with mixed refresh rates can introduce added jitter if the wrong output is used.

## Hardening with OML scheduling (planned/added)

- Prefer `GLX_OML_sync_control` when available. The emitter is toggled immediately around the swap scheduled for a specific MSC parity via `nvstusb_swap`, and the main thread avoids additional parity waiting.
- Future enhancements could use `glXSwapBuffersMscOML` to atomically schedule a swap on a target parity and toggle IR precisely after swap completion.

## Recommendations

- Use fullscreen mode for the most stable results.
- Avoid moving/resizing the window during critical stereo viewing; the library will try to hold and resync, but visual impact is minimized in fullscreen.


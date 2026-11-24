# gl-frame-seq-stereo

This project interposes core OpenGL/GLX calls to produce frame-sequential stereo and drive an NVIDIA 3D Vision IR emitter.

Prefer pictures? Start with **[HUMANS.md](HUMANS.md)**.

See `Drawbacks.md` for known limitations and mitigations.
Based on earlier libnvstusb code base. credits due.
============
Disclaimer
==========

This is an experimental proof of concept to get the NVIDIA 3d stereo
controller working with OpenGL on linux. If this program or any part
of it causes any damage to your hardware, software or health, it is your
own fault. You have been warned.

I do not claim any copyright or ownership on the firmware or drivers of
any device by NVIDIA.


# LD_PRELOAD OpenGL Frame-Sequential Stereo Wrapper

## Project Overview

This project intends to be an **LD_PRELOAD** OpenGL wrapper that implements frame-sequential stereoscopic 3D vision, specifically for older OpenGL titles (that use the fixed pipeline).

## First Challenge

The initial goal is to support **glxgears** as a proof of concept.

## Common Patterns in Fixed Pipeline OpenGL Apps

Fixed pipeline OpenGL applications typically use a sequence of matrix operations to set up their camera and object transformations. Some common patterns include:

```c
// Setting up the modelview matrix
// (for camera and object transformations)
glMatrixMode(GL_MODELVIEW);
glLoadIdentity();
glTranslatef(0.0, 0.0, -40.0);

// Setting up the projection matrix
// (for perspective transformations)
glMatrixMode(GL_PROJECTION);
glLoadIdentity();
glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
```

Drawing is then performed using routines such as `glVertex3f`, `glDrawArrays`, or `glDrawElements`, often within a `glBegin`/`glEnd` block:

```c
glBegin(GL_TRIANGLES);
    glVertex3f(...);
    // ...
glEnd();
```

## Central Idea: Modifying Matrices Before Draw Calls

The core approach of this wrapper is to modify the **projection** and **modelview** matrices just before any drawing routine is called. This is essential for implementing stereoscopic effects, as the matrices must be adjusted for each eye's view.

### Issues with Interposing Primitives (`glBegin`/`glEnd`)

While hooking `glBegin` might seem like a straightforward approach, it has significant limitations:

- **Order matters in matrix multiplication**: Applying the stereo transform this late in the scene means it's applied last, which can lead to incorrect results since matrix multiplication is not commutative.
- **Transform timing is critical**: The stereo camera transform must be applied early in the frame, before any object-specific transformations, to ensure proper eye separation and convergence.

## Current BUGS
- `gamemoderun` is causing jitter, not sure why yet
    - This is almost certainly due to `gamemoderun` disabling the compositor; the bug is noticeably worse with the compositor off at the moment.
- It claims `glClear` is ran 8 times, not sure why!

## Alternative Approach: Applying Matrix Transformation in `glClear`

Another method for achieving frame-sequential stereoscopic 3D is to apply the matrix transformation to the **modelview** matrix at the start of each frame, specifically by hooking or intercepting the `glClear` call. This approach involves modifying the modelview matrix right after the frame buffer is cleared, before any drawing commands are issued for the new frame.

### Why Use `glClear`?
- **Frame Boundary**: `glClear` is often called at the start of a new frame, making it a convenient place to apply per-frame transformations.
- **Simplicity**: By applying the transformation once per frame, you avoid the complexity of tracking and updating matrices for every primitive or draw call.

### Tradeoffs
- **Assumptions**: This method assumes that the application resets or sets up its matrices at the start of each frame, which is true for many, but not all, fixed pipeline OpenGL apps.
- **Potential Limitations**: If an application changes its matrices mid-frame or draws multiple scenes per frame, this approach may not always produce the intended effect.
- **Inconsistent Usage**: Not all applications call `glClear()` consistently, making this method less reliable in some cases.

### Summary
While `glClear` interception can work for some applications, there are tradeoffs and limitations, but it remains a practical approach for many fixed pipeline OpenGL applications.

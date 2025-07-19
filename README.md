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

### Why Hook `glBegin`?

A key insight is that, in the fixed pipeline, the matrix state must be set **before** the first `glVertex*` call inside a `glBegin`/`glEnd` block. By hooking `glBegin`, we can ensure that our matrix modifications are applied at the correct time, right before drawing starts.

This approach is:
- **Generic and simple**: It works for most fixed pipeline apps without needing to track complex matrix state changes (such as those made by `glLoadMatrixf`, `glMultMatrixf`, `glTranslatef`, etc.).
- **More likely to work out-of-the-box**: Since many apps follow the pattern of setting matrices before drawing, this method is broadly compatible.
- **Tradeoff: Increased GL calls**: If an application makes many `glBegin` calls, this method may result in more frequent matrix modifications than strictly necessary. However, this is a reasonable tradeoff for simplicity and robustness, as it bypasses the need for detailed matrix state tracking.

In summary, the wrapper will intercept `glBegin` to apply the necessary edits to the `GL_PROJECTION` and `GL_MODELVIEW` matrices, ensuring the correct stereoscopic effect is achieved for each draw call, with minimal assumptions about the application's internal state management.

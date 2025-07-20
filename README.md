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

### Better Approach: Matrix Mode Detection

A more robust approach is to detect when the application is setting up its matrices at the start of each frame. This can be achieved by:

1. **Tracking `glMatrixMode(GL_MODELVIEW)` after `glSwapBuffers()`**: Set a boolean flag when the first call to `glMatrixMode(GL_MODELVIEW)` occurs after a buffer swap, indicating the start of a new frame's matrix setup.

2. **Applying stereo transform in `glLoadIdentity()`**: Once the flag is set, intercept `glLoadIdentity()` calls and apply the eye-specific `glTranslate` transformation immediately after the identity matrix is loaded.

3. **Alternative: Using `glClear()`**: While `glClear()` is commonly called at frame start, not all programs use it consistently, making matrix mode detection more reliable.

This approach ensures that:
- The stereo transform is applied early in the frame setup
- The transform order is correct (stereo transform applied before object transforms)
- It works with applications that may not use `glClear()` at frame boundaries

### Why This Method is Superior

This method is:
- **Correctly ordered**: The stereo transform is applied before any object-specific transformations
- **Frame-aware**: It properly detects frame boundaries through matrix mode changes
- **More compatible**: Works with applications that don't follow strict `glClear()` patterns
- **Robust**: Avoids the matrix multiplication order issues of late-stage primitive interposition

In summary, the wrapper will track matrix mode changes to detect frame boundaries and apply stereo transformations at the appropriate time in the matrix setup sequence, ensuring correct stereoscopic effects.

## Alternative Approach: Applying Matrix Transformation in `glClear`

Another method for achieving frame-sequential stereoscopic 3D is to apply the matrix transformation to the **modelview** matrix at the start of each frame, specifically by hooking or intercepting the `glClear` call. This approach involves modifying the modelview matrix right after the frame buffer is cleared, before any drawing commands are issued for the new frame.

### Why Use `glClear`?
- **Frame Boundary**: `glClear` is often called at the start of a new frame, making it a convenient place to apply per-frame transformations.
- **Simplicity**: By applying the transformation once per frame, you avoid the complexity of tracking and updating matrices for every primitive or draw call.

### Tradeoffs
- **Assumptions**: This method assumes that the application resets or sets up its matrices at the start of each frame, which is true for many, but not all, fixed pipeline OpenGL apps.
- **Potential Limitations**: If an application changes its matrices mid-frame or draws multiple scenes per frame, this approach may not always produce the intended effect.
- **Inconsistent Usage**: Not all applications call `glClear()` consistently, making this method less reliable than matrix mode detection.

### Comparison with Matrix Mode Detection

While `glClear` interception is simpler than primitive interposition, the **matrix mode detection approach** (described above) is generally superior because:

- **More Reliable**: Matrix mode detection works even when applications don't use `glClear()`
- **Better Timing**: Ensures transforms are applied at the exact right moment in the matrix setup sequence
- **Correct Order**: Guarantees stereo transforms are applied before object-specific transformations

### Summary
While `glClear` interception can work for some applications, the matrix mode detection approach provides a more robust and universally applicable solution for implementing stereoscopic effects in fixed pipeline OpenGL applications.

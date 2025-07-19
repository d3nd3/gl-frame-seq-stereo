#define _GNU_SOURCE
#include <dlfcn.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>

// Function pointer for the real glClear
static void (*real_glClear)(GLbitfield mask) = NULL;

// Interposed glClear
void glClear(GLbitfield mask) {
    if (!real_glClear) {
        real_glClear = dlsym(RTLD_NEXT, "glClear");
        if (!real_glClear) {
            fprintf(stderr, "[stereo] Failed to find real glClear!\n");
            return;
        }
    }
    // Example: Print a message (remove or replace with stereo logic)
    fprintf(stderr, "[stereo] glClear called\n");
    // Call the real glClear
    real_glClear(mask);
}

// Function pointer for the real glBegin
static void (*real_glBegin)(GLenum mode) = NULL;

// Interposed glBegin
void glBegin(GLenum mode) {
    if (!real_glBegin) {
        real_glBegin = dlsym(RTLD_NEXT, "glBegin");
        if (!real_glBegin) {
            fprintf(stderr, "[stereo] Failed to find real glBegin!\n");
            return;
        }
    }
    // Example: Print a message (remove or replace with stereo logic)
    fprintf(stderr, "[stereo] glBegin called with mode=%u\n", mode);
    // Call the real glBegin
    real_glBegin(mode);
}

// Function pointer for the real glXSwapBuffers
static void (*real_glXSwapBuffers)(Display *dpy, GLXDrawable drawable) = NULL;

// Interposed glXSwapBuffers
void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    if (!real_glXSwapBuffers) {
        real_glXSwapBuffers = dlsym(RTLD_NEXT, "glXSwapBuffers");
        if (!real_glXSwapBuffers) {
            fprintf(stderr, "[stereo] Failed to find real glXSwapBuffers!\n");
            return;
        }
    }
    // Example: Print a message (remove or replace with stereo logic)
    fprintf(stderr, "[stereo] glXSwapBuffers called\n");
    // Call the real glXSwapBuffers
    real_glXSwapBuffers(dpy, drawable);
}

// Add more OpenGL hooks here as needed 
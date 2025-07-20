#define _GNU_SOURCE
#include <dlfcn.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "emitter.h"
#include <time.h>

static float CONVERGENCE = 15.0f;
static float EYE_OFFSET = 1.72f;

//static const float CONVERGENCE = 0.0f;
//static const float EYE_OFFSET = 0.0f;

static Display *current_dpy = NULL;
static GLXDrawable current_drawable = 0;

// Forward declarations
void (*real_glFrustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = NULL;
void (*real_gluPerspective)(GLdouble, GLdouble, GLdouble, GLdouble) = NULL;

// Global storage (basic form)
static double last_fov = 60.0;
static double last_aspect = 4.0 / 3.0;
static double last_near = 0.1;
static double last_far = 100.0;

static double last_frustum_left = -1.0;
static double last_frustum_right = 1.0;
static double last_frustum_bottom = -1.0;
static double last_frustum_top = 1.0;



typedef enum {
    PROJECTION_UNKNOWN = 0,
    PROJECTION_FRUSTUM,
    PROJECTION_PERSPECTIVE,
    PROJECTION_ORTHO
} ProjectionType;

static ProjectionType last_projection_type = PROJECTION_UNKNOWN;

void apply_stereo_projection(void);
static void poll_stereo_hotkeys();
static void (*real_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = NULL;

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble zNear, GLdouble zFar) {
    if (!real_glOrtho) {
        real_glOrtho = dlsym(RTLD_NEXT, "glOrtho");
        if (!real_glOrtho) {
            fprintf(stderr, "[stereo] Failed to find real glOrtho!\n");
            return;
        }
    }

    last_projection_type = PROJECTION_ORTHO;
    last_frustum_left = left;
    last_frustum_right = right;
    last_frustum_bottom = bottom;
    last_frustum_top = top;
    last_near = zNear;
    last_far = zFar;

    // Apply stereo transformation directly
    apply_stereo_projection();
}

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar) {
    if (!real_gluPerspective) {
        real_gluPerspective = dlsym(RTLD_NEXT, "gluPerspective");
        if (!real_gluPerspective) {
            fprintf(stderr, "[stereo] Failed to find real gluPerspective!\n");
            return;
        }
    }

    last_projection_type = PROJECTION_PERSPECTIVE;
    // Store values
    last_fov = fovy;
    last_aspect = aspect;
    last_near = zNear;
    last_far = zFar;

    // Apply stereo transformation directly
    apply_stereo_projection();
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
    GLdouble zNear, GLdouble zFar) {
    if (!real_glFrustum) {
        real_glFrustum = dlsym(RTLD_NEXT, "glFrustum");
        if (!real_glFrustum) {
            fprintf(stderr, "[stereo] Failed to find real glFrustum!\n");
            return;
        }
    }

    last_projection_type = PROJECTION_FRUSTUM;
    // Store values
    last_frustum_left = left;
    last_frustum_right = right;
    last_frustum_bottom = bottom;
    last_frustum_top = top;
    last_near = zNear;
    last_far = zFar;

    // Debug
    fprintf(stderr, "[stereo] glFrustum(l=%.3f, r=%.3f, b=%.3f, t=%.3f, n=%.3f, f=%.3f)\n",
            left, right, bottom, top, zNear, zFar);

    // Apply stereo transformation directly
    apply_stereo_projection();
}





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
    //fprintf(stderr, "[stereo] glClear called\n");
    // Call the real glClear
    real_glClear(mask);
}



// Stereo camera selection
typedef enum { CAMERA_LEFT, CAMERA_RIGHT } StereoCamera;
static StereoCamera current_camera = CAMERA_LEFT;

// Helper to apply stereo projection with proper convergence
void apply_stereo_projection(void) {
    if (last_projection_type == PROJECTION_UNKNOWN) {
        fprintf(stderr, "[stereo] No projection type known. Using default.\n");
        // Apply default projection for first frame
        real_gluPerspective(60.0, 4.0/3.0, 0.1, 100.0);
        return;
    }
    
    // For true convergence, we use the original projection without shifting
    // The convergence angle will be applied in the modelview matrix
    switch (last_projection_type) {
        case PROJECTION_PERSPECTIVE: {
            // Use original symmetric projection
            real_gluPerspective(last_fov, last_aspect, last_near, last_far);
            break;
        }
        case PROJECTION_FRUSTUM: {
            // Use original frustum
            real_glFrustum(last_frustum_left, last_frustum_right,
                          last_frustum_bottom, last_frustum_top,
                          last_near, last_far);
            break;
        }
        case PROJECTION_ORTHO: {
            // For orthographic, we still need to shift since there's no perspective
            float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
            real_glOrtho(last_frustum_left + eye_offset,
                        last_frustum_right + eye_offset,
                        last_frustum_bottom,
                        last_frustum_top,
                        last_near,
                        last_far);
            break;
        }
    }
}


static void adjust_stereo_matrices(StereoCamera cam) {

    GLint original_matrix_mode;
    glGetIntegerv(GL_MATRIX_MODE, &original_matrix_mode);
        
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
        
    // Apply proper stereo transformation
    float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
        
    // Step 1: Translate to eye position
    glTranslatef(eye_offset, 0.0f, 0.0f);
        
    // Step 2: Rotate toward convergence point
    // Calculate convergence angle: arctan(eye_offset / convergence_distance)
    float convergence_angle_rad = atan(eye_offset / CONVERGENCE);
    float convergence_angle_deg = convergence_angle_rad * 180.0f / M_PI;
        
    // Rotate around Y-axis to point cameras inward
    glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
        
    glMatrixMode(original_matrix_mode);
}

// Helper to pop matrices after stereo draw
static void pop_stereo_matrices(void) {
    GLint original_matrix_mode;
    glGetIntegerv(GL_MATRIX_MODE, &original_matrix_mode);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    glMatrixMode(original_matrix_mode);
}


static void (*real_glCallList)(GLuint list) = NULL;
void glCallList(GLuint list) {
    if (!real_glCallList)
        real_glCallList = dlsym(RTLD_NEXT, "glCallList");

    adjust_stereo_matrices(current_camera);   // Push stereo matrices
    real_glCallList(list);
    pop_stereo_matrices();                    // Pop stereo matrices
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
    // Adjust matrices for the current camera
    adjust_stereo_matrices(current_camera);
    fprintf(stderr, "[stereo] glBegin called with mode=%u, camera=%s\n", mode, current_camera == CAMERA_LEFT ? "LEFT" : "RIGHT");
    real_glBegin(mode);
}

// Function pointer for the real glEnd
static void (*real_glEnd)(void) = NULL;

// Interposed glEnd
void glEnd(void) {
    if (!real_glEnd) {
        real_glEnd = dlsym(RTLD_NEXT, "glEnd");
        if (!real_glEnd) {
            fprintf(stderr, "[stereo] Failed to find real glEnd!\n");
            return;
        }
    }
    
    
    //fprintf(stderr, "[stereo] glEnd called, matrices popped\n");
    real_glEnd();

    // Pop matrices after drawing
    pop_stereo_matrices();
    
}

// Function pointer for the real glXSwapBuffers
static void (*real_glXSwapBuffers)(Display *dpy, GLXDrawable drawable) = NULL;

static void glx_swap_callback() {
    if (real_glXSwapBuffers && current_dpy && current_drawable) {
        real_glXSwapBuffers(current_dpy, current_drawable);
    }
}


// Initialization flag for glXSwapBuffers
static int glxswapbuffers_initialized = 0;
// Interposed glXSwapBuffers
void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    current_dpy = dpy;
    current_drawable = drawable;
    poll_stereo_hotkeys();

    if (!real_glXSwapBuffers) {
        real_glXSwapBuffers = dlsym(RTLD_NEXT, "glXSwapBuffers");
        if (!real_glXSwapBuffers) {
            fprintf(stderr, "[stereo] Failed to find real glXSwapBuffers!\n");
            return;
        }
    }
    // Perform initialization on first call
    if (!glxswapbuffers_initialized) {
        glxswapbuffers_initialized = 1;
        // TODO: Add your initialization logic here

        init_emitter();
        
    }

    if ( current_camera == CAMERA_LEFT ) {
        nvstusb_swap(nv_ctx, nvstusb_left, glx_swap_callback);
        current_camera = CAMERA_RIGHT;
    } else {
        nvstusb_swap(nv_ctx, nvstusb_right, glx_swap_callback);
        current_camera = CAMERA_LEFT;
    }
    // Example: Print a message (remove or replace with stereo logic)
    //fprintf(stderr, "[stereo] glXSwapBuffers called\n");
    // Call the real glXSwapBuffers
    //real_glXSwapBuffers(dpy, drawable);
}





// Add more OpenGL hooks here as needed 

// Matrix stack tracking for interposed glPushMatrix/glPopMatrix
static int matrix_stack_depth = 0;

// Function pointer for the real glPushMatrix and glPopMatrix
static void (*real_glPushMatrix)(void) = NULL;
static void (*real_glPopMatrix)(void) = NULL;

// Returns 1 if the stack is empty, 0 otherwise
int is_matrix_stack_empty(void) {
    return matrix_stack_depth == 0;
}

void glPushMatrix(void) {
    if (!real_glPushMatrix) {
        real_glPushMatrix = dlsym(RTLD_NEXT, "glPushMatrix");
        if (!real_glPushMatrix) {
            fprintf(stderr, "[stereo] Failed to find real glPushMatrix!\n");
            return;
        }
    }
    matrix_stack_depth++;
    real_glPushMatrix();
}

void glPopMatrix(void) {
    if (!real_glPopMatrix) {
        real_glPopMatrix = dlsym(RTLD_NEXT, "glPopMatrix");
        if (!real_glPopMatrix) {
            fprintf(stderr, "[stereo] Failed to find real glPopMatrix!\n");
            return;
        }
    }
    if (matrix_stack_depth == 0) {
        fprintf(stderr, "[stereo] glPopMatrix called with empty stack!\n");
    } else {
        matrix_stack_depth--;
    }
    real_glPopMatrix();
}

static void poll_stereo_hotkeys() {
    static int initialized = 0;
    static Display *dpy = NULL;
    static Window root = 0;
    char keys[32];
    if (!initialized) {
        dpy = XOpenDisplay(NULL);
        if (!dpy) {
            fprintf(stderr, "[stereo] Could not open X display for hotkey polling!\n");
            return;
        }
        root = DefaultRootWindow(dpy);
        initialized = 1;
    }
    XQueryKeymap(dpy, keys);
    // Helper macro to check key state
    #define KEY_IS_DOWN(keysym) (keys[XKeysymToKeycode(dpy, (keysym))/8] & (1 << (XKeysymToKeycode(dpy, (keysym))%8)))
    static struct timespec last_f7 = {0,0}, last_f8 = {0,0}, last_f9 = {0,0}, last_f10 = {0,0};
    const long repeat_ms = 100; // 100 ms repeat rate
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int cur_f7 = KEY_IS_DOWN(XK_F7);
    int cur_f8 = KEY_IS_DOWN(XK_F8);
    int cur_f9 = KEY_IS_DOWN(XK_F9);
    int cur_f10 = KEY_IS_DOWN(XK_F10);
    // Helper macro for time diff in ms
    #define ELAPSED_MS(last) (((now.tv_sec - (last).tv_sec) * 1000) + ((now.tv_nsec - (last).tv_nsec) / 1000000))
    if (cur_f7 && ELAPSED_MS(last_f7) >= repeat_ms) {
        CONVERGENCE += 5.0f;
        fprintf(stderr, "[stereo] CONVERGENCE decreased: %f\n", CONVERGENCE);
        last_f7 = now;
    }
    if (cur_f8 && ELAPSED_MS(last_f8) >= repeat_ms) {
        CONVERGENCE -= 5.0f;
        fprintf(stderr, "[stereo] CONVERGENCE increased: %f\n", CONVERGENCE);
        last_f8 = now;
    }
    if (cur_f9 && ELAPSED_MS(last_f9) >= repeat_ms) {
        EYE_OFFSET -= 0.01f;
        //if (EYE_OFFSET < 0.0f) EYE_OFFSET = 0.0f;
        fprintf(stderr, "[stereo] EYE_OFFSET decreased: %f\n", EYE_OFFSET);
        last_f9 = now;
    }
    if (cur_f10 && ELAPSED_MS(last_f10) >= repeat_ms) {
        EYE_OFFSET += 0.01f;
        fprintf(stderr, "[stereo] EYE_OFFSET increased: %f\n", EYE_OFFSET);
        last_f10 = now;
    }
    #undef ELAPSED_MS
    // No need for prev_f7, prev_f8, prev_f9, prev_f10 anymore
}

// Hook glXGetProcAddress to intercept when glxgears requests glXSwapBuffers
__GLXextFuncPtr glXGetProcAddress(const GLubyte* procName) {
    // printf("[stereopreload] glXGetProcAddress called with: %s\n", procName);
    
    // Get the real function
    static __GLXextFuncPtr (*real_glXGetProcAddress)(const GLubyte*) = NULL;
    if (!real_glXGetProcAddress) {
        real_glXGetProcAddress = (__GLXextFuncPtr (*)(const GLubyte*))dlsym(RTLD_NEXT, "glXGetProcAddress");
        if (!real_glXGetProcAddress) {
            fprintf(stderr, "[stereopreload] Failed to find real glXGetProcAddress\n");
            return NULL;
        }
    }
    
    // Call the real function to get the original pointer
    __GLXextFuncPtr result = real_glXGetProcAddress(procName);
    
    // If this is glXSwapBuffers, return our hooked version instead
    if (strcmp((const char*)procName, "glXSwapBuffers") == 0) {
        // printf("[stereopreload] Intercepting glXSwapBuffers request, returning our hooked version\n");
        return (__GLXextFuncPtr)glXSwapBuffers; // Return our hooked version
    }
    
    // Return the original function pointer for everything else
    return result;
}

// Hook glXGetProcAddressARB as well (some apps use this)
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte* procName) {
    // printf("[stereopreload] glXGetProcAddressARB called with: %s\n", procName);
    
    // Get the real function
    static __GLXextFuncPtr (*real_glXGetProcAddressARB)(const GLubyte*) = NULL;
    if (!real_glXGetProcAddressARB) {
        real_glXGetProcAddressARB = (__GLXextFuncPtr (*)(const GLubyte*))dlsym(RTLD_NEXT, "glXGetProcAddressARB");
        if (!real_glXGetProcAddressARB) {
            fprintf(stderr, "[stereopreload] Failed to find real glXGetProcAddressARB\n");
            return NULL;
        }
    }
    
    // Call the real function to get the original pointer
    __GLXextFuncPtr result = real_glXGetProcAddressARB(procName);
    
    // If this is glXSwapBuffers, return our hooked version instead
    if (strcmp((const char*)procName, "glXSwapBuffers") == 0) {
        // printf("[stereopreload] Intercepting glXSwapBuffers request (ARB), returning our hooked version\n");
        return (__GLXextFuncPtr)glXSwapBuffers; // Return our hooked version
    }
    
    // Return the original function pointer for everything else
    return result;
}
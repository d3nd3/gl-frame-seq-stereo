#define _GNU_SOURCE
#include <dlfcn.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "emitter.h"
#include "nvstusb.h"
#include <time.h>

static float CONVERGENCE = 275.0f;
static float EYE_OFFSET = 1.17f;

static Display *current_dpy = NULL;
static GLXDrawable current_drawable = 0;

// Global real function pointers
void (*real_glFrustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = NULL;
void (*real_gluPerspective)(GLdouble, GLdouble, GLdouble, GLdouble) = NULL;
void (*real_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = NULL;
void (*real_glClear)(GLbitfield mask) = NULL;
void (*real_glLoadIdentity)(void) = NULL;
void (*real_glMatrixMode)(GLenum mode) = NULL;
void (*real_glPushMatrix)(void) = NULL;
void (*real_glPopMatrix)(void) = NULL;
void (*real_glTranslatef)(GLfloat, GLfloat, GLfloat) = NULL;
void (*real_glRotatef)(GLfloat, GLfloat, GLfloat, GLfloat) = NULL;
void (*real_glXSwapBuffers)(Display *dpy, GLXDrawable drawable) = NULL;
GLXContext (*real_glXCreateContext)(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct) = NULL;
__GLXextFuncPtr (*real_glXGetProcAddress)(const GLubyte*) = NULL;
__GLXextFuncPtr (*real_glXGetProcAddressARB)(const GLubyte*) = NULL;

typedef void (*fp_glXSwapBuffers)(Display* dpy, GLXDrawable drawable);
typedef __GLXextFuncPtr (*fp_glXGetProcAddress) (const GLubyte* procName);

// Forward declaration for apply_stereo_projection
void apply_stereo_projection(void);
// Forward declaration for poll_stereo_hotkeys
static void poll_stereo_hotkeys();
// Forward declaration for handle_user_input
static void handle_user_input(void);

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

// Global storage for saved modelview matrix
static GLfloat savedModelViewMatrix[16];
static int hasSavedMatrix = 0;

// Stereo camera selection
typedef enum { CAMERA_LEFT, CAMERA_RIGHT } StereoCamera;
static StereoCamera current_camera = CAMERA_LEFT;

// Global state tracking for eye transform logic
static int afterSwapBuffers = 0;
static int afterGLmatrixMode = 0;
static int appliedEyeTransformThisFrame = 0;

// Flag to disable camera transform
static int camera_transform_disabled = 0;

// Global initialization flag
static int real_functions_initialized = 0;

// Initialize all real function pointers
static void initialize_real_functions(void) {
    if (real_functions_initialized) {
        return;
    }
    
    real_glFrustum = dlsym(RTLD_NEXT, "glFrustum");
    real_gluPerspective = dlsym(RTLD_NEXT, "gluPerspective");
    real_glOrtho = dlsym(RTLD_NEXT, "glOrtho");
    real_glClear = dlsym(RTLD_NEXT, "glClear");
    real_glLoadIdentity = dlsym(RTLD_NEXT, "glLoadIdentity");
    real_glMatrixMode = dlsym(RTLD_NEXT, "glMatrixMode");
    real_glPushMatrix = dlsym(RTLD_NEXT, "glPushMatrix");
    real_glPopMatrix = dlsym(RTLD_NEXT, "glPopMatrix");
    real_glTranslatef = dlsym(RTLD_NEXT, "glTranslatef");
    real_glRotatef = dlsym(RTLD_NEXT, "glRotatef");
    real_glXSwapBuffers = dlsym(RTLD_NEXT, "glXSwapBuffers");
    real_glXCreateContext = dlsym(RTLD_NEXT, "glXCreateContext");
    real_glXGetProcAddress = (__GLXextFuncPtr (*)(const GLubyte*))dlsym(RTLD_NEXT, "glXGetProcAddress");
    real_glXGetProcAddressARB = (__GLXextFuncPtr (*)(const GLubyte*))dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    
    // Check for any failed lookups
    if (!real_glFrustum) fprintf(stderr, "[stereo] Failed to find real glFrustum!\n");
    if (!real_gluPerspective) fprintf(stderr, "[stereo] Failed to find real gluPerspective!\n");
    if (!real_glOrtho) fprintf(stderr, "[stereo] Failed to find real glOrtho!\n");
    if (!real_glClear) fprintf(stderr, "[stereo] Failed to find real glClear!\n");
    if (!real_glLoadIdentity) fprintf(stderr, "[stereo] Failed to find real glLoadIdentity!\n");
    if (!real_glMatrixMode) fprintf(stderr, "[stereo] Failed to find real glMatrixMode!\n");
    if (!real_glPushMatrix) fprintf(stderr, "[stereo] Failed to find real glPushMatrix!\n");
    if (!real_glPopMatrix) fprintf(stderr, "[stereo] Failed to find real glPopMatrix!\n");
    if (!real_glTranslatef) fprintf(stderr, "[stereo] Failed to find real glTranslatef!\n");
    if (!real_glRotatef) fprintf(stderr, "[stereo] Failed to find real glRotatef!\n");
    if (!real_glXSwapBuffers) fprintf(stderr, "[stereo] Failed to find real glXSwapBuffers!\n");
    if (!real_glXCreateContext) fprintf(stderr, "[stereo] Failed to find real glXCreateContext!\n");
    if (!real_glXGetProcAddress) fprintf(stderr, "[stereo] Failed to find real glXGetProcAddress!\n");
    if (!real_glXGetProcAddressARB) fprintf(stderr, "[stereo] Failed to find real glXGetProcAddressARB!\n");
    
    real_functions_initialized = 1;
}

// Shared function to handle glXGetProcAddress interception
static __GLXextFuncPtr handle_glx_get_proc_address(const GLubyte* procName, __GLXextFuncPtr (*real_func)(const GLubyte*), const char* func_name) {
    initialize_real_functions();
    
    if (!real_func) {
        fprintf(stderr, "[stereopreload] Failed to find real %s\n", func_name);
        return NULL;
    }
    
    // Call the real function to get the original pointer
    __GLXextFuncPtr result = real_func(procName);
    
    // If this is glXSwapBuffers, return our hooked version instead
    if (strcmp((const char*)procName, "glXSwapBuffers") == 0) {
        return (__GLXextFuncPtr)glXSwapBuffers; // Return our hooked version
    }
    
    // If this is glLoadIdentity, return our hooked version instead
    if (strcmp((const char*)procName, "glLoadIdentity") == 0) {
        return (__GLXextFuncPtr)glLoadIdentity; // Return our hooked version
    }
    
    // If this is glMatrixMode, return our hooked version instead
    if (strcmp((const char*)procName, "glMatrixMode") == 0) {
        return (__GLXextFuncPtr)glMatrixMode; // Return our hooked version
    }
    
    // If this is glTranslatef, return our hooked version instead
    if (strcmp((const char*)procName, "glTranslatef") == 0) {
        return (__GLXextFuncPtr)glTranslatef; // Return our hooked version
    }
    
    // If this is glRotatef, return our hooked version instead
    if (strcmp((const char*)procName, "glRotatef") == 0) {
        return (__GLXextFuncPtr)glRotatef; // Return our hooked version
    }
    
    // If this is glXCreateContext, return our hooked version instead
    if (strcmp((const char*)procName, "glXCreateContext") == 0) {
        return (__GLXextFuncPtr)glXCreateContext; // Return our hooked version
    }
    
    // Return the original function pointer for everything else
    return result;
}

/*
  called by glFrustum() etc.
*/
static void saveModelMatrixAndTriggerReApply(void) {
    // Save the current modelview matrix
    glGetFloatv(GL_MODELVIEW_MATRIX, savedModelViewMatrix);
    hasSavedMatrix = 1;
    

    //act as though new frame to re-apply
    afterSwapBuffers = 1;
    afterGLmatrixMode = 0;
    appliedEyeTransformThisFrame = 0;
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble zNear, GLdouble zFar) {
  

    real_glOrtho(left, right, bottom, top, zNear, zFar);

    saveModelMatrixAndTriggerReApply();
}

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar) {


    real_gluPerspective(fovy, aspect, zNear, zFar);

    saveModelMatrixAndTriggerReApply();
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
    GLdouble zNear, GLdouble zFar) {
  

    // Debug
    fprintf(stderr, "[stereo] glFrustum(l=%.3f, r=%.3f, b=%.3f, t=%.3f, n=%.3f, f=%.3f)\n",
            left, right, bottom, top, zNear, zFar);

    // Apply stereo transformation directly
    //apply_stereo_projection();
    real_glFrustum(left,right, bottom, top, zNear, zFar);

    saveModelMatrixAndTriggerReApply();
}



// Common function to apply eye transform logic
static void apply_eye_transform(void) {
    if (appliedEyeTransformThisFrame) {
        return; // Already applied this frame
    }
    
    // Check if camera transform is disabled
    if (camera_transform_disabled) {
        return; // Skip stereo transformation
    }
    printf("Applied apply_eye_transform()\n");

    GLint matrix_mode;
    glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
    
    real_glMatrixMode(GL_MODELVIEW);

    // Save the current modelview matrix
    glGetFloatv(GL_MODELVIEW_MATRIX, savedModelViewMatrix);
    hasSavedMatrix = 1;
    
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
    
    glMatrixMode(matrix_mode);
    
    appliedEyeTransformThisFrame = 1;
    
}

// Interposed glClear - apply proper stereo view transformation
void glClear(GLbitfield mask) {

    //This line is important atm, causes black window if not here.
    //If we call glClear(), apply on-top of the glMatrixMode(GL_MODELVIEW), glLoadIdentity() mechanism.
    //appliedEyeTransformThisFrame = 0;

    // Apply stereo view transformation at the start of each frame
    if (mask & GL_COLOR_BUFFER_BIT) {
        printf("Attempting to apply_eye_transform from glClear()\n");
        apply_eye_transform();
    }
    
    real_glClear(mask);
}

// Interposed glMatrixMode - track when we're in MODELVIEW mode after swap
void glMatrixMode(GLenum mode) {
    
    // Set afterGLmatrixMode if we're in MODELVIEW mode and afterSwapBuffers is true
    if (afterSwapBuffers && mode == GL_MODELVIEW) {
        afterSwapBuffers = 0;
        afterGLmatrixMode = 1;
    }
    
    real_glMatrixMode(mode);
}

// Interposed glLoadIdentity - apply eye transform if conditions are met
void glLoadIdentity(void) {

    real_glLoadIdentity();

    // Apply eye transform if afterGLmatrixMode is true
    if (afterGLmatrixMode) {
        afterGLmatrixMode = 0;

        printf("Attempting to apply_eye_transform from glLoadIdetntity()\n");
        apply_eye_transform();
        
    }
}

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
    
   
    
    // Perform initialization on first call
    if (!glxswapbuffers_initialized) {
        glxswapbuffers_initialized = 1;
        init_emitter();
    }

    // Handle stereo frame sequencing
    if (current_camera == CAMERA_LEFT) {
        nvstusb_swap(nv_ctx, nvstusb_left, glx_swap_callback);
        current_camera = CAMERA_RIGHT;
    } else {
        nvstusb_swap(nv_ctx, nvstusb_right, glx_swap_callback);
        current_camera = CAMERA_LEFT;
    }

    // Restore the saved modelview matrix
    if (hasSavedMatrix) {
        GLint matrix_mode;
        glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
        real_glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(savedModelViewMatrix);
        real_glMatrixMode(matrix_mode);
        hasSavedMatrix = 0;
    }

    afterSwapBuffers = 1;
    appliedEyeTransformThisFrame = 0;
    afterGLmatrixMode = 0;

    poll_stereo_hotkeys();
    // Handle user input from the 3D controller
    handle_user_input();
}

// Matrix stack tracking - keep your existing implementation
static int matrix_stack_depth = 0;

int is_matrix_stack_empty(void) {
    return matrix_stack_depth == 0;
}

void glPushMatrix(void) {
   
    matrix_stack_depth++;
    real_glPushMatrix();
}

void glPopMatrix(void) {
  
    if (matrix_stack_depth == 0) {
        fprintf(stderr, "[stereo] glPopMatrix called with empty stack!\n");
    } else {
        matrix_stack_depth--;
    }
    real_glPopMatrix();
}

// Interposed glTranslatef - can be used for debugging or additional transformations
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    // Debug output
    fprintf(stderr, "[stereo] glTranslatef(x=%.3f, y=%.3f, z=%.3f)\n", x, y, z);
    
    // Call the real function
    real_glTranslatef(x, y, z);
}

// Interposed glRotatef - can be used for debugging or additional transformations
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    // Debug output
    fprintf(stderr, "[stereo] glRotatef(angle=%.3f, x=%.3f, y=%.3f, z=%.3f)\n", angle, x, y, z);
    
    // Call the real function
    real_glRotatef(angle, x, y, z);
}

// Interposed glXCreateContext - initialize real functions when context is created
GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct) {
    initialize_real_functions();
    
    if (!real_glXCreateContext) {
        fprintf(stderr, "[stereo] Failed to find real glXCreateContext!\n");
        return NULL;
    }
    
    return real_glXCreateContext(dpy, vis, shareList, direct);
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

static void handle_user_input() {
    struct nvstusb_keys k;
    nvstusb_get_keys(nv_ctx, &k);
    if (k.toggled3D) {
        if (k.toggled3D < 0) {
            camera_transform_disabled = 1;
            fprintf(stderr, "[stereo] Camera transform disabled\n");
        } else {
            camera_transform_disabled = 0;
            fprintf(stderr, "[stereo] Camera transform enabled\n");
        }
    }
}

// Hook glXGetProcAddress to intercept when glxgears requests glXSwapBuffers
__GLXextFuncPtr glXGetProcAddress(const GLubyte* procName) {
    // printf("[stereopreload] glXGetProcAddress called with: %s\n", procName);
    return handle_glx_get_proc_address(procName, real_glXGetProcAddress, "glXGetProcAddress");
}

// Hook glXGetProcAddressARB as well (some apps use this)
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte* procName) {
    // printf("[stereopreload] glXGetProcAddressARB called with: %s\n", procName);
    return handle_glx_get_proc_address(procName, real_glXGetProcAddressARB, "glXGetProcAddressARB");
}
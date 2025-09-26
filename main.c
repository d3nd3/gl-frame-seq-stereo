#define _GNU_SOURCE
#include <dlfcn.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "emitter.h"
#include "nvstusb.h"
#include <time.h>

#include <sys/time.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>

#define TIMER_RES_NANO
#ifdef TIMER_RES_NANO

#define CAMERA_TRANSFORM_TIMEOUT_USEC_METHOD1 1
#define CAMERA_TRANSFORM_TIMEOUT_USEC_METHOD2_CLEAR 1

#else

#define CAMERA_TRANSFORM_TIMEOUT_USEC_METHOD1 1
#define CAMERA_TRANSFORM_TIMEOUT_USEC_METHOD2_CLEAR 1

#endif
static int cameraTimerSet = 0;
static struct timespec cameraSetupStart;

static float CONVERGENCE = 50.0f;
static float EYE_OFFSET = -1.50f;
static float STEP_CONV = 10.0f;
static float STEP_EYE = 0.03f;
static int force_swap_interval = 1; // mitigation 1: enforce vsync
static int swap_eyes = 0;           // mitigation 2: flip eyes
static int resync_requested = 0;    // mitigation 2b: resync next frame
static int inject_in_clear_enabled = 1; // mitigation 3: frame-start injection
static int osd_notifications_enabled = 1; // mitigation 4: quiet notifications
static int bypass_compositor_main = 1;  // mitigation 5: set _NET_WM_BYPASS_COMPOSITOR on target window
static int bypass_compositor_osd  = 1;  // mitigation 5b: set same hint on OSD
static int bypass_main_applied = 0;
static int bypass_main_needs_apply = 1;
static int fullscreen_enabled = 0;
static int fullscreen_applied = 0;
static int fullscreen_needs_apply = 0;
static int saved_window_geom_valid = 0;
static int saved_win_x = 0, saved_win_y = 0;
static unsigned int saved_win_w = 0, saved_win_h = 0;
// Window-move stabilization tracking
static int move_hold_active = 0;
static struct timespec move_hold_until = {0,0};
static int last_attr_valid = 0;
static int last_attr_x = 0, last_attr_y = 0, last_attr_w = 0, last_attr_h = 0;
// Mitigation 6: drive emitter swap at frame start (glClear) instead of at swap
static int swap_on_clear_enabled = 0;
static int swap_pending_for_frame = 0;
static int swap_done_this_frame = 0;
// Mitigation 7: lock eye selection to video sync parity
static int video_sync_parity_enabled = 0;
static int (*glXGetVideoSyncSGI_ptr)(unsigned int*) = NULL;
static int (*glXWaitVideoSyncSGI_ptr)(int, int, unsigned int*) = NULL;
static int parity_invert = 0; // 0: even=left, 1: odd=left

static inline int get_left_parity(void) { return parity_invert ? 1 : 0; }

static inline int is_video_sync_available(void) {
    return glXGetVideoSyncSGI_ptr != NULL; // glXWaitVideoSyncSGI_ptr may be NULL when not waiting
}

static inline int query_vblank_parity(unsigned int *outCount) {
    if (!is_video_sync_available()) return 0;
    unsigned int c = 0;
    if (glXGetVideoSyncSGI_ptr(&c) == 0) {
        if (outCount) *outCount = c;
        return (int)(c & 1u);
    }
    return -1;
}

static inline int select_eye_from_parity(void) {
    unsigned int cnt = 0;
    int parity = query_vblank_parity(&cnt);
    if (parity < 0) return -1;
    int left_parity = get_left_parity();
    return (parity == left_parity) ? 0 : 1; // 0 for left, 1 for right as simple ints
}

// FPS overlay
static int fps_enabled = 0;
static Window fps_win = 0;
static GC fps_gc = 0;
static int fps_initialized = 0;
static struct timespec fps_prev_time = {0,0};
static double fps_value = 0.0; // smoothed FPS
static double fps_alpha = 0.15; // EMA weight

static Display *current_dpy = NULL;
static GLXDrawable current_drawable = 0;
static int window_events_selected = 0;
static struct timespec last_configure_time = {0,0};
static int stabilize_moves_enabled = 1;
static int stabilize_window_ms = 240;
// Additional diagnostics
static int finish_before_swap = 0;
static int usb_thread_mode = 0; // drive emitter via usb thread, decouple from per-frame swaps
static float usb_rate_hz = 120.0f;
// Prefer OML sync control when available
static int (*glXGetSyncValuesOML_ptr)(Display*, GLXDrawable, long long*, long long*, long long*) = NULL;
static int (*glXWaitForMscOML_ptr)(Display*, GLXDrawable, long long, long long, long long, long long*, long long*, long long*) = NULL;

// Global real function pointers
void (*real_glFrustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = NULL;
void (*real_gluPerspective)(GLdouble, GLdouble, GLdouble, GLdouble) = NULL;
void (*real_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = NULL;
void (*real_glClear)(GLbitfield mask) = NULL;
void (*real_glLoadIdentity)(void) = NULL;
void (*real_glLoadMatrixf)(const GLfloat *m) = NULL;
void (*real_glLoadMatrixd)(const GLdouble *m) = NULL;
void (*real_glMatrixMode)(GLenum mode) = NULL;
void (*real_glPushMatrix)(void) = NULL;
void (*real_glPopMatrix)(void) = NULL;
void (*real_glTranslatef)(GLfloat, GLfloat, GLfloat) = NULL;
void (*real_glRotatef)(GLfloat, GLfloat, GLfloat, GLfloat) = NULL;
void (*real_glXSwapBuffers)(Display *dpy, GLXDrawable drawable) = NULL;
GLXContext (*real_glXCreateContext)(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct) = NULL;
__GLXextFuncPtr (*real_glXGetProcAddress)(const GLubyte*) = NULL;
__GLXextFuncPtr (*real_glXGetProcAddressARB)(const GLubyte*) = NULL;
Bool (*real_glXMakeCurrent)(Display *dpy, GLXDrawable drawable, GLXContext ctx) = NULL;
static void (*real_glBegin)(GLenum mode) = NULL;
static void (*real_glEnd)(void) = NULL;
static void (*real_glCallList)(GLuint list) = NULL;

typedef void (*fp_glXSwapBuffers)(Display* dpy, GLXDrawable drawable);
typedef __GLXextFuncPtr (*fp_glXGetProcAddress) (const GLubyte* procName);

// Forward declaration for apply_stereo_projection
void apply_stereo_projection(void);
// Forward declaration for poll_stereo_hotkeys
static void poll_stereo_hotkeys();
// Forward declaration for handle_user_input
static void handle_user_input(void);
// Forward declaration for internal swap callback
static void glx_swap_callback(void);

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
// Track whether we've pushed a per-frame MODELVIEW transform in glClear
static int transformPushedThisFrame = 0;
// Track whether we've pushed a per-frame PROJECTION adjustment in glClear
static int projectionPushedThisFrame = 0;

// Flag to disable camera transform
static int camera_transform_disabled = 0;

// Global initialization flag
static int real_functions_initialized = 0;
static int inject_at_begin = 0;
static int tint_enabled = 0;

// Debug gate
static int stereo_debug_enabled(void) {
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        enabled = getenv("STEREO_DEBUG") != NULL;
        initialized = 1;
    }
    return enabled;
}

// Load tuning from environment at library load
static void init_stereo_from_env(void) __attribute__((constructor));
static void init_stereo_from_env(void) {
    const char *s;
    if ((s = getenv("STEREO_EYE_OFFSET")) != NULL) {
        float v = strtof(s, NULL);
        if (v != 0.0f) {
            EYE_OFFSET = v;
            fprintf(stderr, "[stereo] EYE_OFFSET from env: %f\n", EYE_OFFSET);
        }
    }
    if ((s = getenv("STEREO_CONVERGENCE")) != NULL) {
        float v = strtof(s, NULL);
        if (v != 0.0f) {
            CONVERGENCE = v;
            fprintf(stderr, "[stereo] CONVERGENCE from env: %f\n", CONVERGENCE);
        }
    }
    if ((s = getenv("STEREO_INJECT_AT_BEGIN")) != NULL) {
        inject_at_begin = atoi(s) != 0;
    }
    if ((s = getenv("STEREO_TINT")) != NULL) {
        tint_enabled = atoi(s) != 0;
    }
}

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
    real_glLoadMatrixf = dlsym(RTLD_NEXT, "glLoadMatrixf");
    real_glLoadMatrixd = dlsym(RTLD_NEXT, "glLoadMatrixd");
    real_glMatrixMode = dlsym(RTLD_NEXT, "glMatrixMode");
    real_glPushMatrix = dlsym(RTLD_NEXT, "glPushMatrix");
    real_glPopMatrix = dlsym(RTLD_NEXT, "glPopMatrix");
    real_glTranslatef = dlsym(RTLD_NEXT, "glTranslatef");
    real_glRotatef = dlsym(RTLD_NEXT, "glRotatef");
    real_glXSwapBuffers = dlsym(RTLD_NEXT, "glXSwapBuffers");
    real_glXCreateContext = dlsym(RTLD_NEXT, "glXCreateContext");
    real_glXMakeCurrent = (Bool (*)(Display*, GLXDrawable, GLXContext))dlsym(RTLD_NEXT, "glXMakeCurrent");
    real_glXGetProcAddress = (__GLXextFuncPtr (*)(const GLubyte*))dlsym(RTLD_NEXT, "glXGetProcAddress");
    real_glXGetProcAddressARB = (__GLXextFuncPtr (*)(const GLubyte*))dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    real_glBegin = dlsym(RTLD_NEXT, "glBegin");
    real_glEnd = dlsym(RTLD_NEXT, "glEnd");
    
    // Check for any failed lookups
    if (stereo_debug_enabled() && !real_glFrustum) fprintf(stderr, "[stereo] Failed to find real glFrustum!\n");
    if (stereo_debug_enabled() && !real_gluPerspective) fprintf(stderr, "[stereo] Failed to find real gluPerspective!\n");
    if (stereo_debug_enabled() && !real_glOrtho) fprintf(stderr, "[stereo] Failed to find real glOrtho!\n");
    if (stereo_debug_enabled() && !real_glClear) fprintf(stderr, "[stereo] Failed to find real glClear!\n");
    if (stereo_debug_enabled() && !real_glLoadIdentity) fprintf(stderr, "[stereo] Failed to find real glLoadIdentity!\n");
    if (stereo_debug_enabled() && !real_glLoadMatrixf) fprintf(stderr, "[stereo] Failed to find real glLoadMatrixf!\n");
    if (stereo_debug_enabled() && !real_glLoadMatrixd) fprintf(stderr, "[stereo] Failed to find real glLoadMatrixd!\n");
    if (stereo_debug_enabled() && !real_glMatrixMode) fprintf(stderr, "[stereo] Failed to find real glMatrixMode!\n");
    if (stereo_debug_enabled() && !real_glPushMatrix) fprintf(stderr, "[stereo] Failed to find real glPushMatrix!\n");
    if (stereo_debug_enabled() && !real_glPopMatrix) fprintf(stderr, "[stereo] Failed to find real glPopMatrix!\n");
    if (stereo_debug_enabled() && !real_glTranslatef) fprintf(stderr, "[stereo] Failed to find real glTranslatef!\n");
    if (stereo_debug_enabled() && !real_glRotatef) fprintf(stderr, "[stereo] Failed to find real glRotatef!\n");
    if (stereo_debug_enabled() && !real_glXSwapBuffers) fprintf(stderr, "[stereo] Failed to find real glXSwapBuffers!\n");
    if (stereo_debug_enabled() && !real_glXCreateContext) fprintf(stderr, "[stereo] Failed to find real glXCreateContext!\n");
    if (stereo_debug_enabled() && !real_glXMakeCurrent) fprintf(stderr, "[stereo] Failed to find real glXMakeCurrent!\n");
    if (stereo_debug_enabled() && !real_glXGetProcAddress) fprintf(stderr, "[stereo] Failed to find real glXGetProcAddress!\n");
    if (stereo_debug_enabled() && !real_glXGetProcAddressARB) fprintf(stderr, "[stereo] Failed to find real glXGetProcAddressARB!\n");
    if (stereo_debug_enabled() && !real_glBegin) fprintf(stderr, "[stereo] Failed to find real glBegin!\n");
    if (stereo_debug_enabled() && !real_glEnd) fprintf(stderr, "[stereo] Failed to find real glEnd!\n");
    
    real_functions_initialized = 1;
}

// ------------------------
// OSD overlay (notification + interactive menu)
// ------------------------
typedef enum { OSD_STATE_HIDDEN = 0, OSD_STATE_NOTIFY = 1, OSD_STATE_MENU = 2 } OsdState;

typedef struct { int x; int y; int w; int h; } OsdRect;

static Display *osd_dpy = NULL;
static Window osd_win = 0;
static GC osd_gc = 0;
static int osd_initialized = 0;
static OsdState osd_state = OSD_STATE_HIDDEN;
static struct timespec osd_hide_time;
static int osd_width = 600;
static int osd_height = 520;

// Fields/edit state
typedef enum { EDIT_NONE = 0, EDIT_CONVERGENCE = 1, EDIT_EYEOFFSET = 2, EDIT_CONV_STEP = 3, EDIT_EYE_STEP = 4 } EditField;
static EditField osd_editing = EDIT_NONE;
static char osd_edit_buf[64];

// Rebind state
typedef enum {
    ACT_CONV_INC = 0,
    ACT_CONV_DEC = 1,
    ACT_EYE_INC  = 2,
    ACT_EYE_DEC  = 3,
    ACT_OSD_TOGGLE = 4,
    ACT_COUNT = 5
} OsdAction;
static int action_keycode[ACT_COUNT] = {0};
static KeySym action_keysym[ACT_COUNT] = {0};
static int action_repeatable[ACT_COUNT] = {1,1,1,1,0};
static int action_repeat_ms[ACT_COUNT] = {60,60,60,60,250};
static struct timespec action_last_time[ACT_COUNT] = {{0,0}};
static int action_prev_down[ACT_COUNT] = {0};
static int osd_rebind_waiting = -1; // -1 if not waiting

// UI layout rects
static OsdRect rc_title = {0,0,0,0};
static OsdRect rc_close = {0,0,0,0};
static OsdRect rc_conv_label = {0,0,0,0};
static OsdRect rc_conv_minus = {0,0,0,0};
static OsdRect rc_conv_val   = {0,0,0,0};
static OsdRect rc_conv_plus  = {0,0,0,0};
static OsdRect rc_conv_step_label = {0,0,0,0};
static OsdRect rc_conv_step_val   = {0,0,0,0};
static OsdRect rc_eye_label  = {0,0,0,0};
static OsdRect rc_eye_minus  = {0,0,0,0};
static OsdRect rc_eye_val    = {0,0,0,0};
static OsdRect rc_eye_plus   = {0,0,0,0};
static OsdRect rc_eye_step_label  = {0,0,0,0};
static OsdRect rc_eye_step_val    = {0,0,0,0};
static OsdRect rc_hotkeys_hdr= {0,0,0,0};
// Rebind rows
static OsdRect rc_bind_name[ACT_COUNT] = {
    {10, 136, 160, 18},
    {10, 158, 160, 18},
    {10, 180, 160, 18},
    {10, 202, 160, 18},
    {248, 112, 160, 18} // toggle shown near header
};
static OsdRect rc_bind_key[ACT_COUNT] = {
    {176, 136, 110, 18},
    {176, 158, 110, 18},
    {176, 180, 110, 18},
    {176, 202, 110, 18},
    {414, 112, 52, 18}
};
static OsdRect rc_bind_btn[ACT_COUNT] = {
    {292, 136, 64, 18},
    {292, 158, 64, 18},
    {292, 180, 64, 18},
    {292, 202, 64, 18},
    {414, 132, 52, 18}
};

// Mitigation toggles
static OsdRect rc_mit_hdr = {0,0,0,0};
static OsdRect rc_vs_label = {0,0,0,0};
static OsdRect rc_vs_toggle = {0,0,0,0};
static OsdRect rc_flip_label = {0,0,0,0};
static OsdRect rc_flip_toggle = {0,0,0,0};
static OsdRect rc_resync_btn = {0,0,0,0};
static OsdRect rc_ppinj_label = {0,0,0,0};
static OsdRect rc_ppinj_toggle = {0,0,0,0};
static OsdRect rc_notif_label = {0,0,0,0};
static OsdRect rc_notif_toggle = {0,0,0,0};
static OsdRect rc_bypass_label = {0,0,0,0};
static OsdRect rc_bypass_main_toggle = {0,0,0,0};
static OsdRect rc_bypass_osd_toggle  = {0,0,0,0};
static OsdRect rc_fps_label = {0,0,0,0};
static OsdRect rc_fps_toggle = {0,0,0,0};
static OsdRect rc_vsyncpar_label = {0,0,0,0};
static OsdRect rc_vsyncpar_toggle = {0,0,0,0};
static OsdRect rc_ppi_label = {0,0,0,0};
static OsdRect rc_ppi_toggle = {0,0,0,0};
static OsdRect rc_fullscreen_label = {0,0,0,0};
static OsdRect rc_fullscreen_toggle = {0,0,0,0};
static OsdRect rc_finish_label = {0,0,0,0};
static OsdRect rc_finish_toggle = {0,0,0,0};
static OsdRect rc_soc_label = {0,0,0,0};
static OsdRect rc_soc_toggle = {0,0,0,0};
static OsdRect rc_usbthread_label = {0,0,0,0};
static OsdRect rc_usbthread_toggle = {0,0,0,0};
static OsdRect rc_usbstatus_label = {0,0,0,0};
static OsdRect rc_usbstatus_val = {0,0,0,0};

static void osd_compute_layout(void) {
    const int margin = 10;
    const int title_h = 24;
    const int row_h = 26;
    const int btn_w = 32;
    const int step_label_w = 44;
    const int step_val_w = 110;
    const int label_w = 160;
    const int val_w = 170;
    int x = margin;
    int y = margin;

    // Title bar and top-right controls
    rc_title.x = x; rc_title.y = y; rc_title.w = osd_width - (margin*3) - 28 - 80; rc_title.h = title_h;
    rc_close.w = 28; rc_close.h = 18; rc_close.x = osd_width - margin - rc_close.w; rc_close.y = y;
    rc_resync_btn.w = 80; rc_resync_btn.h = 18; rc_resync_btn.x = rc_close.x - margin - rc_resync_btn.w; rc_resync_btn.y = y;
    y += title_h + margin;

    // Convergence row
    rc_conv_label = (OsdRect){x, y, label_w, row_h};
    rc_conv_minus = (OsdRect){x + label_w + margin, y-2, btn_w, row_h};
    rc_conv_val   = (OsdRect){rc_conv_minus.x + btn_w + margin, y-2, val_w, row_h};
    rc_conv_plus  = (OsdRect){rc_conv_val.x + val_w + margin, y-2, btn_w, row_h};
    rc_conv_step_label = (OsdRect){rc_conv_plus.x + btn_w + margin, y, step_label_w, row_h};
    rc_conv_step_val   = (OsdRect){rc_conv_step_label.x + step_label_w + margin, y-2, step_val_w, row_h};
    y += row_h + margin;

    // Eye row
    rc_eye_label = (OsdRect){x, y, label_w, row_h};
    rc_eye_minus = (OsdRect){x + label_w + margin, y-2, btn_w, row_h};
    rc_eye_val   = (OsdRect){rc_eye_minus.x + btn_w + margin, y-2, val_w, row_h};
    rc_eye_plus  = (OsdRect){rc_eye_val.x + val_w + margin, y-2, btn_w, row_h};
    rc_eye_step_label = (OsdRect){rc_eye_plus.x + btn_w + margin, y, step_label_w, row_h};
    rc_eye_step_val   = (OsdRect){rc_eye_step_label.x + step_label_w + margin, y-2, step_val_w, row_h};
    y += row_h + margin;

    // Toggles block (one per row for clarity)
    int toggle_label_w = 260; int toggle_w = 70;
    rc_vs_label  = (OsdRect){x, y, toggle_label_w, row_h}; rc_vs_toggle  = (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_flip_label= (OsdRect){x, y, toggle_label_w, row_h}; rc_flip_toggle= (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_ppinj_label= (OsdRect){x, y, toggle_label_w, row_h}; rc_ppinj_toggle= (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_notif_label= (OsdRect){x, y, toggle_label_w, row_h}; rc_notif_toggle= (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_bypass_label= (OsdRect){x, y, toggle_label_w, row_h};
    rc_bypass_main_toggle = (OsdRect){x + toggle_label_w + margin, y-2, 60, row_h};
    rc_bypass_osd_toggle  = (OsdRect){rc_bypass_main_toggle.x + 60 + margin, y-2, 60, row_h};
    y += row_h + margin;
    rc_fps_label = (OsdRect){x, y, toggle_label_w, row_h}; rc_fps_toggle= (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_vsyncpar_label = (OsdRect){x, y, toggle_label_w, row_h}; rc_vsyncpar_toggle = (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_ppi_label = (OsdRect){x, y, toggle_label_w, row_h}; rc_ppi_toggle = (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_fullscreen_label = (OsdRect){x, y, toggle_label_w, row_h}; rc_fullscreen_toggle = (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_soc_label = (OsdRect){x, y, toggle_label_w, row_h}; rc_soc_toggle = (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_finish_label = (OsdRect){x, y, toggle_label_w, row_h}; rc_finish_toggle = (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h}; y += row_h + margin;
    rc_usbthread_label = (OsdRect){x, y, toggle_label_w, row_h};
    rc_usbthread_toggle = (OsdRect){x + toggle_label_w + margin, y-2, toggle_w, row_h};
    // USB status fields to the right of the toggle
    rc_usbstatus_label = (OsdRect){rc_usbthread_toggle.x + toggle_w + margin, y, 60, row_h};
    rc_usbstatus_val   = (OsdRect){rc_usbstatus_label.x + 60 + margin, y-2, 100, row_h};
    y += row_h + margin;
    // Parity invert under it
    // We'll reuse rc_vsyncpar_toggle sizing for parity invert toggle placed right of it

    // Hotkeys header
    rc_hotkeys_hdr = (OsdRect){x, y, 260, row_h};
    y += row_h + margin/2;

    // Hotkey rows (ACT_COUNT)
    for (int i = 0; i < ACT_COUNT; i++) {
        rc_bind_name[i] = (OsdRect){x, y, 240, row_h};
        rc_bind_key[i]  = (OsdRect){x + 250, y-2, 200, row_h};
        rc_bind_btn[i]  = (OsdRect){x + 460, y-2, 100, row_h};
        y += row_h + margin/2;
    }

    // Adjust total height
    osd_height = y + margin;
}

static void osd_init(Display *dpy) {
    if (osd_initialized) return;
    if (!dpy) return;
    osd_dpy = dpy;
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = BlackPixel(dpy, screen);
    osd_compute_layout();
    osd_win = XCreateWindow(dpy, root,
                            10, 10, (unsigned)osd_width, (unsigned)osd_height, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel, &swa);
    XStoreName(dpy, osd_win, "stereo-osd");
    if (bypass_compositor_osd) {
        Atom bypass = XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
        unsigned long value = 1; // 1 = bypass
        XChangeProperty(dpy, osd_win, bypass, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&value, 1);
    }
    XSelectInput(dpy, osd_win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | KeyPressMask | KeyReleaseMask |
                 FocusChangeMask | StructureNotifyMask);
    XMapWindow(dpy, osd_win);
    XRaiseWindow(dpy, osd_win);
    XFlush(dpy);
    XUnmapWindow(dpy, osd_win);
    XFlush(dpy);
    XGCValues gcv;
    gcv.foreground = WhitePixel(dpy, screen);
    gcv.background = BlackPixel(dpy, screen);
    osd_gc = XCreateGC(dpy, osd_win, GCForeground | GCBackground, &gcv);
    osd_initialized = 1;
}

static void osd_showf(Display *dpy, const char *fmt, ...) {
    if (!osd_initialized) osd_init(dpy);
    if (!osd_initialized) return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (osd_notifications_enabled) {
        // Notification banner dimensions
        osd_state = OSD_STATE_NOTIFY;
        XResizeWindow(osd_dpy, osd_win, 360u, 28u);
    XMapWindow(osd_dpy, osd_win);
    XRaiseWindow(osd_dpy, osd_win);
    XSetForeground(osd_dpy, osd_gc, BlackPixel(osd_dpy, DefaultScreen(osd_dpy)));
    XFillRectangle(osd_dpy, osd_win, osd_gc, 0, 0, 360, 28);
    XSetForeground(osd_dpy, osd_gc, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)));
    XDrawString(osd_dpy, osd_win, osd_gc, 8, 18, buf, (int)strlen(buf));
    XFlush(osd_dpy);
    }

    if (osd_notifications_enabled) {
    // visible for 1200 ms
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    osd_hide_time.tv_sec = now.tv_sec + 1;
    long nsec = now.tv_nsec + 200000000L;
    if (nsec >= 1000000000L) { osd_hide_time.tv_sec += 1; nsec -= 1000000000L; }
    osd_hide_time.tv_nsec = nsec;
    }
}
// --------- Compositor bypass helper for main window ---------
static Window find_toplevel_window(Display *dpy, Window w) {
    if (!dpy || !w) return 0;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return 0;
    Window root = 0, parent = 0;
    Window *children = NULL;
    unsigned int nchildren = 0;
    Window cur = w;
    // Ascend parents until parent is root
    for (int i = 0; i < 16; i++) {
        if (!XQueryTree(dpy, cur, &root, &parent, &children, &nchildren)) break;
        if (children) XFree(children);
        if (parent == 0 || parent == root) {
            return cur;
        }
        cur = parent;
    }
    return w;
}

static void set_bypass_compositor_for_window(Display *dpy, Window w, int enable) {
    if (!dpy || !w) return;
    Atom bypass = XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
    unsigned long value = enable ? 1UL : 0UL;
    XChangeProperty(dpy, w, bypass, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&value, 1);
}

static void apply_bypass_main_if_needed(Display *dpy, GLXDrawable drawable) {
    if (!dpy || !drawable) return;
    if (!bypass_compositor_main) { return; }
    if (!bypass_main_needs_apply) return;
    // Treat drawable as a Window and attempt to ascend to toplevel
    Window w = (Window)drawable;
    Window top = find_toplevel_window(dpy, w);
    if (top) {
        set_bypass_compositor_for_window(dpy, top, 1);
        set_bypass_compositor_for_window(dpy, w, 1);
        XFlush(dpy);
        bypass_main_applied = 1;
        bypass_main_needs_apply = 0;
    }
}

static void apply_fullscreen_if_needed(Display *dpy, GLXDrawable drawable) {
    if (!dpy || !drawable) return;
    Window w = (Window)drawable;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) return;
    int screen = DefaultScreen(dpy);
    int screen_w = DisplayWidth(dpy, screen);
    int screen_h = DisplayHeight(dpy, screen);

    if (fullscreen_enabled) {
        if (fullscreen_applied && !fullscreen_needs_apply) return;
        // Save current geometry once
        if (!saved_window_geom_valid) {
            saved_win_x = attr.x;
            saved_win_y = attr.y;
            saved_win_w = (unsigned int)attr.width;
            saved_win_h = (unsigned int)attr.height;
            saved_window_geom_valid = 1;
        }
        // Request EWMH fullscreen first
        Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        XEvent xev;
        memset(&xev, 0, sizeof(xev));
        xev.xclient.type = ClientMessage;
        xev.xclient.window = w;
        xev.xclient.message_type = wm_state;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
        xev.xclient.data.l[1] = fs;
        xev.xclient.data.l[2] = 0;
        xev.xclient.data.l[3] = 1; // normal source
        XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

        // Fallback: override-redirect + resize
        XSetWindowAttributes swa;
        swa.override_redirect = True;
        XChangeWindowAttributes(dpy, w, CWOverrideRedirect, &swa);
        XMoveResizeWindow(dpy, w, 0, 0, (unsigned int)screen_w, (unsigned int)screen_h);
        XRaiseWindow(dpy, w);
        XMapWindow(dpy, w);
        XFlush(dpy);
        fullscreen_applied = 1;
        fullscreen_needs_apply = 0;
    } else {
        if (!fullscreen_applied) return;
        // Remove EWMH fullscreen
        Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        XEvent xev;
        memset(&xev, 0, sizeof(xev));
        xev.xclient.type = ClientMessage;
        xev.xclient.window = w;
        xev.xclient.message_type = wm_state;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = 0; // _NET_WM_STATE_REMOVE
        xev.xclient.data.l[1] = fs;
        xev.xclient.data.l[2] = 0;
        xev.xclient.data.l[3] = 1;
        XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

        // Clear override-redirect
        XSetWindowAttributes swa;
        swa.override_redirect = False;
        XChangeWindowAttributes(dpy, w, CWOverrideRedirect, &swa);
        // Restore geometry if we have it
        if (saved_window_geom_valid) {
            XMoveResizeWindow(dpy, w, saved_win_x, saved_win_y, saved_win_w, saved_win_h);
        }
        XRaiseWindow(dpy, w);
        XMapWindow(dpy, w);
        XFlush(dpy);
        fullscreen_applied = 0;
        fullscreen_needs_apply = 0;
    }
}

static void osd_tick(Display *dpy) {
    (void)dpy;
    if (!osd_initialized) return;
    if (osd_state != OSD_STATE_NOTIFY) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > osd_hide_time.tv_sec ||
        (now.tv_sec == osd_hide_time.tv_sec && now.tv_nsec >= osd_hide_time.tv_nsec)) {
        XUnmapWindow(osd_dpy, osd_win);
        XFlush(osd_dpy);
        osd_state = OSD_STATE_HIDDEN;
    }
}

// ---------- OSD menu helpers ----------
static const char* action_name(OsdAction a) {
    switch (a) {
        case ACT_CONV_INC: return "Convergence +";
        case ACT_CONV_DEC: return "Convergence -";
        case ACT_EYE_INC:  return "Eye offset +";
        case ACT_EYE_DEC:  return "Eye offset -";
        case ACT_OSD_TOGGLE: return "Toggle OSD";
        default: return "";
    }
}

static const char* keysym_to_string(KeySym ks) {
    const char *s = XKeysymToString(ks);
    return s ? s : "(none)";
}

static void init_default_bindings(Display *dpy) {
    if (!dpy) return;
    action_keysym[ACT_CONV_INC] = XK_F7;
    action_keysym[ACT_CONV_DEC] = XK_F8;
    action_keysym[ACT_EYE_DEC]  = XK_F9; // keep legacy mapping
    action_keysym[ACT_EYE_INC]  = XK_F10;
    action_keysym[ACT_OSD_TOGGLE] = XK_F6;
    for (int i = 0; i < ACT_COUNT; i++) {
        action_keycode[i] = XKeysymToKeycode(dpy, action_keysym[i]);
    }
}

static void osd_draw_box(OsdRect r, unsigned long fg, unsigned long bg, const char *label) {
    XSetForeground(osd_dpy, osd_gc, bg);
    XFillRectangle(osd_dpy, osd_win, osd_gc, r.x, r.y, (unsigned)r.w, (unsigned)r.h);
    XSetForeground(osd_dpy, osd_gc, fg);
    XDrawRectangle(osd_dpy, osd_win, osd_gc, r.x, r.y, (unsigned)r.w, (unsigned)r.h);
    if (label && *label) {
        int tx = r.x + 6;
        int ty = r.y + (r.h/2) + 5;
        XDrawString(osd_dpy, osd_win, osd_gc, tx, ty, label, (int)strlen(label));
    }
}

static void osd_draw_menu(void) {
    if (!osd_initialized) return;
    // Ensure layout and size are up-to-date each draw
    osd_compute_layout();
    XResizeWindow(osd_dpy, osd_win, (unsigned)osd_width, (unsigned)osd_height);
    XMoveWindow(osd_dpy, osd_win, 10, 10);
    // Window background
    XSetForeground(osd_dpy, osd_gc, BlackPixel(osd_dpy, DefaultScreen(osd_dpy)));
    XFillRectangle(osd_dpy, osd_win, osd_gc, 0, 0, (unsigned)osd_width, (unsigned)osd_height);
    XSetForeground(osd_dpy, osd_gc, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)));
    // Draw window border for clarity
    XDrawRectangle(osd_dpy, osd_win, osd_gc, 0, 0, (unsigned)osd_width-1, (unsigned)osd_height-1);

    // Title and close
    XDrawString(osd_dpy, osd_win, osd_gc, rc_title.x, rc_title.y + 12, "Stereo OSD", 10);
    osd_draw_box(rc_close, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "X");

    // Convergence row
    XDrawString(osd_dpy, osd_win, osd_gc, rc_conv_label.x, rc_conv_label.y + 12, "Convergence", 12);
    osd_draw_box(rc_conv_minus, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "-");
    char buf[64];
    if (osd_editing == EDIT_CONVERGENCE) {
        snprintf(buf, sizeof(buf), "%s", osd_edit_buf);
    } else {
        snprintf(buf, sizeof(buf), "%.2f", CONVERGENCE);
    }
    osd_draw_box(rc_conv_val, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, buf);
    osd_draw_box(rc_conv_plus, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "+");
    osd_draw_box(rc_conv_step_label, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "Step");
    snprintf(buf, sizeof(buf), "%.2f", STEP_CONV);
    osd_draw_box(rc_conv_step_val, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, buf);

    // Eye offset row
    XDrawString(osd_dpy, osd_win, osd_gc, rc_eye_label.x, rc_eye_label.y + 12, "Eye offset", 10);
    osd_draw_box(rc_eye_minus, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "-");
    if (osd_editing == EDIT_EYEOFFSET) {
        snprintf(buf, sizeof(buf), "%s", osd_edit_buf);
    } else {
        snprintf(buf, sizeof(buf), "%.3f", EYE_OFFSET);
    }
    osd_draw_box(rc_eye_val, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, buf);
    osd_draw_box(rc_eye_plus, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "+");
    osd_draw_box(rc_eye_step_label, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "Step");
    snprintf(buf, sizeof(buf), "%.3f", STEP_EYE);
    osd_draw_box(rc_eye_step_val, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, buf);

    // Mitigation controls
    XDrawString(osd_dpy, osd_win, osd_gc, rc_vs_label.x, rc_vs_label.y + 12, "VSync (SwapInterval)", 20);
    osd_draw_box(rc_vs_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, force_swap_interval ? "On" : "Off");
    XDrawString(osd_dpy, osd_win, osd_gc, rc_flip_label.x, rc_flip_label.y + 12, "Flip Eyes", 9);
    osd_draw_box(rc_flip_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, swap_eyes ? "On" : "Off");
    osd_draw_box(rc_resync_btn, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "Resync");
    XDrawString(osd_dpy, osd_win, osd_gc, rc_ppinj_label.x, rc_ppinj_label.y + 12, "Inject in glClear", 16);
    osd_draw_box(rc_ppinj_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, inject_in_clear_enabled ? "On" : "Off");
    XDrawString(osd_dpy, osd_win, osd_gc, rc_notif_label.x, rc_notif_label.y + 12, "OSD Notifications", 17);
    osd_draw_box(rc_notif_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, osd_notifications_enabled ? "On" : "Off");
    XDrawString(osd_dpy, osd_win, osd_gc, rc_bypass_label.x, rc_bypass_label.y + 12, "Bypass Compositor (main/OSD)", 28);
    osd_draw_box(rc_bypass_main_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, bypass_compositor_main ? "On" : "Off");
    osd_draw_box(rc_bypass_osd_toggle,  WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, bypass_compositor_osd  ? "On" : "Off");
    XDrawString(osd_dpy, osd_win, osd_gc, rc_fps_label.x, rc_fps_label.y + 12, "FPS OSD", 7);
    osd_draw_box(rc_fps_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, fps_enabled ? "On" : "Off");
    XDrawString(osd_dpy, osd_win, osd_gc, rc_vsyncpar_label.x, rc_vsyncpar_label.y + 12, "Lock to VBlank Parity (SGI)", 26);
    osd_draw_box(rc_vsyncpar_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, video_sync_parity_enabled ? "On" : "Off");
    // Parity invert moved to its own row (reusing rc_ppi position)
    XDrawString(osd_dpy, osd_win, osd_gc, rc_ppi_label.x, rc_ppi_label.y + 12, "Invert Parity", 13);
    osd_draw_box(rc_ppi_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, parity_invert ? "On" : "Off");
    // Fullscreen toggle
    XDrawString(osd_dpy, osd_win, osd_gc, rc_fullscreen_label.x, rc_fullscreen_label.y + 12, "Fullscreen (override redirect)", 31);
    osd_draw_box(rc_fullscreen_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, fullscreen_enabled ? "On" : "Off");
    // Swap-on-clear row (dedicated)
    XDrawString(osd_dpy, osd_win, osd_gc, rc_soc_label.x, rc_soc_label.y + 12, "Swap on Clear (advanced)", 24);
    osd_draw_box(rc_soc_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, swap_on_clear_enabled ? "On" : "Off");
    XDrawString(osd_dpy, osd_win, osd_gc, rc_finish_label.x, rc_finish_label.y + 12, "glFinish before swap", 20);
    osd_draw_box(rc_finish_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, (finish_before_swap ? "On" : "Off"));
    XDrawString(osd_dpy, osd_win, osd_gc, rc_usbthread_label.x, rc_usbthread_label.y + 12, "USB thread drive (120Hz)", 24);
    osd_draw_box(rc_usbthread_toggle, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, (usb_thread_mode ? "On" : "Off"));
    if (usb_thread_mode) {
        XDrawString(osd_dpy, osd_win, osd_gc, rc_usbthread_label.x + 200, rc_usbthread_label.y + 12, "(decouples swap; may freeze app animation)", 44);
    }
    // USB thread status indicator
    XDrawString(osd_dpy, osd_win, osd_gc, rc_usbstatus_label.x, rc_usbstatus_label.y + 12, "Thread", 6);
    osd_draw_box(rc_usbstatus_val, WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, (stereo_thread_started ? "Running" : "Stopped"));
    // Drag stabilization hint
    char stabLabel[64];
    snprintf(stabLabel, sizeof(stabLabel), "Drag stabilize %s (%dms)", stabilize_moves_enabled?"On":"Off", stabilize_window_ms);
    XDrawString(osd_dpy, osd_win, osd_gc, rc_soc_label.x + 320, rc_soc_label.y + 12, stabLabel, (int)strlen(stabLabel));

    // Hotkeys header
    XDrawString(osd_dpy, osd_win, osd_gc, rc_hotkeys_hdr.x, rc_hotkeys_hdr.y + 12, "Hotkeys", 7);

    // Bind rows for four actions
    for (int i = 0; i < 4; i++) {
        osd_draw_box(rc_bind_name[i], WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, action_name((OsdAction)i));
        const char *kn = keysym_to_string(action_keysym[i]);
        osd_draw_box(rc_bind_key[i], WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, kn);
        osd_draw_box(rc_bind_btn[i], WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "Rebind");
    }
    // Toggle row (compact near header)
    osd_draw_box(rc_bind_name[ACT_OSD_TOGGLE], WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, action_name(ACT_OSD_TOGGLE));
    osd_draw_box(rc_bind_key[ACT_OSD_TOGGLE], WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, keysym_to_string(action_keysym[ACT_OSD_TOGGLE]));
    osd_draw_box(rc_bind_btn[ACT_OSD_TOGGLE], WhitePixel(osd_dpy, DefaultScreen(osd_dpy)), 0, "Rebind");

    if (osd_rebind_waiting >= 0) {
        const char *msg = "Press a key to bind... (Esc to cancel)";
        XDrawString(osd_dpy, osd_win, osd_gc, 10, 16, msg, (int)strlen(msg));
    } else if (osd_editing != EDIT_NONE) {
        const char *msg = "Type value, Enter to apply, Esc to cancel";
        XDrawString(osd_dpy, osd_win, osd_gc, 10, 16, msg, (int)strlen(msg));
    }
    XFlush(osd_dpy);
}

static void osd_open_menu(void) {
    if (!osd_initialized) return;
    osd_state = OSD_STATE_MENU;
    osd_compute_layout();
    XResizeWindow(osd_dpy, osd_win, (unsigned)osd_width, (unsigned)osd_height);
    XMoveWindow(osd_dpy, osd_win, 10, 10);
    XMapWindow(osd_dpy, osd_win);
    XRaiseWindow(osd_dpy, osd_win);
    XSetInputFocus(osd_dpy, osd_win, RevertToParent, CurrentTime);
    osd_draw_menu();
}

static void osd_close_menu(void) {
    if (!osd_initialized) return;
    osd_state = OSD_STATE_HIDDEN;
    osd_editing = EDIT_NONE;
    osd_rebind_waiting = -1;
    XUnmapWindow(osd_dpy, osd_win);
    XFlush(osd_dpy);
}

static void osd_toggle_menu(void) {
    if (osd_state == OSD_STATE_MENU) osd_close_menu(); else osd_open_menu();
}

static int point_in_rect(int x, int y, OsdRect r) {
    return (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
}

static void osd_commit_edit(void) {
    char *endp = NULL;
    float v = strtof(osd_edit_buf, &endp);
    if (endp != osd_edit_buf) {
        if (osd_editing == EDIT_CONVERGENCE) {
            CONVERGENCE = v;
        } else if (osd_editing == EDIT_EYEOFFSET) {
            EYE_OFFSET = v;
        } else if (osd_editing == EDIT_CONV_STEP) {
            STEP_CONV = v;
        } else if (osd_editing == EDIT_EYE_STEP) {
            STEP_EYE = v;
        }
    }
    osd_editing = EDIT_NONE;
}

// ---------- FPS window ----------
static void fps_init(Display *dpy) {
    if (fps_initialized || !dpy) return;
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = BlackPixel(dpy, screen);
    fps_win = XCreateWindow(dpy, root, 10, 40, 120, 22, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel, &swa);
    XStoreName(dpy, fps_win, "stereo-fps");
    if (bypass_compositor_osd) {
        Atom bypass = XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
        unsigned long value = 1;
        XChangeProperty(dpy, fps_win, bypass, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&value, 1);
    }
    XMapWindow(dpy, fps_win);
    XRaiseWindow(dpy, fps_win);
    XGCValues gcv;
    gcv.foreground = WhitePixel(dpy, screen);
    gcv.background = BlackPixel(dpy, screen);
    fps_gc = XCreateGC(dpy, fps_win, GCForeground | GCBackground, &gcv);
    clock_gettime(CLOCK_MONOTONIC, &fps_prev_time);
    fps_initialized = 1;
}

static void fps_hide(Display *dpy) {
    if (!fps_initialized || !dpy) return;
    XUnmapWindow(dpy, fps_win);
}

static void fps_deinit(Display *dpy) {
    if (!fps_initialized || !dpy) return;
    XDestroyWindow(dpy, fps_win);
    XFreeGC(dpy, fps_gc);
    fps_win = 0;
    fps_gc = 0;
    fps_initialized = 0;
}

static void fps_update_and_draw(Display *dpy) {
    if (!fps_enabled) { if (fps_initialized) fps_hide(dpy); return; }
    if (!fps_initialized) fps_init(dpy);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (now.tv_sec - fps_prev_time.tv_sec) + (now.tv_nsec - fps_prev_time.tv_nsec) / 1e9;
    if (dt <= 0.0) dt = 1e-6;
    double inst_fps = 1.0 / dt;
    if (fps_value <= 0.0) fps_value = inst_fps;
    else fps_value = fps_alpha * inst_fps + (1.0 - fps_alpha) * fps_value;
    fps_prev_time = now;

    char text[64];
    snprintf(text, sizeof(text), "FPS: %.1f", fps_value);
    XSetForeground(dpy, fps_gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, fps_win, fps_gc, 0, 0, 120, 22);
    XSetForeground(dpy, fps_gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XDrawString(dpy, fps_win, fps_gc, 6, 15, text, (int)strlen(text));
    XFlush(dpy);
}

// -------------- Simple config persistence --------------
static void get_config_dir(char *out, size_t out_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(out, out_sz, "%s/%s", xdg, "gl-frame-seq-stereo");
    } else if (home && *home) {
        snprintf(out, out_sz, "%s/.config/%s", home, "gl-frame-seq-stereo");
    } else {
        snprintf(out, out_sz, "%s", ".");
    }
}

static void get_config_path(char *out, size_t out_sz) {
    char dir[PATH_MAX];
    get_config_dir(dir, sizeof(dir));
    snprintf(out, out_sz, "%s/config.ini", dir);
}

static void ensure_config_dir_exists(void) {
    char dir[PATH_MAX];
    get_config_dir(dir, sizeof(dir));
    // Try to create parent ~/.config when XDG not set; ignore if exists
    // Create the app dir
    mkdir(dir, 0700);
}

static void config_save(void) {
    ensure_config_dir_exists();
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    get_config_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "CONVERGENCE=%.6f\n", (double)CONVERGENCE);
    fprintf(f, "EYE_OFFSET=%.6f\n", (double)EYE_OFFSET);
    fprintf(f, "STEP_CONV=%.6f\n", (double)STEP_CONV);
    fprintf(f, "STEP_EYE=%.6f\n", (double)STEP_EYE);
    fprintf(f, "FORCE_SWAP_INTERVAL=%d\n", force_swap_interval);
    fprintf(f, "SWAP_EYES=%d\n", swap_eyes);
    fprintf(f, "INJECT_IN_CLEAR=%d\n", inject_in_clear_enabled);
    fprintf(f, "OSD_NOTIFICATIONS=%d\n", osd_notifications_enabled);
    fprintf(f, "BYPASS_COMPOSITOR_MAIN=%d\n", bypass_compositor_main);
    fprintf(f, "BYPASS_COMPOSITOR_OSD=%d\n", bypass_compositor_osd);
    fprintf(f, "FPS_ENABLED=%d\n", fps_enabled);
    fprintf(f, "SWAP_ON_CLEAR=%d\n", swap_on_clear_enabled);
    fprintf(f, "VIDEO_SYNC_PARITY=%d\n", video_sync_parity_enabled);
    fprintf(f, "PARITY_INVERT=%d\n", parity_invert);
    fprintf(f, "INJECT_AT_BEGIN=%d\n", inject_at_begin);
    fprintf(f, "FINISH_BEFORE_SWAP=%d\n", finish_before_swap);
    fprintf(f, "USB_THREAD_MODE=%d\n", usb_thread_mode);
    fprintf(f, "FULLSCREEN_ENABLED=%d\n", fullscreen_enabled);
    const char *k0 = XKeysymToString(action_keysym[ACT_CONV_INC]);
    const char *k1 = XKeysymToString(action_keysym[ACT_CONV_DEC]);
    const char *k2 = XKeysymToString(action_keysym[ACT_EYE_INC]);
    const char *k3 = XKeysymToString(action_keysym[ACT_EYE_DEC]);
    const char *k4 = XKeysymToString(action_keysym[ACT_OSD_TOGGLE]);
    fprintf(f, "ACTION_CONV_INC=%s\n", k0 ? k0 : "");
    fprintf(f, "ACTION_CONV_DEC=%s\n", k1 ? k1 : "");
    fprintf(f, "ACTION_EYE_INC=%s\n", k2 ? k2 : "");
    fprintf(f, "ACTION_EYE_DEC=%s\n", k3 ? k3 : "");
    fprintf(f, "ACTION_OSD_TOGGLE=%s\n", k4 ? k4 : "");
    fclose(f);
    rename(tmp, path);
}

static void config_load(void) {
    char path[PATH_MAX];
    get_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // strip newline
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "CONVERGENCE") == 0) {
            CONVERGENCE = strtof(val, NULL);
        } else if (strcmp(key, "EYE_OFFSET") == 0) {
            EYE_OFFSET = strtof(val, NULL);
        } else if (strcmp(key, "STEP_CONV") == 0) {
            STEP_CONV = strtof(val, NULL);
        } else if (strcmp(key, "STEP_EYE") == 0) {
            STEP_EYE = strtof(val, NULL);
        } else if (strcmp(key, "FORCE_SWAP_INTERVAL") == 0) {
            force_swap_interval = atoi(val);
        } else if (strcmp(key, "SWAP_EYES") == 0) {
            swap_eyes = atoi(val);
        } else if (strcmp(key, "INJECT_IN_CLEAR") == 0) {
            inject_in_clear_enabled = atoi(val);
        } else if (strcmp(key, "OSD_NOTIFICATIONS") == 0) {
            osd_notifications_enabled = atoi(val);
        } else if (strcmp(key, "BYPASS_COMPOSITOR_MAIN") == 0) {
            bypass_compositor_main = atoi(val);
        } else if (strcmp(key, "BYPASS_COMPOSITOR_OSD") == 0) {
            bypass_compositor_osd = atoi(val);
        } else if (strcmp(key, "FPS_ENABLED") == 0) {
            fps_enabled = atoi(val);
        } else if (strcmp(key, "SWAP_ON_CLEAR") == 0) {
            swap_on_clear_enabled = atoi(val);
        } else if (strcmp(key, "VIDEO_SYNC_PARITY") == 0) {
            video_sync_parity_enabled = atoi(val);
        } else if (strcmp(key, "PARITY_INVERT") == 0) {
            parity_invert = atoi(val);
        } else if (strcmp(key, "INJECT_AT_BEGIN") == 0) {
            inject_at_begin = atoi(val);
        } else if (strcmp(key, "FINISH_BEFORE_SWAP") == 0) {
            finish_before_swap = atoi(val);
        } else if (strcmp(key, "USB_THREAD_MODE") == 0) {
            usb_thread_mode = atoi(val);
        } else if (strcmp(key, "FULLSCREEN_ENABLED") == 0) {
            fullscreen_enabled = atoi(val);
        } else if (strcmp(key, "ACTION_CONV_INC") == 0) {
            action_keysym[ACT_CONV_INC] = XStringToKeysym(val);
        } else if (strcmp(key, "ACTION_CONV_DEC") == 0) {
            action_keysym[ACT_CONV_DEC] = XStringToKeysym(val);
        } else if (strcmp(key, "ACTION_EYE_INC") == 0) {
            action_keysym[ACT_EYE_INC] = XStringToKeysym(val);
        } else if (strcmp(key, "ACTION_EYE_DEC") == 0) {
            action_keysym[ACT_EYE_DEC] = XStringToKeysym(val);
        } else if (strcmp(key, "ACTION_OSD_TOGGLE") == 0) {
            action_keysym[ACT_OSD_TOGGLE] = XStringToKeysym(val);
        }
    }
    fclose(f);
}

static void update_action_keycodes(Display *dpy) {
    if (!dpy) return;
    for (int i = 0; i < ACT_COUNT; i++) {
        action_keycode[i] = XKeysymToKeycode(dpy, action_keysym[i]);
    }
}

static void osd_poll_events(void) {
    if (!osd_initialized || osd_state != OSD_STATE_MENU) return;
    while (XPending(osd_dpy)) {
        XEvent ev;
        XNextEvent(osd_dpy, &ev);
        switch (ev.type) {
            case Expose:
                osd_draw_menu();
                break;
            case ButtonPress: {
                int mx = ev.xbutton.x;
                int my = ev.xbutton.y;
                if (point_in_rect(mx, my, rc_close)) { osd_close_menu(); break; }
                if (point_in_rect(mx, my, rc_conv_minus)) { CONVERGENCE -= STEP_CONV; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_conv_plus))  { CONVERGENCE += STEP_CONV; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_eye_minus))  { EYE_OFFSET -= STEP_EYE; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_eye_plus))   { EYE_OFFSET += STEP_EYE; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_conv_val)) {
                    osd_editing = EDIT_CONVERGENCE;
                    snprintf(osd_edit_buf, sizeof(osd_edit_buf), "%.2f", CONVERGENCE);
                    osd_draw_menu();
                    break;
                }
                if (point_in_rect(mx, my, rc_eye_val)) {
                    osd_editing = EDIT_EYEOFFSET;
                    snprintf(osd_edit_buf, sizeof(osd_edit_buf), "%.3f", EYE_OFFSET);
                    osd_draw_menu();
                    break;
                }
                if (point_in_rect(mx, my, rc_conv_step_val)) {
                    osd_editing = EDIT_CONV_STEP;
                    snprintf(osd_edit_buf, sizeof(osd_edit_buf), "%.2f", STEP_CONV);
                    osd_draw_menu();
                    break;
                }
                if (point_in_rect(mx, my, rc_eye_step_val)) {
                    osd_editing = EDIT_EYE_STEP;
                    snprintf(osd_edit_buf, sizeof(osd_edit_buf), "%.3f", STEP_EYE);
                    osd_draw_menu();
                    break;
                }
                if (point_in_rect(mx, my, rc_vs_toggle)) { force_swap_interval = !force_swap_interval; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_flip_toggle)) { swap_eyes = !swap_eyes; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_resync_btn)) { resync_requested = 1; osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_ppinj_toggle)) { inject_in_clear_enabled = !inject_in_clear_enabled; if (inject_in_clear_enabled) inject_at_begin = 0; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_notif_toggle)) { osd_notifications_enabled = !osd_notifications_enabled; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_bypass_main_toggle)) { bypass_compositor_main = !bypass_compositor_main; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_bypass_osd_toggle))  { bypass_compositor_osd  = !bypass_compositor_osd;  config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_fps_toggle)) { fps_enabled = !fps_enabled; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_vsyncpar_toggle)) { video_sync_parity_enabled = !video_sync_parity_enabled; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_ppi_toggle)) { parity_invert = !parity_invert; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_fullscreen_toggle)) { fullscreen_enabled = !fullscreen_enabled; fullscreen_needs_apply = 1; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_soc_toggle)) { swap_on_clear_enabled = !swap_on_clear_enabled; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_finish_toggle)) { finish_before_swap = !finish_before_swap; config_save(); osd_draw_menu(); break; }
                if (point_in_rect(mx, my, rc_usbthread_toggle)) {
                    usb_thread_mode = !usb_thread_mode;
                    if (usb_thread_mode) {
                        emitter_start_usb_thread();
                        osd_showf(osd_dpy, "USB thread: Running");
                    } else {
                        emitter_stop_usb_thread();
                        // Force resync to LEFT on next frame when returning to swap-driven mode
                        resync_requested = 1;
                        current_camera = CAMERA_LEFT;
                        osd_showf(osd_dpy, "USB thread: Stopped (resync)");
                    }
                    config_save();
                    osd_draw_menu();
                    break;
                }
                for (int i = 0; i < ACT_COUNT; i++) {
                    if (point_in_rect(mx, my, rc_bind_btn[i])) {
                        osd_rebind_waiting = i;
                        osd_draw_menu();
                        break;
                    }
                }
                break;
            }
            case KeyPress: {
                XKeyEvent *ke = &ev.xkey;
                KeySym ks = XkbKeycodeToKeysym(osd_dpy, ke->keycode, 0, (ke->state & ShiftMask) ? 1 : 0);
                char str[8];
                KeySym tmp;
                XLookupString(ke, str, sizeof(str), &tmp, NULL);
                if (osd_rebind_waiting >= 0) {
                    if (ks == XK_Escape) { osd_rebind_waiting = -1; osd_draw_menu(); break; }
                    action_keysym[osd_rebind_waiting] = ks;
                    action_keycode[osd_rebind_waiting] = ke->keycode;
                    config_save();
                    osd_rebind_waiting = -1;
                    osd_draw_menu();
                    break;
                }
                if (osd_editing != EDIT_NONE) {
                    if (ks == XK_Return) { osd_commit_edit(); config_save(); osd_draw_menu(); break; }
                    if (ks == XK_Escape) { osd_editing = EDIT_NONE; osd_draw_menu(); break; }
                    if (ks == XK_BackSpace) {
                        size_t len = strlen(osd_edit_buf);
                        if (len > 0) osd_edit_buf[len-1] = '\0';
                        osd_draw_menu();
                        break;
                    }
                    if (isprint((unsigned char)str[0])) {
                        size_t len = strlen(osd_edit_buf);
                        if (len + 1 < sizeof(osd_edit_buf)) {
                            osd_edit_buf[len] = str[0];
                            osd_edit_buf[len+1] = '\0';
                            osd_draw_menu();
                        }
                    }
                    break;
                }
                if (ks == XK_Escape) { osd_close_menu(); break; }
                break;
            }
            default:
                break;
        }
    }
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
    // If this is glLoadMatrixf/d, return our hooked version instead
    if (strcmp((const char*)procName, "glLoadMatrixf") == 0) {
        return (__GLXextFuncPtr)glLoadMatrixf; // Return our hooked version
    }
    if (strcmp((const char*)procName, "glLoadMatrixd") == 0) {
        return (__GLXextFuncPtr)glLoadMatrixd; // Return our hooked version
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
    // If this is glXMakeCurrent, return our hooked version instead
    if (strcmp((const char*)procName, "glXMakeCurrent") == 0) {
        return (__GLXextFuncPtr)glXMakeCurrent; // Return our hooked version
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
    // Save projection parameters and type
    last_projection_type = PROJECTION_ORTHO;
    last_frustum_left = left;
    last_frustum_right = right;
    last_frustum_bottom = bottom;
    last_frustum_top = top;
    last_near = zNear;
    last_far = zFar;
    // Use app's projection directly; we will override per-eye at frame start
    real_glOrtho(left, right, bottom, top, zNear, zFar);
    saveModelMatrixAndTriggerReApply();
}

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar) {
    // Save projection parameters and type
    last_projection_type = PROJECTION_PERSPECTIVE;
    last_fov = fovy;
    last_aspect = aspect;
    last_near = zNear;
    last_far = zFar;
    // Use app's projection directly; we will override per-eye at frame start
    real_gluPerspective(fovy, aspect, zNear, zFar);
    saveModelMatrixAndTriggerReApply();
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
    GLdouble zNear, GLdouble zFar) {
  
    // Debug
    if (stereo_debug_enabled())
        fprintf(stderr, "[stereo] glFrustum(l=%.3f, r=%.3f, b=%.3f, t=%.3f, n=%.3f, f=%.3f)\n",
                left, right, bottom, top, zNear, zFar);

    // Save projection parameters and type
    last_projection_type = PROJECTION_FRUSTUM;
    last_frustum_left = left;
    last_frustum_right = right;
    last_frustum_bottom = bottom;
    last_frustum_top = top;
    last_near = zNear;
    last_far = zFar;
    // Use app's projection directly; we will override per-eye at frame start
    real_glFrustum(left, right, bottom, top, zNear, zFar);

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

    #if 0    
    GLint matrix_mode;
    glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
    real_glMatrixMode(GL_MODELVIEW);
    // Save the current modelview matrix
    glGetFloatv(GL_MODELVIEW_MATRIX, savedModelViewMatrix);
    hasSavedMatrix = 1;
    #endif

    // Compute eye-specific offset and convergence rotation
    float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
    if (CONVERGENCE > 0.001f) {
        float convergence_angle_rad = atanf(eye_offset / CONVERGENCE);
        float convergence_angle_deg = convergence_angle_rad * (180.0f / (float)M_PI);
        glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
    }
    glTranslatef(eye_offset, 0.0f, 0.0f);
    
    //glMatrixMode(matrix_mode);
    
    appliedEyeTransformThisFrame = 1;
    
}

// Optional per-primitive injection and debug tinting
void glBegin(GLenum mode) {
    if (!real_glBegin) real_glBegin = dlsym(RTLD_NEXT, "glBegin");
    if (!real_glBegin) return;

    if (inject_at_begin && !appliedEyeTransformThisFrame) {
        // Fallback: nudge the modelview for the current eye before drawing
        float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
        glPushMatrix();
        if (CONVERGENCE > 0.001f) {
            float convergence_angle_rad = atanf(eye_offset / CONVERGENCE);
            float convergence_angle_deg = convergence_angle_rad * (180.0f / (float)M_PI);
            glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
        }
        glTranslatef(eye_offset, 0.0f, 0.0f);
    }
    if (tint_enabled) {
        // Apply a subtle tint to distinguish views when glasses are off
        if (current_camera == CAMERA_LEFT) glColor4f(1.0f, 0.9f, 0.9f, 1.0f);
        else glColor4f(0.9f, 0.9f, 1.0f, 1.0f);
    }
    real_glBegin(mode);
}

void glEnd(void) {
    if (!real_glEnd) real_glEnd = dlsym(RTLD_NEXT, "glEnd");
    if (!real_glEnd) return;
    real_glEnd();
    if (inject_at_begin && !appliedEyeTransformThisFrame) {
        glPopMatrix();
    }
}

// Interposed glCallList - ensure display lists also get the eye transform when using per-primitive mode
void glCallList(GLuint list) {
    if (!real_glCallList) real_glCallList = dlsym(RTLD_NEXT, "glCallList");
    if (!real_glCallList) return;
    if (inject_at_begin && !appliedEyeTransformThisFrame && !camera_transform_disabled) {
        float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
        glPushMatrix();
        if (CONVERGENCE > 0.001f) {
            float convergence_angle_rad = atanf(eye_offset / CONVERGENCE);
            float convergence_angle_deg = convergence_angle_rad * (180.0f / (float)M_PI);
            glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
        }
        glTranslatef(eye_offset, 0.0f, 0.0f);
        real_glCallList(list);
        glPopMatrix();
        return;
    }
    real_glCallList(list);
}

int clearCount = 0;
// Interposed glClear - apply proper stereo view transformation
void glClear(GLbitfield mask) {
    clearCount += 1;
    //This line is important atm, causes black window if not here.
    //If we call glClear(), apply on-top of the glMatrixMode(GL_MODELVIEW), glLoadIdentity() mechanism.
    //appliedEyeTransformThisFrame = 0;

    // Apply stereo view transformation at the start of each frame.
    // For apps using display lists (e.g., glxgears), glBegin/End hooks won't
    // fire per frame. So we push a MODELVIEW translation here and pop it
    // right before swapping in glXSwapBuffers.
    if (inject_in_clear_enabled && (mask & GL_COLOR_BUFFER_BIT) && !transformPushedThisFrame && !camera_transform_disabled) {
        // Optional timing capture (kept for future tuning)
        #ifdef TIMER_RES_NANO
        (void)clock_gettime(CLOCK_MONOTONIC, &cameraSetupStart);
        #else
        gettimeofday(&cameraSetupStart, NULL);
        #endif

        // Compute eye offset for current eye
        float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;

        // Save current modes
        GLint prev_mode;
        glGetIntegerv(GL_MATRIX_MODE, &prev_mode);

        // Optionally adjust projection per-eye (off-axis frustum for perspective/frustum)
        int didProjectionAdjust = 0;
        if (last_projection_type == PROJECTION_PERSPECTIVE || last_projection_type == PROJECTION_FRUSTUM || last_projection_type == PROJECTION_ORTHO) {
            real_glMatrixMode(GL_PROJECTION);
            real_glPushMatrix();
            glLoadIdentity();
            if (last_projection_type == PROJECTION_PERSPECTIVE) {
                double n = last_near;
                double f = last_far;
                double top = n * tan((last_fov * 0.5) * (M_PI / 180.0));
                double bottom = -top;
                double right = top * last_aspect;
                double left = -right;
                double shift = ((double)eye_offset * n) / (double)CONVERGENCE;
                real_glFrustum(left + shift, right + shift, bottom, top, n, f);
                didProjectionAdjust = 1;
            } else if (last_projection_type == PROJECTION_FRUSTUM) {
                double n = last_near;
                double f = last_far;
                double left = last_frustum_left;
                double right = last_frustum_right;
                double bottom = last_frustum_bottom;
                double top = last_frustum_top;
                double shift = ((double)eye_offset * n) / (double)CONVERGENCE;
                real_glFrustum(left + shift, right + shift, bottom, top, n, f);
                didProjectionAdjust = 1;
            } else if (last_projection_type == PROJECTION_ORTHO) {
                real_glOrtho(last_frustum_left + eye_offset, last_frustum_right + eye_offset,
                             last_frustum_bottom, last_frustum_top, last_near, last_far);
                didProjectionAdjust = 1;
            }
            if (didProjectionAdjust) projectionPushedThisFrame = 1;
        }

        // Apply modelview eye transform (rotation then translation)
        real_glMatrixMode(GL_MODELVIEW);
        real_glPushMatrix();
        if (!didProjectionAdjust && CONVERGENCE > 0.001f) {
            float convergence_angle_rad = atanf(eye_offset / CONVERGENCE);
            float convergence_angle_deg = convergence_angle_rad * (180.0f / (float)M_PI);
            glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
        }
        glTranslatef(eye_offset, 0.0f, 0.0f);

        // Restore mode
        real_glMatrixMode(prev_mode);

        transformPushedThisFrame = 1;
        // Mark transforms as applied for this frame so other injection paths skip
        appliedEyeTransformThisFrame = 1;
        if (stereo_debug_enabled()) fprintf(stderr, "[stereo] Applied eye transform in glClear (eye=%s, offset=%.3f)\n",
                                            (current_camera == CAMERA_LEFT ? "L" : "R"), eye_offset);
    }

    // Optionally perform emitter swap at frame start to reduce phase jitter
    if ((mask & GL_COLOR_BUFFER_BIT) && swap_on_clear_enabled && !swap_done_this_frame && !usb_thread_mode) {
        // Align eye with parity before issuing swap when enabled
        if (video_sync_parity_enabled && glXGetVideoSyncSGI_ptr) {
            int left_par = get_left_parity();
            int desired_parity = (current_camera == CAMERA_LEFT) ? left_par : (1-left_par);
            if (glXWaitVideoSyncSGI_ptr) {
                unsigned int out = 0; glXWaitVideoSyncSGI_ptr(2, desired_parity, &out);
            } else {
                int eyeFromParity = select_eye_from_parity();
                if (eyeFromParity == 0) current_camera = CAMERA_LEFT;
                else if (eyeFromParity == 1) current_camera = CAMERA_RIGHT;
            }
        }
        if (resync_requested) { current_camera = CAMERA_LEFT; resync_requested = 0; }
        int left_token = swap_eyes ? nvstusb_right : nvstusb_left;
        int right_token = swap_eyes ? nvstusb_left : nvstusb_right;
        if (current_camera == CAMERA_LEFT) {
            nvstusb_swap(nv_ctx, left_token, glx_swap_callback);
            current_camera = CAMERA_RIGHT;
        } else {
            nvstusb_swap(nv_ctx, right_token, glx_swap_callback);
            current_camera = CAMERA_LEFT;
        }
        swap_done_this_frame = 1;
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

    // If we pushed a per-frame transform in glClear, glLoadIdentity() within that
    // frame will clear the translation/rotation on the top stack level.
    // Re-apply the eye transform here when in MODELVIEW mode so the
    // stereo offset and convergence remain effective for the frame.
    if (transformPushedThisFrame && !camera_transform_disabled) {
        GLint mode;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        if (mode == GL_MODELVIEW) {
            float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
            if (!projectionPushedThisFrame && CONVERGENCE > 0.001f) {
                float convergence_angle_rad = atanf(eye_offset / CONVERGENCE);
                float convergence_angle_deg = convergence_angle_rad * (180.0f / (float)M_PI);
                glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
            }
            glTranslatef(eye_offset, 0.0f, 0.0f);
        }
    }

    // Apply eye transform if afterGLmatrixMode is true (when not using push-in-clear)
    if (afterGLmatrixMode && !appliedEyeTransformThisFrame) {
        afterGLmatrixMode = 0;
        apply_eye_transform();
    }
}

// Interposed glLoadMatrixf - ensure app matrix loads do not wipe stereo transform
void glLoadMatrixf(const GLfloat *m) {
    if (!real_glLoadMatrixf) real_glLoadMatrixf = dlsym(RTLD_NEXT, "glLoadMatrixf");
    real_glLoadMatrixf(m);
    if (transformPushedThisFrame && !camera_transform_disabled) {
        GLint mode;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        if (mode == GL_MODELVIEW) {
            float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
            if (!projectionPushedThisFrame && CONVERGENCE > 0.001f) {
                float convergence_angle_rad = atanf(eye_offset / CONVERGENCE);
                float convergence_angle_deg = convergence_angle_rad * (180.0f / (float)M_PI);
                glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
            }
            glTranslatef(eye_offset, 0.0f, 0.0f);
        }
    }
}

// Interposed glLoadMatrixd - mirror float path
void glLoadMatrixd(const GLdouble *m) {
    if (!real_glLoadMatrixd) real_glLoadMatrixd = dlsym(RTLD_NEXT, "glLoadMatrixd");
    real_glLoadMatrixd(m);
    if (transformPushedThisFrame && !camera_transform_disabled) {
        GLint mode;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        if (mode == GL_MODELVIEW) {
            float eye_offset = (current_camera == CAMERA_LEFT) ? -EYE_OFFSET : EYE_OFFSET;
            if (CONVERGENCE > 0.001f) {
                float convergence_angle_rad = atanf(eye_offset / CONVERGENCE);
                float convergence_angle_deg = convergence_angle_rad * (180.0f / (float)M_PI);
                glRotatef(-convergence_angle_deg, 0.0f, 1.0f, 0.0f);
            }
            glTranslatef(eye_offset, 0.0f, 0.0f);
        }
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
    // Debug: number of clears in the last frame
    // printf("clear was called %i times\n", clearCount);
    clearCount = 0;

    current_dpy = dpy;
    current_drawable = drawable;
    
   
    // Perform initialization on first call
    if (!glxswapbuffers_initialized) {
        glxswapbuffers_initialized = 1;
        init_emitter();
    }

    // Poll hotkeys every frame
    poll_stereo_hotkeys();

    // If USB thread mode is enabled via config, ensure the background thread is running
    if (usb_thread_mode && emitter_initialized && !stereo_thread_started) {
        emitter_start_usb_thread();
        // When entering USB thread mode, reset sequencing so UI reflects correct next eye
        resync_requested = 1;
        current_camera = CAMERA_LEFT;
    }

    // Enforce swap interval if requested (best-effort across extensions)
    if (force_swap_interval) {
        static int tried = 0;
        if (!tried) {
            tried = 1;
            // Try SGI
            int (*glXSwapIntervalSGI_ptr)(int) = (int (*)(int)) real_glXGetProcAddress((const GLubyte*)"glXSwapIntervalSGI");
            if (glXSwapIntervalSGI_ptr) glXSwapIntervalSGI_ptr(1);
            // Try MESA
            int (*glXSwapIntervalMESA_ptr)(unsigned int) = (int (*)(unsigned int)) real_glXGetProcAddress((const GLubyte*)"glXSwapIntervalMESA");
            if (glXSwapIntervalMESA_ptr) glXSwapIntervalMESA_ptr(1);
            // Try EXT
            void (*glXSwapIntervalEXT_ptr)(Display*, GLXDrawable, int) = (void (*)(Display*, GLXDrawable, int)) real_glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
            if (glXSwapIntervalEXT_ptr) glXSwapIntervalEXT_ptr(dpy, drawable, 1);
            // Query SGI video sync once
            glXGetVideoSyncSGI_ptr = (int (*)(unsigned int*)) real_glXGetProcAddress((const GLubyte*)"glXGetVideoSyncSGI");
            glXWaitVideoSyncSGI_ptr = (int (*)(int,int,unsigned int*)) real_glXGetProcAddress((const GLubyte*)"glXWaitVideoSyncSGI");
            // Query OML sync control
            glXGetSyncValuesOML_ptr = (int (*)(Display*,GLXDrawable,long long*,long long*,long long*)) real_glXGetProcAddress((const GLubyte*)"glXGetSyncValuesOML");
            glXWaitForMscOML_ptr = (int (*)(Display*,GLXDrawable,long long,long long,long long,long long*,long long*,long long*)) real_glXGetProcAddress((const GLubyte*)"glXWaitForMscOML");
        }
    }

    // Apply compositor bypass hint to main window if requested
    if (bypass_compositor_main) {
        apply_bypass_main_if_needed(dpy, drawable);
    }
    // Apply fullscreen override if enabled
    if (fullscreen_enabled) {
        apply_fullscreen_if_needed(dpy, drawable);
    }

    // Detect window moves/resizes and hold emitter sequencing briefly
    if (stabilize_moves_enabled && !fullscreen_enabled) {
        XWindowAttributes attr;
        if (XGetWindowAttributes(dpy, (Window)drawable, &attr)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (!last_attr_valid) {
                last_attr_x = attr.x; last_attr_y = attr.y; last_attr_w = attr.width; last_attr_h = attr.height; last_attr_valid = 1;
            } else {
                if (attr.x != last_attr_x || attr.y != last_attr_y || attr.width != last_attr_w || attr.height != last_attr_h) {
                    last_attr_x = attr.x; last_attr_y = attr.y; last_attr_w = attr.width; last_attr_h = attr.height;
                    // Set hold window: now + stabilize_window_ms
                    move_hold_active = 1;
                    move_hold_until.tv_sec = now.tv_sec;
                    long add_ns = (long)stabilize_window_ms * 1000000L;
                    long ns = now.tv_nsec + add_ns;
                    move_hold_until.tv_sec += ns / 1000000000L;
                    move_hold_until.tv_nsec = ns % 1000000000L;
                }
            }
            if (move_hold_active) {
                if (now.tv_sec > move_hold_until.tv_sec || (now.tv_sec == move_hold_until.tv_sec && now.tv_nsec >= move_hold_until.tv_nsec)) {
                    move_hold_active = 0;
                    resync_requested = 1; // resync after drag ends
                }
            }
        }
    }

    if (stabilize_moves_enabled && move_hold_active) {
        // During active drag/move, perform raw swap only and hold current eye
        glx_swap_callback();
        goto SWAP_AND_CLEANUP;
    }
    // Window move/resize stabilization: if window is moving (ConfigureNotify flurry),
    // hold eye advance briefly to avoid L/R drift during compositor redraws.
    // NOTE: Do not consume application events here to avoid interfering with app's queue.
    // Stabilization based on ConfigureNotify is disabled to prevent event interaction.

    // If per-frame transforms were pushed in glClear, pop them before the swap
    if (transformPushedThisFrame) {
        GLint prev_mode;
        glGetIntegerv(GL_MATRIX_MODE, &prev_mode);
        // Pop modelview adjustment
        real_glMatrixMode(GL_MODELVIEW);
        real_glPopMatrix();
        // Pop projection adjustment if applied
        if (projectionPushedThisFrame) {
            real_glMatrixMode(GL_PROJECTION);
            real_glPopMatrix();
            projectionPushedThisFrame = 0;
        }
        // Restore
        real_glMatrixMode(prev_mode);
        transformPushedThisFrame = 0;
        if (stereo_debug_enabled()) fprintf(stderr, "[stereo] Popped per-frame transforms before swap\n");
    }

    // If swap and emitter were already driven at glClear, skip sequencing here
    if (swap_on_clear_enabled && swap_done_this_frame) {
        // We already called real swap via nvstusb_swap(glx_swap_callback) in glClear.
        // Avoid a second swap which would desynchronize the emitter.
        goto SWAP_AND_CLEANUP;
    }

    // Handle stereo frame sequencing (unless driven at frame start)
    if (!swap_on_clear_enabled) {
        // Optionally synchronize eye selection to vblank parity (SGI). Skip if USB thread drives emitter or OML is available (nvstusb handles that).
        if (!usb_thread_mode && video_sync_parity_enabled && glXGetVideoSyncSGI_ptr && !glXWaitForMscOML_ptr) {
            int left_par = get_left_parity();
            int desired_parity = (current_camera == CAMERA_LEFT) ? left_par : (1-left_par);
            if (glXWaitVideoSyncSGI_ptr) {
                unsigned int out = 0;
                glXWaitVideoSyncSGI_ptr(2, desired_parity, &out);
            } else {
                // If waiting is unavailable, at least re-evaluate which eye to present based on current parity
                int eyeFromParity = select_eye_from_parity();
                if (eyeFromParity == 0) current_camera = CAMERA_LEFT;
                else if (eyeFromParity == 1) current_camera = CAMERA_RIGHT;
            }
        }
        // If modelview push was set up in clear for this frame, confirm we popped it
        // before issuing swap (we do pop below). This ensures no stale transform leaks.
        if (resync_requested) {
            // On resync, force a known state; start with LEFT next
            current_camera = CAMERA_LEFT;
            resync_requested = 0;
        }
        int left_token = swap_eyes ? nvstusb_right : nvstusb_left;
        int right_token = swap_eyes ? nvstusb_left : nvstusb_right;
        if (current_camera == CAMERA_LEFT) {
            if (finish_before_swap) glFinish();
            if (!usb_thread_mode) {
                nvstusb_swap(nv_ctx, left_token, glx_swap_callback);
            } else {
                // In USB thread mode, still perform a real swap to keep the app animating
                glx_swap_callback();
            }
        current_camera = CAMERA_RIGHT;
    } else {
            if (finish_before_swap) glFinish();
            if (!usb_thread_mode) {
                nvstusb_swap(nv_ctx, right_token, glx_swap_callback);
            } else {
                glx_swap_callback();
            }
        current_camera = CAMERA_LEFT;
        }
    }

SWAP_AND_CLEANUP:
    // OSD maintenance
    if (osd_state == OSD_STATE_MENU) {
        osd_poll_events();
    } else {
    osd_tick(dpy);
    }

    // FPS draw/update
    fps_update_and_draw(dpy);

    /*
    // Restore the saved modelview matrix
    if (hasSavedMatrix) {
        GLint matrix_mode;
        glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
        real_glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(savedModelViewMatrix);
        real_glMatrixMode(matrix_mode);
        hasSavedMatrix = 0;
    }
    */
    afterSwapBuffers = 1;
    appliedEyeTransformThisFrame = 0;
    afterGLmatrixMode = 0;
    swap_done_this_frame = 0;

    // Drain USB controller events to prevent device stall when not using USB thread mode
    if (!usb_thread_mode && emitter_initialized && nv_ctx) {
        handle_user_input();
    }
}

// Matrix stack tracking - keep your existing implementation
static int matrix_stack_depth = 0;

int is_matrix_stack_empty(void) {
    return matrix_stack_depth == 0;
}

void glPushMatrix(void) {
    matrix_stack_depth++;
    real_glPushMatrix();

    // No deferred timer mechanism; transform applied right after modelview identity
    
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
    //fprintf(stderr, "[stereo] glTranslatef(x=%.3f, y=%.3f, z=%.3f)\n", x, y, z);
    
    // Call the real function
    real_glTranslatef(x, y, z);
}

// Interposed glRotatef - can be used for debugging or additional transformations
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    // Debug output
    //fprintf(stderr, "[stereo] glRotatef(angle=%.3f, x=%.3f, y=%.3f, z=%.3f)\n", angle, x, y, z);
    
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

// Interposed glXMakeCurrent - calls the real function
Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx) {
    initialize_real_functions();
    if (!real_glXMakeCurrent) {
        fprintf(stderr, "[stereo] Failed to find real glXMakeCurrent!\n");
        return 0;
    }
    return real_glXMakeCurrent(dpy, drawable, ctx);
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
        osd_init(dpy);
        init_default_bindings(dpy);
        config_load();
        update_action_keycodes(dpy);
        // Sanitize OSD toggle binding: ensure we always have a usable key (F6 fallback)
        if (action_keysym[ACT_OSD_TOGGLE] == NoSymbol || action_keycode[ACT_OSD_TOGGLE] == 0) {
            action_keysym[ACT_OSD_TOGGLE] = XK_F6;
            action_keycode[ACT_OSD_TOGGLE] = XKeysymToKeycode(dpy, XK_F6);
            fprintf(stderr, "[stereo] OSD toggle key was invalid; defaulting to F6.\n");
        }
    }
    static int keymap_poll_divider = 0;
    keymap_poll_divider = (keymap_poll_divider + 1) % 3; // poll ~every 3 frames
    if (keymap_poll_divider != 0 && osd_state != OSD_STATE_MENU) {
        // Skip polling this frame to reduce X11 round-trips during cursor motion
        return;
    }
    XQueryKeymap(dpy, keys);
    // Helper macro to check key state by keycode
    #define KEYCODE_IS_DOWN(keycode) ( (keycode) > 0 ? ( keys[(keycode)/8] & (1 << ((keycode)%8)) ) : 0 )
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < ACT_COUNT; i++) {
        int down = KEYCODE_IS_DOWN(action_keycode[i]) ? 1 : 0;
        long elapsed_ms = ((now.tv_sec - action_last_time[i].tv_sec) * 1000L) + ((now.tv_nsec - action_last_time[i].tv_nsec) / 1000000L);
        int should_fire = 0;
        if (action_repeatable[i]) {
            should_fire = down && (elapsed_ms >= action_repeat_ms[i]);
        } else {
            should_fire = down && !action_prev_down[i];
        }
        if (should_fire) {
            switch ((OsdAction)i) {
                case ACT_CONV_INC:
                    if (osd_state != OSD_STATE_MENU) {
                        CONVERGENCE += STEP_CONV;
        osd_showf(dpy, "Convergence: %.2f", CONVERGENCE);
                        config_save();
                    }
                    break;
                case ACT_CONV_DEC:
                    if (osd_state != OSD_STATE_MENU) {
                        CONVERGENCE -= STEP_CONV;
        osd_showf(dpy, "Convergence: %.2f", CONVERGENCE);
                        config_save();
                    }
                    break;
                case ACT_EYE_INC:
                    if (osd_state != OSD_STATE_MENU) {
                        EYE_OFFSET += STEP_EYE;
        osd_showf(dpy, "Eye offset: %.3f", EYE_OFFSET);
                        config_save();
                    }
                    break;
                case ACT_EYE_DEC:
                    if (osd_state != OSD_STATE_MENU) {
                        EYE_OFFSET -= STEP_EYE;
        osd_showf(dpy, "Eye offset: %.3f", EYE_OFFSET);
                        config_save();
                    }
                    break;
                case ACT_OSD_TOGGLE:
                    osd_toggle_menu();
                    break;
                default: break;
            }
            action_last_time[i] = now;
        }
        action_prev_down[i] = down;
    }
    #undef KEYCODE_IS_DOWN
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
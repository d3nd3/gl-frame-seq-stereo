#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

static int running = 1;
static int use_display_list = 0;
static int extra_load_identity = 0;
static int extra_clear_midframe = 0;
static int animate = 1;

static GLuint tri_list = 0;

static double get_time_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void build_display_list(void) {
    if (tri_list) glDeleteLists(tri_list, 1);
    tri_list = glGenLists(1);
    glNewList(tri_list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.2f, 0.2f); glVertex2f(-0.6f, -0.6f);
    glColor3f(0.2f, 1.0f, 0.2f); glVertex2f( 0.6f, -0.6f);
    glColor3f(0.2f, 0.2f, 1.0f); glVertex2f( 0.0f,  0.6f);
    glEnd();
    glEndList();
}

static void draw_triangle_immediate(void) {
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.2f, 0.2f); glVertex2f(-0.6f, -0.6f);
    glColor3f(0.2f, 1.0f, 0.2f); glVertex2f( 0.6f, -0.6f);
    glColor3f(0.2f, 0.2f, 1.0f); glVertex2f( 0.0f,  0.6f);
    glEnd();
}

static void handle_keypress(Display *dpy, XEvent *ev) {
    KeySym ks = XLookupKeysym(&ev->xkey, 0);
    switch (ks) {
        case XK_Escape: running = 0; break;
        case XK_l: case XK_L:
            use_display_list = !use_display_list;
            fprintf(stderr, "[demo] use_display_list: %d\n", use_display_list);
            break;
        case XK_i: case XK_I:
            extra_load_identity = !extra_load_identity;
            fprintf(stderr, "[demo] extra_load_identity: %d\n", extra_load_identity);
            break;
        case XK_c: case XK_C:
            extra_clear_midframe = !extra_clear_midframe;
            fprintf(stderr, "[demo] extra_clear_midframe: %d\n", extra_clear_midframe);
            break;
        case XK_a: case XK_A:
            animate = !animate;
            fprintf(stderr, "[demo] animate: %d\n", animate);
            break;
        default: break;
    }
}

int main(int argc, char **argv) {
    // Parse simple flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--use-lists") == 0) use_display_list = 1;
        else if (strcmp(argv[i], "--extra-identity") == 0) extra_load_identity = 1;
        else if (strcmp(argv[i], "--extra-clear") == 0) extra_clear_midframe = 1;
        else if (strcmp(argv[i], "--static") == 0) animate = 0;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "[demo] Failed to open display\n"); return 1; }
    int screen = DefaultScreen(dpy);
    int attrib[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
    XVisualInfo *vi = glXChooseVisual(dpy, screen, attrib);
    if (!vi) { fprintf(stderr, "[demo] No appropriate visual\n"); return 1; }

    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    XSetWindowAttributes swa;
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask;
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen),
                               100, 100, 640, 480, 0, vi->depth, InputOutput, vi->visual,
                               CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, "stereo-demo");
    XMapWindow(dpy, win);

    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
    glXMakeCurrent(dpy, win, ctx);

    glDisable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);

    if (use_display_list) build_display_list();

    double start = get_time_sec();
    while (running) {
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type == KeyPress) handle_keypress(dpy, &ev);
            if (ev.type == DestroyNotify) running = 0;
            if (ev.type == ConfigureNotify) {
                glViewport(0, 0, ev.xconfigure.width, ev.xconfigure.height);
            }
        }

        double t = get_time_sec() - start;
        float angle = animate ? (float)fmod(t * 50.0, 360.0) : 0.0f;

        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRotatef(angle, 0.0f, 0.0f, 1.0f);

        if (extra_load_identity) {
            // Simulate mid-frame identity reset
            glLoadIdentity();
            glRotatef(angle, 0.0f, 0.0f, 1.0f);
        }

        if (use_display_list) {
            glCallList(tri_list);
        } else {
            draw_triangle_immediate();
        }

        if (extra_clear_midframe) {
            // Simulate an extra clear and redraw, as a stress test
            glClear(GL_COLOR_BUFFER_BIT);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glRotatef(angle, 0.0f, 0.0f, 1.0f);
            if (use_display_list) glCallList(tri_list); else draw_triangle_immediate();
        }

        glXSwapBuffers(dpy, win);
        // Tiny sleep to keep CPU sane when vsync off
        usleep(1000);
    }

    if (tri_list) glDeleteLists(tri_list, 1);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XFreeColormap(dpy, cmap);
    XCloseDisplay(dpy);
    return 0;
}



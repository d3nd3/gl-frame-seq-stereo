#include <stdio.h>
#include <stdbool.h>
#include "nvstusb.h"
#include "emitter.h"

// Globals needed for emitter logic
struct nvstusb_context *nv_ctx = NULL;
int emitter_initialized = 0;
bool stereo_thread_started = false;

void init_emitter() {
    if (emitter_initialized) return;

    if (getenv("STEREO_DEBUG")) printf("[stereopreload] Initializing emitter...\n");
    nv_ctx = nvstusb_init();
    if (nv_ctx) {
        if (getenv("STEREO_DEBUG")) printf("[stereopreload] Emitter initialized successfully.\n");
        emitter_initialized = 1;
        
        //This is the line that wakes the emitter up
        nvstusb_set_refresh_rate(nv_ctx);
 
    } else {
        fprintf(stderr, "[stereopreload] ERROR: Failed to initialize emitter!\n");
    }
}

void emitter_start_usb_thread(void) {
    if (!emitter_initialized || !nv_ctx) return;
    if (stereo_thread_started) return;
    nvstusb_start_stereo_thread(nv_ctx);
    stereo_thread_started = true;
}

void emitter_stop_usb_thread(void) {
    if (!emitter_initialized || !nv_ctx) return;
    if (!stereo_thread_started) return;
    nvstusb_stop_stereo_thread(nv_ctx);
    stereo_thread_started = false;
}
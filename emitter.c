#include <stdio.h>
#include <stdbool.h>
#include "nvstusb.h"

// Globals needed for emitter logic
struct nvstusb_context *nv_ctx = NULL;
int emitter_initialized = 0;

void init_emitter() {
    if (emitter_initialized) return;

    printf("[stereopreload] Initializing emitter...\n");
    nv_ctx = nvstusb_init();
    if (nv_ctx) {
        printf("[stereopreload] Emitter initialized successfully.\n");
        emitter_initialized = 1;
        
        //This is the line that wakes the emitter up
        nvstusb_set_refresh_rate(nv_ctx);
 
    } else {
        fprintf(stderr, "[stereopreload] ERROR: Failed to initialize emitter!\n");
    }
}
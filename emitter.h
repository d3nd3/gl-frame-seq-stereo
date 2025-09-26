#ifndef EMITTER_H
#define EMITTER_H

#include <stdbool.h>
#include "nvstusb.h"

// Expose the emitter initialization function
void init_emitter();

// Optionally expose globals if needed externally
extern struct nvstusb_context *nv_ctx;
extern int emitter_initialized;
extern bool stereo_thread_started;

// Control the NVST USB background stereo thread
void emitter_start_usb_thread(void);
void emitter_stop_usb_thread(void);

#endif // EMITTER_H 
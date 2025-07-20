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
extern int render_count;

#endif // EMITTER_H 
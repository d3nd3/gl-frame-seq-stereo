/* nvstusb.c 
 * Copyright (C) 2010 Bjoern Paetzel
 * Copyright (C) 2010 Johann Baudy
 *
 * This program comes with ABSOLUTELY NO WARRANTY.
 * This is free software, and you are welcome to redistribute it
 * under certain conditions. See the file COPYING for details
 * */

 #include <stdio.h>
 #include <stdint.h>
 #include <stdlib.h>
 #include <string.h>
 #include <malloc.h>
 #include <time.h>
 #include <assert.h>
 #include <math.h>
  #include <stdbool.h>
 #include <pthread.h>
 #include <unistd.h>

 #include <GL/gl.h>
 #include <GL/glx.h>
 #include <GL/glxext.h>
 #include <GL/glext.h>
 
 #include "nvstusb.h"
 #include "usb.h"

#include <X11/Xlib.h>
 #include <X11/extensions/xf86vmode.h>
 #ifdef USE_XRANDR
 #include <X11/extensions/Xrandr.h>
 #endif

// --- Globals for the Asynchronous I/O Thread ---

// The thread itself
static pthread_t io_thread;

// Mutex and Condition Variable to manage the command queue
static pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_cond = PTHREAD_COND_INITIALIZER;

// The command queue. It's very simple: it just holds the next eye to set.
// A value of 0 means "no command pending".
// A value of 1 means "set left eye".
// A value of 2 means "set right eye".
// A value of -1 means "shut down the thread".
static volatile int eye_command = 0;

// We need a pointer to the nvstusb context for the I/O thread
static struct nvstusb_context *async_nv_ctx = NULL;

// Flag to track if the I/O thread is initialized
static int io_thread_initialized = 0;

// Forward declaration for nvstusb_set_eye
static void nvstusb_set_eye(struct nvstusb_context *ctx, enum nvstusb_eye eye);

// The consumer thread's main loop
static void* io_thread_func(void* arg) {
    (void)arg; // Unused

    fprintf(stderr, "[stereo-io] I/O thread started.\n");

    while (1) {
        // Lock the mutex to safely access the shared eye_command variable
        pthread_mutex_lock(&io_mutex);

        // Wait until there is a command.
        // pthread_cond_wait atomically unlocks the mutex and waits.
        // When woken up, it re-locks the mutex before continuing.
        while (eye_command == 0) {
            pthread_cond_wait(&io_cond, &io_mutex);
        }

        // We have a command. Capture it and reset the queue.
        int cmd = eye_command;
        eye_command = 0;

        // Unlock the mutex so the main thread isn't blocked while we do I/O
        pthread_mutex_unlock(&io_mutex);

        // --- Execute the command ---
        if (cmd == -1) {
            // Shutdown command
            break; 
        } else if (cmd == 1) { // Left eye
            // This is the original blocking call, but now it's in a safe thread
            nvstusb_set_eye(async_nv_ctx, nvstusb_left);
        } else if (cmd == 2) { // Right eye
            nvstusb_set_eye(async_nv_ctx, nvstusb_right);
        }
    }

    fprintf(stderr, "[stereo-io] I/O thread shutting down.\n");
    return NULL;
}

// The new non-blocking function to be called by the main thread
static void nvstusb_set_eye_async(enum nvstusb_eye eye) {
    // Lock the mutex to safely modify the command variable
    pthread_mutex_lock(&io_mutex);

    // Set the command for the I/O thread.
    // We overwrite any previous command that hasn't been processed yet.
    // This is fine because we only care about the most recent eye state.
    if (eye == nvstusb_left) {
        eye_command = 1;
    } else if (eye == nvstusb_right) {
        eye_command = 2;
    }

    // Signal the I/O thread that a new command is ready.
    // This will wake it up from pthread_cond_wait.
    pthread_cond_signal(&io_cond);

    // Unlock the mutex. This entire function is extremely fast.
    pthread_mutex_unlock(&io_mutex);
}

// Initialize the I/O thread
static void init_io_thread(struct nvstusb_context *ctx) {
    if (io_thread_initialized) return;
    
    async_nv_ctx = ctx;
    if (pthread_create(&io_thread, NULL, io_thread_func, NULL) != 0) {
        fprintf(stderr, "[stereo-io] Failed to create I/O thread!\n");
        return;
    }
    
    io_thread_initialized = 1;
    fprintf(stderr, "[stereo-io] I/O thread initialized.\n");
}

// Shutdown the I/O thread
static void shutdown_io_thread(void) {
    if (!io_thread_initialized) return;
    
    fprintf(stderr, "[stereo-io] Requesting I/O thread shutdown...\n");
    
    // Send the shutdown command
    pthread_mutex_lock(&io_mutex);
    eye_command = -1; // -1 is the shutdown signal
    pthread_cond_signal(&io_cond);
    pthread_mutex_unlock(&io_mutex);

    // Wait for the thread to finish
    pthread_join(io_thread, NULL);
    
    io_thread_initialized = 0;
    fprintf(stderr, "[stereo-io] Shutdown complete.\n");
}
 
 static PFNGLXGETVIDEOSYNCSGIPROC glXGetVideoSyncSGI = NULL;
 static PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI = NULL;
 static PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI = NULL;
 static PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT = NULL;
 static PFNGLXGETSYNCVALUESOMLPROC glXGetSyncValuesOML = NULL;
 static PFNGLXWAITFORMSCOMLPROC glXWaitForMscOML = NULL;
 static PFNGLXWAITFORSBCOMLPROC glXWaitForSbcOML = NULL;
 static PFNGLXSWAPBUFFERSMSCOMLPROC glXSwapBuffersMscOML = NULL;
 
 /* Static functions */
 static void nvstusb_print_refresh_rate(void);
 static void * nvstusb_stereo_thread(void * in_pv_arg);
 
 /* cpu clock */
 #define NVSTUSB_CLOCK           48000000LL
 
 /* T0 runs at 4 MHz */
 #define NVSTUSB_T0_CLOCK        (NVSTUSB_CLOCK/12LL)
 #define NVSTUSB_T0_COUNT(us)    (-(us)*(NVSTUSB_T0_CLOCK/1000000)+1)
 #define NVSTUSB_T0_US(count)    (-(count-1)/(NVSTUSB_T0_CLOCK/1000000))
 
 /* T2 runs at 12 MHz */
 #define NVSTUSB_T2_CLOCK        (NVSTUSB_CLOCK/ 4LL)
 #define NVSTUSB_T2_COUNT(us)    (-(us)*(NVSTUSB_T2_CLOCK/1000000)+1)
 #define NVSTUSB_T2_US(count)    (-(count-1)/(NVSTUSB_T2_CLOCK/1000000))
 
 #define NVSTUSB_CMD_WRITE       (0x01)  /* write data */
 #define NVSTUSB_CMD_READ        (0x02)  /* read data */
 #define NVSTUSB_CMD_CLEAR       (0x40)  /* set data to 0 */
 
 #define NVSTUSB_CMD_SET_EYE     (0xAA)  /* set current eye */
 #define NVSTUSB_CMD_CALL_X0199  (0xBE)  /* call routine at 0x0199 */
 
 /* state of the controller */
 struct nvstusb_context {
   /* currently selected refresh rate */
   float rate;
 
   /* currently active eye */
   enum nvstusb_eye eye;
 
   /* device handle */
   struct nvstusb_usb_device *device;
 
   /* Toggled state */
   int toggled3D;
 
   /* Vblank method */
   int vblank_method;
 
   /* Invert eyes command status */
   int invert_eyes;
 
   /* Stereo Thread handler */
   pthread_t s_thread;
 
   /* Stereo thread state */
   char b_thread_running;

    /* Prefer OML scheduled swaps when enabled */
    int use_oml_schedule;

    /* OML runtime health */
    int oml_schedule_ok;
    long long last_msc_seen;
    int oml_stall_score;
 };
 
 /* initialize controller */
struct nvstusb_context *
nvstusb_init(void) 
{

  /* initialize usb */
  if (!nvstusb_usb_init()) return 0;

  /* open device with the required sequence */
  struct nvstusb_usb_device *dev = 0;
  
  /* First two attempts with PRODUCTID 0x7003 */
  for (int i = 0; i < 2; i++) {
    dev = nvstusb_usb_open_device("nvstusb.fw");
    if (0 == dev) {
      fprintf(stderr, "nvstusb: Failed to open device on attempt %d\n", i + 1);
      return 0;
    }
    fprintf(stderr, "nvstusb: Successfully opened device on attempt %d\n", i + 1);
    
    /* Close the device after first two attempts */
    if (i < 1) {
      nvstusb_usb_close_device(dev);
      dev = 0;
      usleep(100000); /* Small delay between attempts */
    }
  }
  
  /* Final attempt with PRODUCTID 0x0007 */
  if (0 == dev) {
    dev = nvstusb_usb_open_device("nvstusb.fw");
    if (0 == dev) {
      fprintf(stderr, "nvstusb: Failed to open device on final attempt\n");
      return 0;
    }
    fprintf(stderr, "nvstusb: Successfully opened device on final attempt\n");
  }

  /* allocate context */
  struct nvstusb_context *ctx = malloc(sizeof(*ctx));
  if (0 == ctx) {
    fprintf(stderr, "nvstusb: Could not allocate %d bytes for nvstusb_context...\n", (int)sizeof(*ctx));
    nvstusb_usb_close_device(dev);
    nvstusb_usb_deinit();
    return 0;
  }
  ctx->rate = 0.0;
  ctx->eye = 0;
  ctx->device = dev;
  ctx->vblank_method = 0;
  ctx->toggled3D = 0;
  ctx->invert_eyes = 0;
  ctx->b_thread_running = 0;


  /* Optional env override for vblank method */
  const char *env_vblank = getenv("STEREO_VBLANK_METHOD");
  if (env_vblank) {
    ctx->vblank_method = atoi(env_vblank);
  }

  /* Vblank init */
  /* NVIDIA VBlank syncing environment variable defined, signal it and disable
   * any attempt to application side method */
  if (getenv ("__GL_SYNC_TO_VBLANK"))
  {
    if (getenv("STEREO_DEBUG")) fprintf (stderr, "__GL_SYNC_TO_VBLANK defined in environment\n");
    ctx->vblank_method = 2;
    goto out_err;
  }

  /* Swap interval */
  glXSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddress((const GLubyte*)"glXSwapIntervalSGI");
  glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");

  if (NULL != glXSwapIntervalSGI) {
    if (getenv("STEREO_DEBUG")) fprintf(stderr, "nvstusb: forcing vsync\n");
    if (!getenv("STEREO_PREFER_OML")) ctx->vblank_method = 3;
  }

  /* Sync Video */
  glXGetVideoSyncSGI = (PFNGLXGETVIDEOSYNCSGIPROC)glXGetProcAddress((const GLubyte*)"glXGetVideoSyncSGI");
  glXWaitVideoSyncSGI = (PFNGLXWAITVIDEOSYNCSGIPROC)glXGetProcAddress((const GLubyte*)"glXWaitVideoSyncSGI");
  if (NULL == glXWaitVideoSyncSGI) {
    glXGetVideoSyncSGI = 0;
  } else {
    ctx->vblank_method = 1;
  }

  if (NULL != glXGetVideoSyncSGI ) {
    if (getenv("STEREO_DEBUG")) fprintf(stderr, "nvstusb: GLX_SGI_video_sync supported!\n");
  }

  /* OML sync control */
  glXGetSyncValuesOML = (PFNGLXGETSYNCVALUESOMLPROC)glXGetProcAddress((const GLubyte*)"glXGetSyncValuesOML");
  glXWaitForMscOML = (PFNGLXWAITFORMSCOMLPROC)glXGetProcAddress((const GLubyte*)"glXWaitForMscOML");
  glXWaitForSbcOML = (PFNGLXWAITFORSBCOMLPROC)glXGetProcAddress((const GLubyte*)"glXWaitForSbcOML");
  glXSwapBuffersMscOML = (PFNGLXSWAPBUFFERSMSCOMLPROC)glXGetProcAddress((const GLubyte*)"glXSwapBuffersMscOML");
  if (glXGetSyncValuesOML && (glXWaitForMscOML || glXSwapBuffersMscOML)) {
    if (getenv("STEREO_DEBUG")) fprintf(stderr, "nvstusb: GLX_OML_sync_control supported!\n");
    if (!env_vblank) ctx->vblank_method = 4; /* prefer OML unless overridden */
  }

  {
    const char *method_name = "unknown";
    switch (ctx->vblank_method) {
      case 0: method_name = "software (front-read)"; break;
      case 1: method_name = "GLX_SGI_video_sync"; break;
      case 2: method_name = "__GL_SYNC_TO_VBLANK (driver)"; break;
      case 3: method_name = "SwapInterval (SGI/EXT)"; break;
      case 4: method_name = "GLX_OML_sync_control"; break;
    }
    fprintf(stderr, "nvstusb: vblank method: %d (%s), OML schedule: %s\n", ctx->vblank_method, method_name, ctx->use_oml_schedule ? "on" : "off");
  }
  /* OML schedule gating */
  ctx->use_oml_schedule = (getenv("STEREO_OML_SCHEDULE") && atoi(getenv("STEREO_OML_SCHEDULE")) != 0) ? 1 : 0;
  ctx->oml_schedule_ok = 1;
  ctx->last_msc_seen = 0;
  ctx->oml_stall_score = 0;

 out_err:
  return ctx;
}
 
 /* deinitialize controller */
 void
 nvstusb_deinit(
     struct nvstusb_context *ctx
     ) {
   if (0 == ctx) return;
 
   /* Close thread if running */
   if(ctx->b_thread_running) {
     nvstusb_stop_stereo_thread(ctx);
   }
 
   /* Shutdown I/O thread if running */
   shutdown_io_thread();
 
   /* close device */
   if (0 != ctx->device) nvstusb_usb_close_device(ctx->device);
   ctx->device = 0;
 
   /* close usb */
   nvstusb_usb_deinit();
 
   /* free context */
   memset(ctx, 0, sizeof(*ctx));
   free(ctx);
 }
 
 /* set controller refresh rate (should be monitor refresh rate) */
  void
  nvstusb_set_rate(
     struct nvstusb_context *ctx,
     float rate
     ) {
   assert(ctx != 0);
   assert(ctx->device != 0);
    if (rate < 60.0f) {
      fprintf(stderr, "nvstusb: refresh rate %.2f Hz too low for stereo; clamping to 60.00 Hz.\n", rate);
      rate = 60.0f;
    }
 
   /* send some magic data to device, this function is mainly black magic */
 
   /* some timing voodoo */
   int32_t frameTime   = (int32_t)(1000000.0 / (double)rate);     /* 8.33333 ms if 120 Hz */
    /* Active shutter on-time tuning */
    int32_t activeTime;
    const char *env_active = getenv("STEREO_ACTIVE_US");
    if (env_active && atoi(env_active) > 0) {
      activeTime = atoi(env_active);
    } else {
      /* Default to original 2.08 ms for stability */
      activeTime  = 2080;
    }
 
   int32_t w = NVSTUSB_T2_COUNT((int32_t)4568.50);      /* 4.56800 ms */
   int32_t x = NVSTUSB_T0_COUNT((int32_t)4774.25);      /* 4.77425 ms */
   int32_t y = NVSTUSB_T0_COUNT(activeTime);
   int32_t z = NVSTUSB_T2_COUNT(frameTime);    
 
   uint8_t cmdTimings[] = { 
     NVSTUSB_CMD_WRITE,      /* write data */
     0x00,                   /* to address 0x2007 (0x2007+0x00) = ?? */
     0x18, 0x00,             /* 24 bytes follow */
 
     /* original: e1 29 ff ff (-54815; -55835) */
     w, w>>8, w>>16, w>>24,    /* 2007: ?? some timer 2 counter, 1020 is subtracted from this
                                *       loaded at startup with:
                                *       0x44 0xEC 0xFE 0xFF (-70588(-1020)) */ 
     /* original: 68 b5 ff ff (-19096), 4.774 ms */
     x, x>>8, x>>16, x>>24,    /* 200b: ?? counter saved at long at address 0x4f
                                *       increased at timer 0 interrupt if bit 20h.1 
                                *       is cleared, on overflow
                                *       to 0 the code at 0x03c8 is executed.
                                *       timer 0 will be started with this value
                                *       by timer2 */
 
     /* original: 81 df ff ff (-8319), 2.08 ms */
     y, y>>8, y>>16, y>>24,    /* 200f: ?? counter saved at long at address 0x4f, 784 is added to this
                                *       if PD1 is set, delay until turning eye off? */
 
     /* wave forms to send via IR: */
     0x30,                     /* 2013: 110000 PD1=0, PD2=0: left eye off  */
     0x28,                     /* 2014: 101000 PD1=1, PD2=0: left eye on   */
     0x24,                     /* 2015: 100100 PD1=0, PD2=1: right eye off */
     0x22,                     /* 2016: 100010 PD1=1, PD2=1: right eye on  */
 
     /* ?? used when frameState is != 2, for toggling bits in Port B,
      * values seem to have no influence on the glasses or infrared signals */
     0x0a,                     /* 2017: 1010 */
     0x08,                     /* 2018: 1000 */
     0x05,                     /* 2019: 0101 */
     0x04,                     /* 201a: 0100 */
 
     z, z>>8, z>>16, z>>24     /* 201b: timer 2 reload value */
   }; 
   nvstusb_usb_write_bulk(ctx->device, 2, cmdTimings, sizeof(cmdTimings));
 
   uint8_t cmd0x1c[] = {
     NVSTUSB_CMD_WRITE,      /* write data */
     0x1c,                   /* to address 0x2023 (0x2007+0x1c) = ?? */
     0x02, 0x00,             /* 2 bytes follow */
 
     0x02, 0x00              /* ?? seems to be the start value of some 
                                counter. runs up to 6, some things happen
                                when it is lower, that will stop if when
                                it reaches 6. could be the index to 6 byte values 
                                at 0x17ce that are loaded into TH0*/
   };
   nvstusb_usb_write_bulk(ctx->device, 2, cmd0x1c, sizeof(cmd0x1c));
 
   /* wait at most 2 seconds before going into idle */
   uint16_t timeout = rate * 4;  
 
   uint8_t cmdTimeout[] = {
     NVSTUSB_CMD_WRITE,      /* write data */
     0x1e,                   /* to address 0x2025 (0x2007+0x1e) = timeout */
     0x02, 0x00,             /* 2 bytes follow */
 
     timeout, timeout>>8     /* idle timeout (number of frames) */
   };
   nvstusb_usb_write_bulk(ctx->device, 2, cmdTimeout, sizeof(cmdTimeout));
 
   uint8_t cmd0x1b[] = {
     NVSTUSB_CMD_WRITE,      /* write data */
     0x1b,                   /* to address 0x2022 (0x2007+0x1b) = ?? */
     0x01, 0x00,             /* 1 byte follows */
 
     0x07                    /* ?? compared with byte at 0x29 in TD_Poll()
                                bit 0-1: index to a table of 4 bytes at 0x17d4 (0x00,0x08,0x04,0x0C),
                                PB1 is set in TD_Poll() if this index is 0, cleared otherwise
                                bit 2:   set bool21_4, start timer 1, enable ext. int. 5
                                bit 3:   PC1 is set to the inverted value of this bit in TD_Poll()
                                bit 4-5: index to a table of 4 bytes at 0x2a 
                                bit 6:   restart t0 on some conditions in TD_Poll()
                              */
   };
   nvstusb_usb_write_bulk(ctx->device, 2, cmd0x1b, sizeof(cmd0x1b));
 
   ctx->rate = rate;
 }
 
 void
 nvstusb_invert_eyes(
     struct nvstusb_context *ctx
     ) {
   ctx->invert_eyes = !ctx->invert_eyes;
 }
 
 /* set currently open eye */
 static void
 nvstusb_set_eye(
     struct nvstusb_context *ctx,
     enum nvstusb_eye eye
     ) {
   assert(ctx != 0);
   assert(ctx->device != 0);
   assert(eye == nvstusb_left || eye == nvstusb_right || eye == nvstusb_quad);
   uint32_t r;
 
   //#define FF_TEST_R
 #ifdef FF_TEST_R
   static int i = 0;
   i++;
   static int j = 0;
   static uint32_t r_tmp = NVSTUSB_T2_COUNT(0);;
 
   if(ctx->toggled3D) {
     r = r_tmp;
   } else {
     r = NVSTUSB_T2_COUNT((int32_t)((1000000.0 / (double)ctx->rate) / 1.8));
   }
 
   if(i%32 == 0) {
     if(ctx->toggled3D) {
       r_tmp -= 500;
     }
 
     if(((int)r_tmp) < NVSTUSB_T2_COUNT((1e6/ctx->rate))) {
       r_tmp = NVSTUSB_T2_COUNT(0);
     }
     printf("r:%08x %d %lld %lld\n",r, r,NVSTUSB_T0_US(r), NVSTUSB_T2_US(r));
   }
 #else
   r = NVSTUSB_T2_COUNT((int32_t)((1000000.0 / (double)ctx->rate) / 1.8));
 #endif
 
   switch(eye) {
   case nvstusb_right:
   case nvstusb_left:
     {
       uint8_t buf[8] = { 
         NVSTUSB_CMD_SET_EYE,      /* set shutter state */
         ((eye==nvstusb_right)^(ctx->invert_eyes))?0xFE:0xFF,        /* eye selection */
         0x00, 0x00,               /* unused */
         r, r>>8, r>>16, r>>24
       };
       nvstusb_usb_write_bulk(ctx->device, 1, buf, 8);		
     }
     break;
   case nvstusb_quad:
     {
       nvstusb_set_eye(ctx, nvstusb_right);
       nvstusb_set_eye(ctx, nvstusb_left);
     }
     break;
   }
 }
 
 
 /* perform swap and toggle eyes hopefully with correct timing */
  void
  nvstusb_swap(
     struct nvstusb_context *ctx,
     enum nvstusb_eye eye,
     void (*swapfunc)()
     ) {
   assert(ctx != 0);
   assert(ctx->device != 0);
   assert(eye == nvstusb_left || eye == nvstusb_right || eye == nvstusb_quad);
 
   /* if we have the GLX_SGI_video_sync extension, we just wait
    * for vertical blanking, then issue swap. */
  switch(ctx->vblank_method) {
   case 0:
     {
       /* Swap buffers */
       if(swapfunc) {
         swapfunc();
       }
 
       /* Sw Vsync method: read from front buffer.
        * this operation can only finish after swapping is complete. 
        * (seems like it won't work if page flipping is disabled) */
       uint8_t pixels[4] = { 255, 0, 255, 255 };
       glReadBuffer(GL_FRONT);
       glReadPixels(1,1,1,1,GL_RGB, GL_UNSIGNED_BYTE, pixels);
       nvstusb_set_eye(ctx, eye);
     }
     break;
    case 1:
      {
        unsigned int count;
        glXGetVideoSyncSGI(&count);

        /* Target parity: 0 for left, 1 for right, with inversion support */
        int target_parity = ((eye == nvstusb_right) ^ ctx->invert_eyes) ? 1 : 0;
        glXWaitVideoSyncSGI(2, target_parity, &count);

        /* Swap on vblank edge first, then set emitter immediately */
        if (swapfunc) swapfunc();
        nvstusb_set_eye(ctx, eye);
      }
      break;
   case 2:
     {
       /* case __GL_SYNC_TO_VBLANK is defined */
 
       /* Initialize I/O thread if not already done */
       init_io_thread(ctx);
 
       /* Swap buffers */
       if(swapfunc) {
         swapfunc();
       }
 
       /* Change eye - use async version to avoid blocking */
       nvstusb_set_eye_async(eye);
     }
     break;
    case 3:
     {
       static int i_current_interval = -1;
       int i_interval;
       if(eye == nvstusb_quad) {
         i_interval = 2;
       } else {
         i_interval = 1;
       }
 
       /* Swap interval */
        if(i_current_interval != i_interval) {
          Display *dpy = glXGetCurrentDisplay();
          GLXDrawable draw = glXGetCurrentDrawable();
          if (glXSwapIntervalEXT && dpy && draw) {
            glXSwapIntervalEXT(dpy, draw, i_interval);
          } else if (glXSwapIntervalSGI) {
            glXSwapIntervalSGI(i_interval);
          }
          i_current_interval = i_interval;
        }
 
        /* For SGI swap interval, prefer matching parity similarly to method 1
           to reduce phase drift */
        if (ctx->vblank_method == 3 && glXGetVideoSyncSGI && glXWaitVideoSyncSGI) {
          unsigned int count;
          glXGetVideoSyncSGI(&count);
          int target_parity = ((eye == nvstusb_right) ^ ctx->invert_eyes) ? 1 : 0;
          glXWaitVideoSyncSGI(2, target_parity, &count);
        }

        if(swapfunc) swapfunc();
        nvstusb_set_eye(ctx, eye);
 
     }
     break;
  case 4:
      {
        /* OML sync control: prefer scheduled swap to specific parity */
        Display *dpy = glXGetCurrentDisplay();
        GLXDrawable draw = glXGetCurrentDrawable();
        if (dpy && draw && glXGetSyncValuesOML) {
          int64_t ust, msc, sbc;
          glXGetSyncValuesOML(dpy, draw, &ust, &msc, &sbc);
          int target_parity = ((eye == nvstusb_right) ^ ctx->invert_eyes) ? 1 : 0;
          if (ctx->use_oml_schedule && glXSwapBuffersMscOML) {
            /* Schedule swap at next MSC with parity = target_parity */
  int64_t target_divisor = 2LL;
  int64_t target_remainder = (int64_t)target_parity;
            int64_t target_msc = 0; /* as soon as possible */
            int64_t ret_sbc = glXSwapBuffersMscOML(dpy, draw, target_msc, target_divisor, target_remainder);
            if (ret_sbc >= 0 && glXWaitForSbcOML) {
              /* Wait for scheduled swap completion */
              glXWaitForSbcOML(dpy, draw, ret_sbc, &ust, &msc, &sbc);
              /* Health: MSC should advance; track stalls */
              if (msc <= ctx->last_msc_seen) {
                ctx->oml_stall_score++;
              } else if (ctx->oml_stall_score > 0) {
                ctx->oml_stall_score--;
              }
              ctx->last_msc_seen = msc;
              if (ctx->oml_stall_score > 6) {
                /* Too many non-advancing swaps; disable schedule path */
                ctx->oml_schedule_ok = 0;
                ctx->use_oml_schedule = 0;
                fprintf(stderr, "nvstusb: OML schedule disabled due to repeated MSC stalls.\n");
              }
            } else {
              /* Fallback: glFinish() after issuing swap */
              if (swapfunc) swapfunc();
            }
          } else if (glXWaitForMscOML) {
            /* Fallback: wait for target parity MSC, then swap immediately */
            glXWaitForMscOML(dpy, draw, 0, 2, target_parity, &ust, &msc, &sbc);
            if (swapfunc) swapfunc();
          } else {
            if (swapfunc) swapfunc();
          }
          /* Toggle the emitter immediately after swap aligns to target parity */
          nvstusb_set_eye(ctx, eye);
        } else {
          if (swapfunc) swapfunc();
          nvstusb_set_eye(ctx, eye);
        }
      }
      break;
    default:
     fprintf(stderr, "nvstusb: unknown vblank method\n");
   }
 
 
 
 }
 
 /* get key status from controller */
 void
 nvstusb_get_keys(
     struct nvstusb_context *ctx,
     struct nvstusb_keys *keys
     ) {
   assert(ctx  != 0);
   assert(keys != 0);
 
   uint8_t cmd1[] = { 
     NVSTUSB_CMD_READ |      /* read and clear data */
       NVSTUSB_CMD_CLEAR,
 
     0x18,                   /* from address 0x201F (0x2007+0x18) = status? */
     0x03, 0x00              /* read/clear 3 bytes */
   };
   nvstusb_usb_write_bulk(ctx->device, 2, cmd1, sizeof(cmd1));
 
   uint8_t readBuf[4+cmd1[2]];
   nvstusb_usb_read_bulk(ctx->device, 4, readBuf, sizeof(readBuf));
 
   /* readBuf[0] contains the offset (0x18),
    * readBuf[1] contains the number of read bytes (0x03),
    * readBuf[2] (msb) and readBuf[3] (lsb) of the bytes sent (sizeof(cmd1)) 
    * readBuf[4] and following contain the requested data */
 
   /* from address 0x201F:
    * signed 8 bit integer: amount the wheel was turned without the button pressed
    */
   keys->deltaWheel = readBuf[4];
 
   /* from address 0x2020:
    * signed 8 bit integer: amount the wheel was turned with the button pressed
    */
   keys->pressedDeltaWheel = readBuf[5];
 
   /* from address 0x2021:
    * bit 0: front button was pressed since last time (presumably fom pin 4 on port C)
    * bit 1: logic state of pin 7 on port E
    * bit 2: logic state of pin 2 on port C
    */
   keys->toggled3D  = readBuf[6] & 0x01; 
 
   if(keys->toggled3D) {
     ctx->toggled3D = !ctx->toggled3D;
   } 
 }
 
 /* Start Stereo Thread - For GL_STEREO */
 void nvstusb_start_stereo_thread(struct nvstusb_context *ctx) 
 {
   assert(ctx != 0);
   assert(ctx->device != 0);
 
   ctx->b_thread_running = true;
   if ( pthread_create(&ctx->s_thread, NULL, nvstusb_stereo_thread, (void *)ctx) != 0 ) {
     fprintf(stderr, "nvstusb: Unable to start stereo stread");
   }
 }
 
 /* End Stereo Thread - For GL_STEREO  */
 void nvstusb_stop_stereo_thread(struct nvstusb_context *ctx) 
 {
   assert(ctx != 0);
   assert(ctx->device != 0);
 
   if(!ctx->b_thread_running) return;
 
   ctx->b_thread_running = false;
   if ( pthread_join(ctx->s_thread, NULL) != 0 ) {
     fprintf(stderr, "nvstusb: Unable to wait end of stereo stread");
   }
 }
 
 /* Stereo thread - For GL_STEREO  */
 static void * nvstusb_stereo_thread(void * in_pv_arg)
 {
   struct nvstusb_context *ctx = (struct nvstusb_context *) in_pv_arg;
   Display *dpy;
   Window win;
 
   /* Openning X display */
   dpy = XOpenDisplay(0);
 
   /* Preparing new X window */
   Window s_window;
   static int attributeList[] =
   { GLX_RGBA,
     GLX_DOUBLEBUFFER,
     GLX_RED_SIZE,
     1,
     GLX_GREEN_SIZE,
     1,
     GLX_BLUE_SIZE,
     1,
     None };
   XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), attributeList);
   s_window = RootWindow(dpy, vi->screen);
   XSetWindowAttributes swa;
   swa.colormap = XCreateColormap(dpy, s_window, vi->visual, AllocNone);
   swa.override_redirect = true;
 
   /* Create X window 1x1 top left of screen */
   win = XCreateWindow(dpy,
       s_window ,
       0,
       0,
       1,
       1,
       0,
       vi->depth,
       InputOutput,
       vi->visual,
       CWColormap|CWOverrideRedirect,
       &swa);
 
   XMapWindow(dpy, win);
 
   /* Create glX context */
   GLXContext glx_ctx = glXCreateContext(dpy, vi, 0, true);
   glXMakeCurrent(dpy, win, glx_ctx);
 
   /* Loop until stop */
   while (ctx->b_thread_running) {
     /* Send swap to usb controler */
     nvstusb_swap(ctx, nvstusb_quad, NULL /*f_swap*/);
 
     /* Read status from usb controler */
     struct nvstusb_keys k;
     nvstusb_get_keys(ctx, &k);
     if (k.toggled3D) {
       nvstusb_invert_eyes(ctx);
     }
   }
   /* Destroy context */
   glx_ctx = glXGetCurrentContext();
   glXDestroyContext(dpy, glx_ctx);
 
   return NULL;
 }
 
 /* Refresh rate calculation */
 static void nvstusb_print_refresh_rate(void)
 {
   static int i_it = 0;
   static uint64_t i_last = 0;
   static uint64_t i_first = 0;
   static uint64_t i_acc_var = 0;
   double f_mean, f_var;
 
   /* First frame */
   if(i_it == 0) {
     struct timespec s_tmp;
     clock_gettime(CLOCK_REALTIME, &s_tmp);
     i_first = (uint64_t)s_tmp.tv_sec*1000000+(uint64_t)s_tmp.tv_nsec/1000;
     i_last = i_first;
     f_mean = 0;
     f_var = 0;
 
   } else {
     struct timespec s_tmp;
     clock_gettime(CLOCK_REALTIME, &s_tmp);
     uint64_t i_new = (uint64_t)s_tmp.tv_sec*1000000+(uint64_t)s_tmp.tv_nsec/1000;
     /* Update average */
     f_mean = (double)(i_new-i_first)/(i_it);
     /* Calculate variance */
     i_acc_var += pow((double)(i_new-i_last)-f_mean, 2);
 
     /* std dev */
      f_var = (double)sqrt((double)i_acc_var/(double)i_it);
     i_last = i_new;
 
     /* Display each 512 frame */
  if(i_it % 512 == 0) {
      fprintf(stderr,"nvstusb: frame:%d (%0.2f s) mean: %f Hz (%0.2f us) sqrt(var): %0.2f us (%0.1f %%)\n",i_it,(double)f_mean*(double)i_it/1000000.0, 1000000.0/f_mean, f_mean, f_var, 100.0*f_var/f_mean);
     }
   }
   /* Increment frame counter */
   i_it++;
 }
 

 /**
 * Syncs nvstusb refresh rate with the X11 display.
 */
void nvstusb_set_refresh_rate(struct nvstusb_context *ctx) {
    Display *display = XOpenDisplay(0);
    if (!display) {
        fprintf(stderr, "StereoHelper: Cannot open display.\n");
        return;
    }
    /* 1) Env override */
    const char *env_rate = getenv("STEREO_RATE_HZ");
    if (env_rate && atof(env_rate) > 0.0) {
        double frame_rate = atof(env_rate);
        printf("[stereopreload] Using env STEREO_RATE_HZ=%.2f Hz.\n", frame_rate);
        nvstusb_set_rate(ctx, frame_rate);
        XCloseDisplay(display);
        return;
    }

    int screen = DefaultScreen(display);
    /* 2) Try XRandR first (optional) */
    #ifdef USE_XRANDR
    {
        Window root = RootWindow(display, screen);
        XRRScreenResources *res = XRRGetScreenResources(display, root);
        if (res) {
            double frame_rate = 0.0;
            for (int i = 0; i < res->ncrtc && frame_rate == 0.0; ++i) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(display, res, res->crtcs[i]);
                if (ci && ci->mode != None && ci->noutput > 0) {
                    for (int m = 0; m < res->nmode; ++m) {
                        if (res->modes[m].id == ci->mode) {
                            XRRModeInfo *mi = &res->modes[m];
                            if (mi->hTotal > 0 && mi->vTotal > 0 && mi->dotClock > 0) {
                                double fr = (double)mi->dotClock / (mi->hTotal * mi->vTotal);
                                if (fr < 10.0) fr *= 1000.0; // handle kHz dotClock
                                frame_rate = fr;
                            }
                            break;
                        }
                    }
                }
                if (ci) XRRFreeCrtcInfo(ci);
            }
            if (frame_rate > 0.0) {
                printf("[stereopreload] Detected refresh rate via XRandR: %.2f Hz.\n", frame_rate);
                nvstusb_set_rate(ctx, frame_rate);
                XRRFreeScreenResources(res);
                XCloseDisplay(display);
                return;
            }
            XRRFreeScreenResources(res);
        }
    }
    #endif

    /* 3) Fallback to XF86VidMode */
    XF86VidModeModeLine mode_line;
    int pixel_clk = 0;
    if (XF86VidModeGetModeLine(display, screen, &pixel_clk, &mode_line)) {
        if (mode_line.htotal > 0 && mode_line.vtotal > 0) {
            double frame_rate = (double) pixel_clk * 1000.0 / mode_line.htotal / mode_line.vtotal;
            printf("[stereopreload] Detected refresh rate via XF86VidMode: %.2f Hz.\n", frame_rate);
            nvstusb_set_rate(ctx, frame_rate);
        }
    } else {
        fprintf(stderr, "[stereopreload] Could not get video mode line; refresh rate not set.\n");
    }
    XCloseDisplay(display);
}

// This function will be called automatically when your library is unloaded
__attribute__((destructor))
static void shutdown_io_thread_destructor(void) {
    shutdown_io_thread();
}
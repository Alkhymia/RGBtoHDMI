#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include "cache.h"
#include "defs.h"
#include "info.h"
#include "logging.h"
#include "rpi-aux.h"
#include "rpi-gpio.h"
#include "rpi-mailbox-interface.h"
#include "startup.h"
#include "rpi-mailbox.h"

#define NUM_CAL_PASSES 1
#define NUM_CAL_FRAMES 10

// #define INSTRUMENT_CAL

#ifdef DOUBLE_BUFFER
#include "rpi-interrupts.h"
#endif

static int sp_mode7[6] = {1, 1, 1, 1, 1, 1};
static int sp_default = 4;

typedef void (*func_ptr)();

#define GZ_CLK_BUSY    (1 << 7)
#define GP_CLK1_CTL (volatile uint32_t *)(PERIPHERAL_BASE + 0x101078)
#define GP_CLK1_DIV (volatile uint32_t *)(PERIPHERAL_BASE + 0x10107C)

extern int rgb_to_fb(unsigned char *fb, int chars_per_line, int bytes_per_line, int mode7);

extern int measure_vsync();

// 0     0 Hz     Ground
// 1     19.2 MHz oscillator
// 2     0 Hz     testdebug0
// 3     0 Hz     testdebug1
// 4     0 Hz     PLLA
// 5     1000 MHz PLLC (changes with overclock settings)
// 6     500 MHz  PLLD
// 7     216 MHz  HDMI auxiliary
// 8-15  0 Hz     Ground


// Source 1 = OSC = 19.2MHz
// Source 4 = PLLA = 0MHz
// Source 5 = PLLC = core_freq * 3 = (384 * 3) = 1152
// Source 6 = PLLD = 500MHz

void init_gpclk(int source, int divisor) {
   log_debug("A GP_CLK1_DIV = %08"PRIx32, *GP_CLK1_DIV);

   log_debug("B GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Stop the clock generator
   *GP_CLK1_CTL = 0x5a000000 | source;

   // Wait for BUSY low
   log_debug("C GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);
   while ((*GP_CLK1_CTL) & GZ_CLK_BUSY) {}
   log_debug("D GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Configure the clock generator
   *GP_CLK1_DIV = 0x5A000000 | (divisor << 12);
   *GP_CLK1_CTL = 0x5A000000 | source;

   log_debug("E GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Start the clock generator
   *GP_CLK1_CTL = 0x5A000010 | source;

   log_debug("F GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);
   while (!((*GP_CLK1_CTL) & GZ_CLK_BUSY)) {}    // Wait for BUSY high
   log_debug("G GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   log_debug("H GP_CLK1_DIV = %08"PRIx32, *GP_CLK1_DIV);
}

static unsigned char* fb = NULL;
static int width = 0, height = 0, pitch = 0;

#ifdef USE_PROPERTY_INTERFACE_FOR_FB

void init_framebuffer(int mode7) {

   rpi_mailbox_property_t *mp;

   int w = mode7 ? SCREEN_WIDTH_MODE7 : SCREEN_WIDTH_MODE06;

   /* Initialise a framebuffer... */
   RPI_PropertyInit();
   RPI_PropertyAddTag( TAG_ALLOCATE_BUFFER );
   RPI_PropertyAddTag( TAG_SET_PHYSICAL_SIZE, w, SCREEN_HEIGHT );
#ifdef DOUBLE_BUFFER
   RPI_PropertyAddTag( TAG_SET_VIRTUAL_SIZE, w, SCREEN_HEIGHT * 2 );
#else
   RPI_PropertyAddTag( TAG_SET_VIRTUAL_SIZE, w, SCREEN_HEIGHT );
#endif
   RPI_PropertyAddTag( TAG_SET_DEPTH, SCREEN_DEPTH );
   if (SCREEN_DEPTH <= 8) {
      RPI_PropertyAddTag( TAG_SET_PALETTE );
   }
   RPI_PropertyAddTag( TAG_GET_PITCH );
   RPI_PropertyAddTag( TAG_GET_PHYSICAL_SIZE );
   RPI_PropertyAddTag( TAG_GET_DEPTH );

   RPI_PropertyProcess();

   // FIXME: A small delay (like the log) is neccessary here
   // or the RPI_PropertyGet seems to return garbage
   log_info( "Initialised Framebuffer" );

   if( ( mp = RPI_PropertyGet( TAG_GET_PHYSICAL_SIZE ) ) )
   {
      width = mp->data.buffer_32[0];
      height = mp->data.buffer_32[1];

      log_info( "Size: %dx%d ", width, height );
   }

   if( ( mp = RPI_PropertyGet( TAG_GET_PITCH ) ) )
   {
      pitch = mp->data.buffer_32[0];
      log_info( "Pitch: %d bytes", pitch );
   }

   if( ( mp = RPI_PropertyGet( TAG_ALLOCATE_BUFFER ) ) )
   {
      fb = (unsigned char*)mp->data.buffer_32[0];
      log_info( "Framebuffer address: %8.8X", (unsigned int)fb );
   }

   // On the Pi 2/3 the mailbox returns the address with bits 31..30 set, which is wrong
   fb = (unsigned char *)(((unsigned int) fb) & 0x3fffffff);
}

#else

// An alternative way to initialize the framebuffer using mailbox channel 1
//
// I was hoping it would then be possible to page flip just by modifying the structure
// in-place. Unfortunately that didn't work, but the code might be useful in the future.

typedef struct {
   uint32_t width;
   uint32_t height;
   uint32_t virtual_width;
   uint32_t virtual_height;
   volatile uint32_t pitch;
   volatile uint32_t depth;
   uint32_t x_offset;
   uint32_t y_offset;
   volatile uint32_t pointer;
   volatile uint32_t size;
} framebuf;

// The + 0x10000 is to miss the property buffer

static framebuf *fbp = (framebuf *) (UNCACHED_MEM_BASE + 0x10000);

void init_framebuffer(int mode7) {
   log_info( "Framebuf struct address: %p", fbp );

   int w = mode7 ? SCREEN_WIDTH_MODE7 : SCREEN_WIDTH_MODE06;

   // Fill in the frame buffer structure
   fbp->width          = w;
   fbp->height         = SCREEN_HEIGHT;
   fbp->virtual_width  = w;
#ifdef DOUBLE_BUFFER
   fbp->virtual_height = SCREEN_HEIGHT * 2;
#else
   fbp->virtual_height = SCREEN_HEIGHT;
#endif
   fbp->pitch          = 0;
   fbp->depth          = SCREEN_DEPTH;
   fbp->x_offset       = 0;
   fbp->y_offset       = 0;
   fbp->pointer        = 0;
   fbp->size           = 0;

   // Send framebuffer struct to the mailbox
   //
   // The +0x40000000 ensures the GPU bypasses it's cache when accessing
   // the framebuffer struct. If this is not done, the screen still initializes
   // but the ARM doesn't see the updated value for a very long time
   // i.e. the commented out section of code below is needed, and this eventually
   // exits with i=4603039
   //
   // 0xC0000000 should be added if disable_l2cache=1
   RPI_Mailbox0Write(MB0_FRAMEBUFFER, ((unsigned int)fbp) + 0xC0000000 );

   // Wait for the response (0)
   RPI_Mailbox0Read( MB0_FRAMEBUFFER );

   pitch = fbp->pitch;
   fb = (unsigned char*)(fbp->pointer);
   width = fbp->width;
   height = fbp->height;

   // See comment above
   // int i  = 0;
   // while (!pitch || !fb) {
   //    pitch = fbp->pitch;
   //    fb = (unsigned char*)(fbp->pointer);
   //    i++;
   // }
   // log_info( "i=%d", i);

   log_info( "Initialised Framebuffer: %dx%d ", width, height );
   log_info( "Pitch: %d bytes", pitch );
   log_info( "Framebuffer address: %8.8X", (unsigned int)fb );

   // Initialize the palette
   if (SCREEN_DEPTH <= 8) {
      RPI_PropertyInit();
      RPI_PropertyAddTag( TAG_SET_PALETTE );
      RPI_PropertyProcess();
   }

   // On the Pi 2/3 the mailbox returns the address with bits 31..30 set, which is wrong
   fb = (unsigned char *)(((unsigned int) fb) & 0x3fffffff);
}

#endif

int delay;

int calibrate_clock() {
   int a = 13;
   unsigned int frame_ref;

   log_info("     Nominal clock = %d Hz", CORE_FREQ);

   unsigned int frame_time = measure_vsync();

   if (frame_time & INTERLACED_FLAG) {
      frame_ref = 40000000;
      log_info("Nominal frame time = %d ns (interlaced)", frame_ref);
   } else {
      frame_ref = 40000000 - 64000;
      log_info("Nominal frame time = %d ns (non-interlaced)", frame_ref);
   }
   frame_time &= ~INTERLACED_FLAG;

   log_info(" Actual frame time = %d ns", frame_time);

   double error = (double) frame_time / (double) frame_ref;
   log_info("  Frame time error = %d PPM", (int) ((error - 1.0) * 1e6));

   int new_clock = (int) (((double) CORE_FREQ) / error);
   log_info("     Optimal clock = %d Hz", new_clock);

   // Sanity check clock
   if (new_clock < 380000000 || new_clock > 388000000) {
      log_info("Clock out of range 380MHz-388MHz, defaulting to 384MHz");
      new_clock = CORE_FREQ;
   }

   // Wait a while to allow UART time to empty
   for (delay = 0; delay < 100000; delay++) {
      a = a * 13;
   }

   // Switch to new core clock speed
   RPI_PropertyInit();
   RPI_PropertyAddTag( TAG_SET_CLOCK_RATE, CORE_CLK_ID, new_clock, 1);
   RPI_PropertyProcess();

   // Re-initialize UART, as system clock rate changed
   RPI_AuxMiniUartInit( 115200, 8 );

   // Check the new clock
   int actual_clock = get_clock_rate(CORE_CLK_ID);
   log_info("       Final clock = %d Hz", actual_clock);

   return a;
}

void init_sampling_point_register(int *sp_mode7, int def) {
   int i;
   int j;
   int sp = ((def & 7) << 18);
   for (i = 0; i <= 5; i++) {
      sp |= (sp_mode7[i] & 7) << (i * 3);
   }
   for (i = 0; i <= 20; i++) {
      RPI_SetGpioValue(SP_DATA_PIN, sp & 1);
      for (j = 0; j < 1000; j++);
      RPI_SetGpioValue(SP_CLK_PIN, 0);
      RPI_SetGpioValue(SP_CLK_PIN, 1);
      for (j = 0; j < 1000; j++);
      sp >>= 1;
   }
   RPI_SetGpioValue(SP_DATA_PIN, 0);
}

void init_hardware() {
   int i;
   for (i = 0; i < 12; i++) {
      RPI_SetGpioPinFunction(PIXEL_BASE + i, FS_INPUT);
   }
   RPI_SetGpioPinFunction(PSYNC_PIN, FS_INPUT);
   RPI_SetGpioPinFunction(CSYNC_PIN, FS_INPUT);
   RPI_SetGpioPinFunction(MODE7_PIN, FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_CLK_PIN, FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_DATA_PIN, FS_OUTPUT);
   RPI_SetGpioPinFunction(ELK_PIN, FS_OUTPUT);
   RPI_SetGpioPinFunction(LED1_PIN, FS_OUTPUT);
   RPI_SetGpioPinFunction(CAL_PIN, FS_INPUT);
   RPI_SetGpioValue(SP_CLK_PIN, 1);
   RPI_SetGpioValue(SP_DATA_PIN, 0);
   RPI_SetGpioValue(ELK_PIN, 0);
   RPI_SetGpioValue(LED1_PIN, 1); // 1 is off

#ifdef DOUBLE_BUFFER
   // This line enables IRQ interrupts
   // Enable smi_int which is IRQ 48
   // https://github.com/raspberrypi/firmware/issues/67
   RPI_GetIrqController()->Enable_IRQs_2 = (1 << VSYNCINT);
#endif

   // Initialize hardware cycle counter
   _init_cycle_counter();

   // Measure the frame time and set a clock to 384MHz +- the error
   calibrate_clock();

   // Configure the GPCLK pin as a GPCLK
   RPI_SetGpioPinFunction(GPCLK_PIN, FS_ALT5);

   // Initialize the sampling points
   init_sampling_point_register(sp_mode7, sp_default);

   // Initialise the info system with cached values (as we break the GPU property interface)
   init_info();

#ifdef DEBUG
   dump_useful_info();
#endif
}


// TODO: Don't hardcode pitch!
static char last[SCREEN_HEIGHT * 336] __attribute__((aligned(32)));


int diff_N_frames(int sp, int n, int mode7, int elk, int chars_per_line) {

   int sum = 0;
   int min = INT_MAX;
   int max = INT_MIN;

#ifdef INSTRUMENT_CAL
   unsigned int t;
   unsigned int t_capture = 0;
   unsigned int t_memcpy = 0;
   unsigned int t_compare = 0;
#endif

   // In mode 0..6, set BIT_CAL_COUNT to 1 (capture 1 field)
   // In mode 7, set BIT_CAL_COUNT to 0 (capture two fields, doesn't matter whether odd-even or even-odd)
   unsigned int flags = mode7 | BIT_CALIBRATE | (mode7 ? 0 : BIT_CAL_COUNT) | (elk ? BIT_ELK : 0);

#ifdef INSTRUMENT_CAL
   t = _get_cycle_counter();
#endif
   // Grab an initial frame
   rgb_to_fb(fb, chars_per_line, pitch, flags);
#ifdef INSTRUMENT_CAL
   t_capture += _get_cycle_counter() - t;
#endif

   for (int i = 0; i < n; i++) {
      int diff = 0;

#ifdef INSTRUMENT_CAL
      t = _get_cycle_counter();
#endif
      // Save the last frame
      memcpy((void *)last, (void *)fb, SCREEN_HEIGHT * pitch);
#ifdef INSTRUMENT_CAL
      t_memcpy += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Grab the next frame
      rgb_to_fb(fb, chars_per_line, pitch, flags);
#ifdef INSTRUMENT_CAL
      t_capture += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Compare the frames
      uint32_t *fbp = (uint32_t *)fb;
      uint32_t *lastp = (uint32_t *)last;
      for (int j = 0; j < SCREEN_HEIGHT * pitch; j += 4) {
         uint32_t d = (*fbp++) ^ (*lastp++);
         while (d) {
            if (d & 0x0F) {
               diff++;
            }
            d >>= 4;
         }
      }
#ifdef INSTRUMENT_CAL
      t_compare += _get_cycle_counter() - t;
#endif

      // Accumulate the result
      sum += diff;
      if (diff < min) {
         min = diff;
      }
      if (diff > max) {
         max = diff;
      }
   }

   int mean = sum / n;

   log_debug("sample point %d: diff:  sum = %d mean = %d, min = %d, max = %d", sp, sum, mean, min, max);
#ifdef INSTRUMENT_CAL
   log_debug("t_capture total = %d, mean = %d ", t_capture, t_capture / (n + 1));
   log_debug("t_compare total = %d, mean = %d ", t_compare, t_compare / n);
   log_debug("t_memcpy  total = %d, mean = %d ", t_memcpy,  t_memcpy / n);
   log_debug("total = %d", t_capture + t_compare + t_memcpy);
#endif
   return sum;
}

int total_N_frames(int sp, int n, int mode7, int elk, int chars_per_line) {

   int sum = 0;
   int min = INT_MAX;
   int max = INT_MIN;

#ifdef INSTRUMENT_CAL
   unsigned int t;
   unsigned int t_capture = 0;
   unsigned int t_compare = 0;
#endif

   // In mode 0..6, set BIT_CAL_COUNT to 1 (capture 1 field)
   // In mode 7, set BIT_CAL_COUNT to 0 (capture two fields, doesn't matter whether odd-even or even-odd)
   unsigned int flags = mode7 | BIT_CALIBRATE | (mode7 ? 0 : BIT_CAL_COUNT) | (elk ? BIT_ELK : 0);

   for (int i = 0; i < n; i++) {
      int total = 0;

      // Grab the next frame
      rgb_to_fb(fb, chars_per_line, pitch, flags);
#ifdef INSTRUMENT_CAL
      t_capture += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Compare the frames
      uint32_t *fbp = (uint32_t *)fb;
      for (int j = 0; j < SCREEN_HEIGHT * pitch; j += 4) {
         uint32_t f = *fbp++;
         while (f) {
            if (f & 0x0F) {
               total++;
            }
            f >>= 4;
         }
      }
#ifdef INSTRUMENT_CAL
      t_compare += _get_cycle_counter() - t;
#endif

      // Accumulate the result
      sum += total;
      if (total < min) {
         min = total;
      }
      if (total > max) {
         max = total;
      }
   }

   int mean = sum / n;
   log_debug("sample point %d: total: sum = %d mean = %d, min = %d, max = %d", sp, sum, mean, min, max);
#ifdef INSTRUMENT_CAL
   log_debug("t_capture total = %d, mean = %d ", t_capture, t_capture / (n + 1));
   log_debug("t_compare total = %d, mean = %d ", t_compare, t_compare / n);
   log_debug("total = %d", t_capture + t_compare + t_memcpy);
#endif
   return sum;
}

// Wait for the cal button to be released
void wait_for_cal_release() {
   int cal_bit = 0;
   do {
      cal_bit = ((*(volatile uint32_t *)(PERIPHERAL_BASE + 0x200034)) >> CAL_PIN) & 1;
      log_debug("cal_bit = %d", cal_bit);
   } while (cal_bit == 0);
}

void calibrate_sampling(int mode7, int elk, int chars_per_line) {
   int i;
   int j;
   int min_i;
   int min_metric;
   int metric;

   if (mode7) {
      log_info("Calibrating mode 7");

      min_metric = INT_MAX;
      min_i = 0;
      for (i = 0; i <= 7; i++) {
         for (j = 0; j <= 5; j++) {
            sp_mode7[j] = i;
         }
         init_sampling_point_register(sp_mode7, sp_default);
         metric = diff_N_frames(i, NUM_CAL_FRAMES, mode7, elk, chars_per_line);
         if (metric < min_metric) {
            min_metric = metric;
            min_i = i;
         }
      }
      for (i = 0; i <= 5; i++) {
         sp_mode7[i] = min_i;
      }
      init_sampling_point_register(sp_mode7, sp_default);


      log_info("Calibration in progress: mode 7: %d %d %d %d %d %d",
               sp_mode7[0], sp_mode7[1], sp_mode7[2], sp_mode7[3], sp_mode7[4], sp_mode7[5]);
      int ref = min_metric;
      log_debug("ref = %d", ref);
      for (i = 0; i <= 5; i++) {
         int left = INT_MAX;
         int right = INT_MAX;
         if (sp_mode7[i] > 0) {
            sp_mode7[i]--;
            init_sampling_point_register(sp_mode7, sp_default);
            left = diff_N_frames(i, NUM_CAL_FRAMES, mode7, elk, chars_per_line);
            sp_mode7[i]++;
         }
         if (sp_mode7[i] < 7) {
            sp_mode7[i]++;
            init_sampling_point_register(sp_mode7, sp_default);
            right = diff_N_frames(i, NUM_CAL_FRAMES, mode7, elk, chars_per_line);
            sp_mode7[i]--;
         }
         if (left < right && left < ref) {
            sp_mode7[i]--;
            ref = left;
            log_debug("nudged %d left, metric = %d", i, ref);
         } else if (right < left && right < ref) {
            sp_mode7[i]++;
            ref = right;
            log_debug("nudged %d right, metric = %d", i, ref);
         }
      }
      init_sampling_point_register(sp_mode7, sp_default);
      log_info("Calibration complete: mode 7: %d %d %d %d %d %d",
               sp_mode7[0], sp_mode7[1], sp_mode7[2], sp_mode7[3], sp_mode7[4], sp_mode7[5]);
   } else {
      log_info("Calibrating modes 0..6");
      min_metric = INT_MAX;
      min_i = 0;
      for (i = 0; i <= 5; i++) {
         init_sampling_point_register(sp_mode7, i);
         metric = diff_N_frames(i, NUM_CAL_FRAMES, mode7, elk, chars_per_line);
         if (metric < min_metric) {
            min_metric = metric;
            min_i = i;
         }
      }
      sp_default = min_i;
      log_info("Setting sp_default = %d", min_i);
      init_sampling_point_register(sp_mode7, sp_default);
   }
}

int test_for_elk(int mode7, int chars_per_line) {

   // If mode 7, then assume the Beeb
   if (mode7) {
      return 0;
   }

   unsigned int flags = BIT_CALIBRATE | BIT_CAL_COUNT;
   unsigned char *fb1 = fb;
   unsigned char *fb2 = fb + SCREEN_HEIGHT * pitch;

   // Grab one field
   rgb_to_fb(fb1, chars_per_line, pitch, flags);

   // Grab second field
   rgb_to_fb(fb2, chars_per_line, pitch, flags);

   unsigned int min_diff = INT_MAX;
   unsigned int min_offset = 0;

   for (int offset = -2; offset <= 2; offset += 2) {

      uint32_t *p1 = (uint32_t *)(fb1 + 2 * pitch);
      uint32_t *p2 = (uint32_t *)(fb2 + 2 * pitch + offset * pitch);
      unsigned int diff = 0;
      for (int i = 0; i < (SCREEN_HEIGHT - 4) * pitch; i += 4) {
         uint32_t d = (*p1++) ^ (*p2++);
         while (d) {
            if (d & 0x0F) {
               diff++;
            }
            d >>= 4;
         }
      }
      if (diff < min_diff) {
         min_diff = diff;
         min_offset = offset;
      }
      log_debug("offset = %d, diff = %u", offset, diff);

   }
   log_debug("min offset = %d", min_offset);
   return min_offset != 0;
}


void rgb_to_hdmi_main() {
   int elk;
   int mode7;
   int last_mode7;
   int result;

   // The divisor us now the same for both modes
   log_debug("Setting up divisor");
   init_gpclk(GPCLK_SOURCE, DEFAULT_GPCLK_DIVISOR);
   log_debug("Done setting up divisor");

   // Determine initial mode
   mode7 = rgb_to_fb(fb, 0, 0, BIT_PROBE);
   elk = 0;

   while (1) {
      log_debug("Setting mode7 = %d", mode7);
      RPI_SetGpioValue(MODE7_PIN, mode7);

      log_debug("Setting up frame buffer");
      init_framebuffer(mode7);
      log_debug("Done setting up frame buffer");

      int chars_per_line = mode7 ? MODE7_CHARS_PER_LINE : DEFAULT_CHARS_PER_LINE;

      do {

         log_debug("Entering rgb_to_fb");
         result = rgb_to_fb(fb, chars_per_line, pitch, mode7 | BIT_INITIALIZE | (elk ? BIT_ELK : 0));
         log_debug("Leaving rgb_to_fb, result= %d", result);

         if (result & BIT_CAL) {
            wait_for_cal_release();
            elk = test_for_elk(mode7, chars_per_line);
            log_debug("Elk mode = %d", elk);
            for (int c = 0; c < NUM_CAL_PASSES; c++) {
               calibrate_sampling(mode7, elk, chars_per_line);
            }
         }

         last_mode7 = mode7;
         mode7 = result & BIT_MODE7;

      } while (mode7 == last_mode7);
   }
}

#ifdef HAS_MULTICORE
static void start_core(int core, func_ptr func) {
   printf("starting core %d\r\n", core);
   *(unsigned int *)(0x4000008C + 0x10 * core) = (unsigned int) func;
}
#endif

#ifdef DOUBLE_BUFFER
void swapBuffer(int buffer) {
   // Flush the previous response from the GPU->ARM mailbox
   // Doing it like this avoids stalling for the response
   RPI_Mailbox0Flush( MB0_TAGS_ARM_TO_VC  );
   RPI_PropertyInit();
   if (buffer) {
      RPI_PropertyAddTag( TAG_SET_VIRTUAL_OFFSET, 0, SCREEN_HEIGHT);
   } else {
      RPI_PropertyAddTag( TAG_SET_VIRTUAL_OFFSET, 0, 0);
   }
   // Use version that doesn't wait for the response
   RPI_PropertyProcessNoCheck();
}
#endif

void kernel_main(unsigned int r0, unsigned int r1, unsigned int atags)
{
   RPI_AuxMiniUartInit( 115200, 8 );

   log_info("RGB to HDMI booted");

   init_hardware();

   enable_MMU_and_IDCaches();
   _enable_unaligned_access();

#ifdef HAS_MULTICORE
   int i;

   printf("main running on core %d\r\n", _get_core());

   for (i = 0; i < 10000000; i++);
   start_core(1, _spin_core);
   for (i = 0; i < 10000000; i++);
   start_core(2, _spin_core);
   for (i = 0; i < 10000000; i++);
   start_core(3, _spin_core);
   for (i = 0; i < 10000000; i++);
#endif

   rgb_to_hdmi_main();
}

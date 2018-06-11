#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "defs.h"
#include "osd.h"
#include "cpld.h"
#include "rpi-gpio.h"
#include "rpi-mailbox-interface.h"
#include "saa5050_font.h"

#define NLINES 4
#define LINELEN 40

static char buffer[LINELEN * NLINES];
static int attributes[NLINES];

// Mapping table for expanding 12-bit row to 24 bit pixel (3 words)
static uint32_t double_size_map[0x1000 * 3];

// Mapping table for expanding 12-bit row to 12 bit pixel (2 words + 2 words)
static uint32_t normal_size_map[0x1000 * 4];

static char message[80];

static int active = 0;

void osd_init() {
   // Precalculate character->screen mapping table
   //
   // Normal size mapping, odd numbered characters
   //
   // char bit  0 -> double_size_map + 2 bit 31
   // ...
   // char bit  7 -> double_size_map + 2 bit  3
   // char bit  8 -> double_size_map + 1 bit 31
   // ...
   // char bit 11 -> double_size_map + 1 bit 19
   //
   // Normal size mapping, even numbered characters
   //
   // char bit  0 -> double_size_map + 1 bit 15
   // ...
   // char bit  3 -> double_size_map + 1 bit  3
   // char bit  4 -> double_size_map + 0 bit 31
   // ...
   // char bit 11 -> double_size_map + 0 bit  3
   //
   // Double size mapping
   //
   // char bit  0 -> double_size_map + 2 bits 31, 27
   // ...
   // char bit  3 -> double_size_map + 2 bits  7,  3
   // char bit  4 -> double_size_map + 1 bits 31, 27
   // ...
   // char bit  7 -> double_size_map + 1 bits  7,  3
   // char bit  8 -> double_size_map + 0 bits 31, 27
   // ...
   // char bit 11 -> double_size_map + 0 bits  7,  3
   memset(normal_size_map, 0, sizeof(normal_size_map));
   memset(double_size_map, 0, sizeof(double_size_map));
   for (int i = 0; i < NLINES; i++) {
      attributes[i] = 0;
   }
   for (int i = 0; i <= 0xFFF; i++) {
      for (int j = 0; j < 12; j++) {
         // j is the pixel font data bit, with bit 11 being left most
         if (i & (1 << j)) {
            // Normal size, odd characters
            // cccc.... dddddddd
            if (j < 8) {
               normal_size_map[i * 4 + 3] |= 0x8 << (4 * (7 - (j ^ 1)));   // dddddddd
            } else {
                  normal_size_map[i * 4 + 2] |= 0x8 << (4 * (15 - (j ^ 1)));  // cccc....
            }
            // Normal size, even characters
            // aaaaaaaa ....bbbb
            if (j < 4) {
               normal_size_map[i * 4 + 1] |= 0x8 << (4 * (3 - (j ^ 1)));   // ....bbbb
            } else {
               normal_size_map[i * 4    ] |= 0x8 << (4 * (11 - (j ^ 1)));  // aaaaaaaa
            }
            // Double size
            // aaaaaaaa bbbbbbbb cccccccc
            if (j < 4) {
               double_size_map[i * 3 + 2] |= 0x88 << (8 * (3 - j));  // cccccccc
            } else if (j < 8) {
               double_size_map[i * 3 + 1] |= 0x88 << (8 * (7 - j));  // bbbbbbbb
            } else {
               double_size_map[i * 3    ] |= 0x88 << (8 * (11 - j)); // aaaaaaaa
            }
         }
      }
   }
}

void osd_clear() {
   if (active) {
      active = 0;
      RPI_PropertyInit();
      RPI_PropertyAddTag( TAG_SET_PALETTE );
      RPI_PropertyProcess();
      memset(buffer, 0, sizeof(buffer));
   }
   osd_update((uint32_t *)fb, pitch);
   osd_update((uint32_t *)(fb + SCREEN_HEIGHT * pitch), pitch);
}

void osd_set(int line, int attr, char *text) {
   if (!active) {
      active = 1;
      RPI_PropertyInit();
      RPI_PropertyAddTag( TAG_SET_PALETTE );
      RPI_PropertyProcess();
   }
   attributes[line] = attr;
   memset(buffer + line * LINELEN, 0, LINELEN);
   int len = strlen(text);
   if (len > LINELEN) {
      len = LINELEN;
   }
   strncpy(buffer + line * LINELEN, text, len);
   osd_update((uint32_t *)fb, pitch);
   osd_update((uint32_t *)(fb + SCREEN_HEIGHT * pitch), pitch);
}


int osd_active() {
   return active;
}


enum {
   IDLE,
   MANUAL
};


static void show_param(int num) {
   param_t *params = cpld->get_params();
   sprintf(message, "%s = %d", params[num].name, cpld->get_value(num));
   osd_set(1, 0, message);
}

void osd_key(int key) {
   static int osd_state = IDLE;
   static int param_num = 0;
   static int mux = 0;

   int value;
   param_t *params = cpld->get_params();

   switch (osd_state) {

   case IDLE:
      switch (key) {
      case OSD_SW1:
         osd_set(0, ATTR_DOUBLE_SIZE, "Manual Calibration");
         param_num = 0;
         show_param(param_num);
         osd_state = MANUAL;
         break;
      case OSD_SW2:
         mux = 1 - mux;
         RPI_SetGpioValue(MUX_PIN, mux);
         sprintf(message, "Input Mux = %d", mux);
         osd_set(0, ATTR_DOUBLE_SIZE, message);
         for (volatile int i = 0; i < 100000000; i++);
         osd_clear();
         break;
      case OSD_SW3:
         osd_set(0, ATTR_DOUBLE_SIZE, "Auto Calibration");
         action_calibrate();
         osd_clear();
         break;
      }
      break;

   case MANUAL:
      switch (key) {
      case OSD_SW1:
         // exit manual configuration
         osd_state = IDLE;
         osd_clear();
         break;
      case OSD_SW2:
         // next param
         param_num++;
         if (params[param_num].name == NULL) {
            param_num = 0;
         }
         show_param(param_num);
         break;
      case OSD_SW3:
         // next value
         value = cpld->get_value(param_num);
         if (value < params[param_num].max) {
            value++;
         } else {
            value = params[param_num].min;
         }
         cpld->set_value(param_num, value);
         show_param(param_num);
         break;
      }
      break;
   }
}

void osd_update(uint32_t *osd_base, int bytes_per_line) {
   // SAA5050 character data is 12x20
   uint32_t *line_ptr = osd_base;
   int words_per_line = bytes_per_line >> 2;
   for (int line = 0; line < NLINES; line++) {
      int attr = attributes[line];
      int len = (attr & ATTR_DOUBLE_SIZE) ? (LINELEN >> 1) : LINELEN;
      for (int y = 0; y < 20; y++) {
         uint32_t *word_ptr = line_ptr;
         for (int i = 0; i < len; i++) {
            int c = buffer[line * LINELEN + i];
            // Deal with unprintable characters
            if (c < 32 || c > 127) {
               c = 32;
            }
            // Character row is 12 pixels
            int data = fontdata[32 * c + y] & 0x3ff;
            // Map to the screen pixel format
            if (attr & ATTR_DOUBLE_SIZE) {
               // Map to three 32-bit words in frame buffer format
               uint32_t *map_ptr = double_size_map + data * 3;
               *word_ptr &= 0x77777777;
               *word_ptr |= *map_ptr;
               *(word_ptr + words_per_line) &= 0x77777777;;
               *(word_ptr + words_per_line) |= *map_ptr;
               word_ptr++;
               map_ptr++;
               *word_ptr &= 0x77777777;
               *word_ptr |= *map_ptr;
               *(word_ptr + words_per_line) &= 0x77777777;;
               *(word_ptr + words_per_line) |= *map_ptr;
               word_ptr++;
               map_ptr++;
               *word_ptr &= 0x77777777;
               *word_ptr |= *map_ptr;
               *(word_ptr + words_per_line) &= 0x77777777;;
               *(word_ptr + words_per_line) |= *map_ptr;
               word_ptr++;
            } else {
               // Map to two 32-bit words in frame buffer format
               if (i & 1) {
                  // odd character
                  uint32_t *map_ptr = normal_size_map + (data << 2) + 2;
                  *word_ptr &= 0x7777FFFF;
                  *word_ptr |= *map_ptr;
                  word_ptr++;
                  map_ptr++;
                  *word_ptr &= 0x77777777;
                  *word_ptr |= *map_ptr;
                  word_ptr++;
               } else {
                  // even character
                  uint32_t *map_ptr = normal_size_map + (data << 2);
                  *word_ptr &= 0x77777777;
                  *word_ptr |= *map_ptr;
                  word_ptr++;
                  map_ptr++;
                  *word_ptr &= 0xFFFF7777;
                  *word_ptr |= *map_ptr;
               }
            }
         }
         if (attr & ATTR_DOUBLE_SIZE) {
            line_ptr += 2 * words_per_line;
         } else {
            line_ptr += words_per_line;
         }
      }
   }
}

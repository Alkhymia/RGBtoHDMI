#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "defs.h"
#include "cpld.h"
#include "gitversion.h"
#include "info.h"
#include "logging.h"
#include "osd.h"
#include "rpi-gpio.h"
#include "rpi-mailbox.h"
#include "rpi-mailbox-interface.h"
#include "saa5050_font.h"

// =============================================================
// Definitions for the size of the OSD
// =============================================================

#define NLINES         12

#define LINELEN        40

#define MAX_MENU_DEPTH  4

// =============================================================
// Main states that the OSD can be in
// =============================================================

typedef enum {
   IDLE,  // No menu
   MENU,  // Browsing a menu
   PARAM, // Changing the value of a menu item
   INFO   // Viewing an info panel
} osd_state_t;

// =============================================================
// Friently names for certain OSD feature values
// =============================================================

static const char *palette_names[] = {
   "Default",
   "Inverse",
   "Mono 1",
   "Mono 2",
   "Just Red",
   "Just Green",
   "Just Blue",
   "Not Red",
   "Not Green",
   "Not Blue"
};

static const char *pllh_names[] = {
   "Original",
   "623",
   "624",
   "625",
   "626",
   "627",
};

static const char *deinterlace_names[] = {
   "None",
   "Motion Adaptive 1",
   "Motion Adaptive 2",
   "Motion Adaptive 3",
   "Motion Adaptive 4",
   "Aligned Motion Adaptive 1",
   "Aligned Motion Adaptive 2",
   "Aligned Motion Adaptive 3",
   "Aligned Motion Adaptive 4"
};

#ifdef MULTI_BUFFER
static const char *nbuffer_names[] = {
   "Single buffered",
   "Double buffered",
   "Triple buffered",
   "Quadruple buffered",
};
#endif

// =============================================================
// Feature definitions
// =============================================================

enum {
   F_PALETTE,
   F_SCANLINES,
   F_ELK,
   F_DEINTERLACE,
   F_VSYNC,
   F_MUX,
   F_PLLH,
#ifdef MULTI_BUFFER
   F_NBUFFERS,
#endif
   F_M7DISABLE,
   F_DEBUG
};

static param_t features[] = {
   {     F_PALETTE,       "Palette", 0,     NUM_PALETTES - 1 },
   {   F_SCANLINES,     "Scanlines", 0,                    1 },
   {         F_ELK,           "Elk", 0,                    1 },
   { F_DEINTERLACE,   "Deinterlace", 0, NUM_DEINTERLACES - 1 },
   {       F_VSYNC,         "Vsync", 0,                    1 },
   {         F_MUX,     "Input Mux", 0,                    1 },
   {        F_PLLH,    "HDMI Clock", 0,                    5 },
#ifdef MULTI_BUFFER
   {    F_NBUFFERS,   "Num Buffers", 0,                    3 },
#endif
   {   F_M7DISABLE, "Mode7 Disable", 0,                    1 },
   {       F_DEBUG,         "Debug", 0,                    1 },
   {            -1,            NULL, 0,                    0 },
};

// =============================================================
// Menu definitions
// =============================================================


typedef enum {
   I_MENU,    // Item points to a sub-menu
   I_FEATURE, // Item is a "feature" (i.e. managed by the osd)
   I_PARAM,   // Item is a "parameter" (i.e. managed by the cpld)
   I_INFO,    // Item is an info screen
   I_BACK     // Item is a link back to the previous menu
} item_type_t;

typedef struct {
   item_type_t       type;
} base_menu_item_t;

typedef struct menu {
   char *name;
   base_menu_item_t *items[];
} menu_t;

typedef struct {
   item_type_t       type;
   menu_t           *child;
   void            (*rebuild)(menu_t *menu);
} child_menu_item_t;

typedef struct {
   item_type_t       type;
   param_t          *param;
} param_menu_item_t;

typedef struct {
   item_type_t       type;
   char             *name;
   void            (*show_info)(int line);
} info_menu_item_t;

typedef struct {
   item_type_t       type;
   char             *name;
} back_menu_item_t;

static void info_firmware_version(int line);
static void info_cal_summary(int line);
static void info_cal_detail(int line);
static void info_cal_raw(int line);

static info_menu_item_t firmware_version_ref = { I_INFO, "Firmware Version",    info_firmware_version};
static info_menu_item_t cal_summary_ref      = { I_INFO, "Calibration Summary", info_cal_summary};
static info_menu_item_t cal_detail_ref       = { I_INFO, "Calibration Detail",  info_cal_detail};
static info_menu_item_t cal_raw_ref          = { I_INFO, "Calibration Raw",     info_cal_raw};
static back_menu_item_t back_ref             = { I_BACK, "Return"};

static menu_t info_menu = {
   "Info Menu",
   {
      (base_menu_item_t *) &firmware_version_ref,
      (base_menu_item_t *) &cal_summary_ref,
      (base_menu_item_t *) &cal_detail_ref,
      (base_menu_item_t *) &cal_raw_ref,
      (base_menu_item_t *) &back_ref,
      NULL
   }
};

static param_menu_item_t palette_ref     = { I_FEATURE, &features[F_PALETTE]     };
static param_menu_item_t scanlines_ref   = { I_FEATURE, &features[F_SCANLINES]   };
static param_menu_item_t elk_ref         = { I_FEATURE, &features[F_ELK]         };
static param_menu_item_t deinterlace_ref = { I_FEATURE, &features[F_DEINTERLACE] };
static param_menu_item_t vsync_ref       = { I_FEATURE, &features[F_VSYNC]       };
static param_menu_item_t mux_ref         = { I_FEATURE, &features[F_MUX]         };
static param_menu_item_t pllh_ref        = { I_FEATURE, &features[F_PLLH]        };
#ifdef MULTI_BUFFER
static param_menu_item_t nbuffers_ref    = { I_FEATURE, &features[F_NBUFFERS]    };
#endif
static param_menu_item_t m7disable_ref   = { I_FEATURE, &features[F_M7DISABLE]   };
static param_menu_item_t debug_ref       = { I_FEATURE, &features[F_DEBUG]       };

static menu_t processing_menu = {
   "Processing Menu",
   {
      (base_menu_item_t *) &deinterlace_ref,
      (base_menu_item_t *) &palette_ref,
      (base_menu_item_t *) &scanlines_ref,
      (base_menu_item_t *) &back_ref,
      NULL
   }
};


static menu_t settings_menu = {
   "Settings Menu",
   {
      (base_menu_item_t *) &elk_ref,
      (base_menu_item_t *) &mux_ref,
      (base_menu_item_t *) &debug_ref,
      (base_menu_item_t *) &m7disable_ref,
      (base_menu_item_t *) &vsync_ref,
      (base_menu_item_t *) &pllh_ref,
      (base_menu_item_t *) &nbuffers_ref,
      (base_menu_item_t *) &back_ref,
      NULL
   }
};

static param_menu_item_t dynamic_item[] = {
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL },
   { I_PARAM, NULL }
};

static menu_t geometry_menu = {
   "Geometry Menu",
   {
      // Allow space for max 10 params
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL
   }
};

static menu_t sampling_menu = {
   "Sampling Menu",
   {
      // Allow space for max 10 params
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL
   }
};

static void rebuild_geometry_menu(menu_t *menu);
static void rebuild_sampling_menu(menu_t *menu);

static child_menu_item_t info_menu_ref        = { I_MENU, &info_menu       , NULL};
static child_menu_item_t processing_menu_ref  = { I_MENU, &processing_menu , NULL};
static child_menu_item_t settings_menu_ref    = { I_MENU, &settings_menu   , NULL};
static child_menu_item_t geometry_menu_ref    = { I_MENU, &geometry_menu   , rebuild_geometry_menu};
static child_menu_item_t sampling_menu_ref    = { I_MENU, &sampling_menu   , rebuild_sampling_menu};

static menu_t main_menu = {
   "Main Menu",
   {
      (base_menu_item_t *) &info_menu_ref,
      (base_menu_item_t *) &processing_menu_ref,
      (base_menu_item_t *) &settings_menu_ref,
      (base_menu_item_t *) &geometry_menu_ref,
      (base_menu_item_t *) &sampling_menu_ref,
      (base_menu_item_t *) &back_ref,
      NULL
   }
};

// =============================================================
// Static local variables
// =============================================================

static char buffer[LINELEN * NLINES];

static int attributes[NLINES];

// Mapping table for expanding 12-bit row to 24 bit pixel (3 words)
static uint32_t double_size_map[0x1000 * 3];

// Mapping table for expanding 12-bit row to 12 bit pixel (2 words + 2 words)
static uint32_t normal_size_map[0x1000 * 4];

// Temporary buffer for assembling OSD lines
static char message[80];

// Is the OSD currently active
static int active = 0;

// Main state of the OSD
osd_state_t osd_state;

// Current menu depth
static int depth = 0;

// Currently active menu
static menu_t *current_menu[MAX_MENU_DEPTH];

// Index to the currently selected menu
static int current_item[MAX_MENU_DEPTH];

// Currently selected palette setting
static int palette   = PALETTE_DEFAULT;

// Currently selected input mux setting
static int mux       = 0;

// =============================================================
// Private Methods
// =============================================================

static void update_palette() {
   // Flush the previous swapBuffer() response from the GPU->ARM mailbox
   RPI_Mailbox0Flush( MB0_TAGS_ARM_TO_VC  );
   RPI_PropertyInit();
   RPI_PropertyAddTag( TAG_SET_PALETTE, osd_get_palette());
   RPI_PropertyProcess();
}

static void delay() {
   for (volatile int i = 0; i < 100000000; i++);
}

static int get_feature(int num) {
   switch (num) {
   case F_PALETTE:
      return palette;
   case F_DEINTERLACE:
      return get_deinterlace();
   case F_SCANLINES:
      return get_scanlines();
   case F_MUX:
      return mux;
   case F_ELK:
      return get_elk();
   case F_VSYNC:
      return get_vsync();
   case F_PLLH:
      return get_pllh();
#ifdef MULTI_BUFFER
   case F_NBUFFERS:
      return get_nbuffers();
#endif
   case F_DEBUG:
      return get_debug();
   case F_M7DISABLE:
      return get_m7disable();
   }
   return -1;
}

static void set_feature(int num, int value) {
   switch (num) {
   case F_PALETTE:
      palette = value;
      update_palette();
      break;
   case F_DEINTERLACE:
      set_deinterlace(value);
      break;
   case F_SCANLINES:
      set_scanlines(value);
      break;
   case F_MUX:
      mux = value;
      RPI_SetGpioValue(MUX_PIN, mux);
      break;
   case F_ELK:
      set_elk(value);
      break;
   case F_VSYNC:
      set_vsync(value);
      break;
   case F_PLLH:
      set_pllh(value);
      break;
#ifdef MULTI_BUFFER
   case F_NBUFFERS:
      set_nbuffers(value);
      break;
#endif
   case F_DEBUG:
      set_debug(value);
      update_palette();
      break;
   case F_M7DISABLE:
      set_m7disable(value);
      break;
   }
}

// Wrapper to extract the name of a menu item
static const char *item_name(base_menu_item_t *item) {
   switch (item->type) {
   case I_MENU:
      return ((child_menu_item_t *)item)->child->name;
   case I_FEATURE:
   case I_PARAM:
      return ((param_menu_item_t *)item)->param->name;
   case I_INFO:
      return ((info_menu_item_t *)item)->name;
   case I_BACK:
      return ((back_menu_item_t *)item)->name;
   default:
      // Should never hit this case
      return NULL;
   }
}

// Test if a parameter is a simple boolean
static int is_boolean_param(param_menu_item_t *param_item) {
   return param_item->param->min == 0 && param_item->param->max == 1;
}

// Set wrapper to abstract different between I_PARAM and I_FEATURE
static void set_param(param_menu_item_t *param_item, int value) {
   item_type_t type = param_item->type;
   if (type == I_FEATURE) {
      set_feature(param_item->param->key, value);
   } else {
      cpld->set_value(param_item->param->key, value);
   }
}

// Get wrapper to abstract different between I_PARAM and I_FEATURE
static int get_param(param_menu_item_t *param_item) {
   item_type_t type = param_item->type;
   if (type == I_FEATURE) {
      return get_feature(param_item->param->key);
   } else {
      return cpld->get_value(param_item->param->key);
   }
}

static void toggle_boolean_param(param_menu_item_t *param_item) {
   set_param(param_item, get_param(param_item) ^ 1);
}

static const char *get_param_string(param_menu_item_t *param_item) {
   static char number[16];
   item_type_t type = param_item->type;
   param_t   *param = param_item->param;
   // Read the current value of the specified feature
   int value = get_param(param_item);
   // Convert certain features to human readable strings
   if (type == I_FEATURE) {
      switch (param->key) {
      case F_PALETTE:
         return palette_names[value];
      case F_DEINTERLACE:
         return deinterlace_names[value];
      case F_PLLH:
         return pllh_names[value];
#ifdef MULTI_BUFFER
      case F_NBUFFERS:
         return nbuffer_names[value];
#endif
      }
   }
   if (is_boolean_param(param_item)) {
      return value ? "On" : "Off";
   }
   sprintf(number, "%d", value);
   return number;
}

static void info_firmware_version(int line) {
   sprintf(message, "Pi Firmware: %s", GITVERSION);
   osd_set(line, 0, message);
   sprintf(message, "%s CPLD: v%x.%x",
           cpld->name,
           (cpld->get_version() >> VERSION_MAJOR_BIT) & 0xF,
           (cpld->get_version() >> VERSION_MINOR_BIT) & 0xF);
   osd_set(line + 1, 0, message);
}

static void info_cal_summary(int line) {
   const char *machine = get_elk() ? "Elk" : "Beeb";
   if (clock_error_ppm > 0) {
      sprintf(message, "Clk Err: %d ppm (%s slower than Pi)", clock_error_ppm, machine);
   } else if (clock_error_ppm < 0) {
      sprintf(message, "Clk Err: %d ppm (%s faster than Pi)", -clock_error_ppm, machine);
   } else {
      sprintf(message, "Clk Err: %d ppm (exact match)", clock_error_ppm);
   }
   osd_set(line, 0, message);
   if (cpld->show_cal_summary) {
      cpld->show_cal_summary(line + 2);
   } else {
      sprintf(message, "show_cal_summary() not implemented");
      osd_set(line + 2, 0, message);
   }
}

static void info_cal_detail(int line) {
   if (cpld->show_cal_details) {
      cpld->show_cal_details(line);
   } else {
      sprintf(message, "show_cal_details() not implemented");
      osd_set(line, 0, message);
   }
}

static void info_cal_raw(int line) {
   if (cpld->show_cal_raw) {
      cpld->show_cal_raw(line);
   } else {
      sprintf(message, "show_cal_raw() not implemented");
      osd_set(line, 0, message);
   }
}

static void rebuild_menu(menu_t *menu, param_t *param_ptr) {
   int i = 0;
   while (param_ptr->name) {
      dynamic_item[i].param = param_ptr;
      menu->items[i] = (base_menu_item_t *)&dynamic_item[i];
      param_ptr++;
      i++;
   }
   menu->items[i] = (base_menu_item_t *)&back_ref;

}
static void rebuild_geometry_menu(menu_t *menu) {
   rebuild_menu(menu, cpld->get_geometry_params());
}

static void rebuild_sampling_menu(menu_t *menu) {
   rebuild_menu(menu, cpld->get_sampling_params());
}

static void redraw_menu() {
   menu_t *menu = current_menu[depth];
   int current = current_item[depth];
   int line = 0;
   base_menu_item_t **item_ptr;
   base_menu_item_t *item;
   if (osd_state == INFO) {
      item = menu->items[current];
      // We should always be on an INFO item...
      if (item->type == I_INFO) {
         info_menu_item_t *info_item = (info_menu_item_t *)item;
         osd_set(line, ATTR_DOUBLE_SIZE, info_item->name);
         line += 2;
         info_item->show_info(line);
      }
   } else {
      osd_set(line, ATTR_DOUBLE_SIZE, menu->name);
      line += 2;
      // Work out the longest item name
      int max = 0;
      item_ptr = menu->items;
      while ((item = *item_ptr++)) {
         int len = strlen(item_name(item));
         if (len > max) {
            max = len;
         }
      }
      log_info("max = %d", max);
      item_ptr = menu->items;
      int i = 0;
      while ((item = *item_ptr++)) {
         char *mp         = message;
         char sel_none    = ' ';
         char sel_open    = (i == current) ? ']' : sel_none;
         char sel_close   = (i == current) ? '[' : sel_none;
         const char *name = item_name(item);
         *mp++ = (osd_state != PARAM) ? sel_open : sel_none;
         strcpy(mp, name);
         mp += strlen(mp);
         if ((item)->type == I_FEATURE || (item)->type == I_PARAM) {
            int len = strlen(name);
            while (len < max) {
               *mp++ = ' ';
               len++;
            }
            *mp++ = ' ';
            *mp++ = '=';
            *mp++ = (osd_state == PARAM) ? sel_open : sel_none;
            strcpy(mp, get_param_string((param_menu_item_t *)item));
            mp += strlen(mp);
         }
         *mp++ = sel_close;
         *mp++ = '\0';
         osd_set(line++, 0, message);
         i++;
      }
   }
}

// =============================================================
// Public Methods
// =============================================================

uint32_t *osd_get_palette() {
   int m;
   static uint32_t palette_data[16];
   for (int i = 0; i < 16; i++) {
      int r = (i & 1) ? 255 : 0;
      int g = (i & 2) ? 255 : 0;
      int b = (i & 4) ? 255 : 0;
      switch (palette) {
      case PALETTE_INVERSE:
         r = 255 - r;
         g = 255 - g;
         b = 255 - b;
         break;
      case PALETTE_MONO1:
         m = 0.299 * r + 0.587 * g + 0.114 * b;
         r = m; g = m; b = m;
         break;
      case PALETTE_MONO2:
         m = (i & 7) * 255 / 7;
         r = m; g = m; b = m;
         break;
      case PALETTE_RED:
         m = (i & 7) * 255 / 7;
         r = m; g = 0; b = 0;
         break;
      case PALETTE_GREEN:
         m = (i & 7) * 255 / 7;
         r = 0; g = m; b = 0;
         break;
      case PALETTE_BLUE:
         m = (i & 7) * 255 / 7;
         r = 0; g = 0; b = m;
         break;
      case PALETTE_NOT_RED:
         r = 0;
         g = (i & 3) * 255 / 3;
         b = ((i >> 2) & 1) * 255;
         break;
      case PALETTE_NOT_GREEN:
         r = (i & 3) * 255 / 3;
         g = 0;
         b = ((i >> 2) & 1) * 255;
         break;
      case PALETTE_NOT_BLUE:
         r = ((i >> 2) & 1) * 255;
         g = (i & 3) * 255 / 3;
         b = 0;
         break;
      }
      if (active) {
         if (i >= 8) {
            palette_data[i] = 0xFFFFFFFF;
         } else {
            r >>= 1; g >>= 1; b >>= 1;
            palette_data[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
         }
      } else {
         palette_data[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
      }
      if (get_debug()) {
         palette_data[i] |= 0x00101010;
      }
   }
   return palette_data;
}

void osd_clear() {
   if (active) {
      memset(buffer, 0, sizeof(buffer));
      osd_update((uint32_t *)capinfo->fb, capinfo->pitch);
      active = 0;
      RPI_SetGpioValue(LED1_PIN, active);
      update_palette();
   }
}

void osd_set(int line, int attr, char *text) {
   if (!active) {
      active = 1;
      RPI_SetGpioValue(LED1_PIN, active);
      update_palette();
   }
   attributes[line] = attr;
   memset(buffer + line * LINELEN, 0, LINELEN);
   int len = strlen(text);
   if (len > LINELEN) {
      len = LINELEN;
   }
   strncpy(buffer + line * LINELEN, text, len);
   osd_update((uint32_t *)capinfo->fb, capinfo->pitch);
}

int osd_active() {
   return active;
}

void osd_refresh() {
   osd_clear();
   if (osd_state != IDLE) {
      redraw_menu();
   }
}

void osd_key(int key) {
   item_type_t type;
   base_menu_item_t *item = current_menu[depth]->items[current_item[depth]];
   child_menu_item_t *child_item = (child_menu_item_t *)item;
   param_menu_item_t *param_item = (param_menu_item_t *)item;
   int val;

   switch (osd_state) {

   case IDLE:
      switch (key) {
      case OSD_SW1:
         // Enter
         osd_state = MENU;
         current_menu[depth] = &main_menu;
         current_item[depth] = 0;
         redraw_menu();
         break;
      case OSD_SW2:
         // NOOP
         break;
      case OSD_SW3:
         // Auto Calibration
         osd_set(0, ATTR_DOUBLE_SIZE, "Auto Calibration");
         action_calibrate();
         delay();
         osd_clear();
         break;
      }
      break;

   case MENU:
      type = item->type;
      switch (key) {
      case OSD_SW1:
         // ENTER
         switch (type) {
         case I_MENU:
            depth++;
            current_menu[depth] = child_item->child;
            current_item[depth] = 0;
            // Rebuild dynamically populated menus, e.g. the sampling and geometry menus that are mode specific
            if (child_item->rebuild) {
               child_item->rebuild(child_item->child);
            }
            osd_clear();
            redraw_menu();
            break;
         case I_FEATURE:
         case I_PARAM:
            if (is_boolean_param(param_item)) {
               // If it's a boolean item, then just toggle it
               toggle_boolean_param(param_item);
            } else {
               // If not then move to the parameter editing state
               osd_state = PARAM;
            }
            redraw_menu();
            break;
         case I_INFO:
            osd_state = INFO;
            osd_clear();
            redraw_menu();
            break;
         case I_BACK:
            osd_clear();
            if (depth == 0) {
               osd_state = IDLE;
            } else {
               depth--;
               redraw_menu();
            }
            break;
         }
         break;
      case OSD_SW2:
         // PREVIOUS
         if (current_item[depth] > 0) {
            current_item[depth]--;
            redraw_menu();
         }
        break;
      case OSD_SW3:
         // NEXT
         if (current_menu[depth]->items[current_item[depth] + 1] != NULL) {
            current_item[depth]++;
            redraw_menu();
         }
         break;
      }
      break;

   case PARAM:
      type = item->type;
      switch (key) {
      case OSD_SW1:
         // ENTER
         osd_state = MENU;
         break;
      case OSD_SW2:
         // PREVIOUS
         val = get_param(param_item);
         if (val > param_item->param->min) {
            val--;
            set_param(param_item, val);
         }
         break;
      case OSD_SW3:
         // NEXT
         val = get_param(param_item);
         if (val < param_item->param->max) {
            val++;
            set_param(param_item, val);
         }
         break;
      }
      redraw_menu();
      break;

   case INFO:
      switch (key) {
      case OSD_SW1:
         // ENTER
         osd_state = MENU;
         osd_clear();
         redraw_menu();
         break;
      }
      break;
   }
}

void osd_init() {
   char *prop;
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
   // Initialize the OSD features
   prop = get_cmdline_prop("palette");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_PALETTE, val);
      log_info("config.txt:     palette = %d", val);
   }
   prop = get_cmdline_prop("deinterlace");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_DEINTERLACE, val);
      log_info("config.txt: deinterlace = %d", val);
   }
   prop = get_cmdline_prop("scanlines");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_SCANLINES, val);
      log_info("config.txt:   scanlines = %d", val);
   }
   prop = get_cmdline_prop("mux");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_MUX, val);
      log_info("config.txt:         mux = %d", val);
   }
   prop = get_cmdline_prop("elk");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_ELK, val);
      log_info("config.txt:         elk = %d", val);
   }
   prop = get_cmdline_prop("pllh");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_PLLH, val);
      log_info("config.txt:        pllh = %d", val);
   }
#ifdef MULTI_BUFFER
   prop = get_cmdline_prop("nbuffers");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_NBUFFERS, val);
      log_info("config.txt:    nbuffers = %d", val);
   }
#endif
   prop = get_cmdline_prop("vsync");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_VSYNC, val);
      log_info("config.txt:       vsync = %d", val);
   }
   prop = get_cmdline_prop("debug");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_DEBUG, val);
      log_info("config.txt:       debug = %d", val);
   }
   prop = get_cmdline_prop("m7disable");
   if (prop) {
      int val = atoi(prop);
      set_feature(F_M7DISABLE, val);
      log_info("config.txt:   m7disable = %d", val);
   }
   // Initialize the CPLD sampling points
   for (int p = 0; p < 2; p++) {
      for (int m7 = 0; m7 <= 1; m7++) {
         char *propname;
         if (p == 0) {
            propname = m7 ? "sampling7" : "sampling06";
         } else {
            propname = m7 ? "geometry7" : "geometry06";
         }
         prop = get_cmdline_prop(propname);
         if (prop) {
            cpld->set_mode(m7);
            log_info("config.txt:  %s = %s", propname, prop);
            char *prop2 = strtok(prop, ",");
            int i = 0;
            while (prop2) {
               param_t *param;
               if (p == 0) {
                  param = cpld->get_sampling_params() + i;
               } else {
                  param = cpld->get_geometry_params() + i;
               }
               if (param->key < 0) {
                  log_warn("Too many sampling sub-params, ignoring the rest");
                  break;
               }
               int val = atoi(prop2);
               log_info("cpld: %s = %d", param->name, val);
               cpld->set_value(param->key, val);
               prop2 = strtok(NULL, ",");
               i++;
            }
         }
      }
   }
   // Disable CPLDv2 specific features for CPLDv1
   if (((cpld->get_version() >> VERSION_MAJOR_BIT) & 0x0F) < 2) {
      features[F_DEINTERLACE].max = DEINTERLACE_MA4;
   }
}

void osd_update(uint32_t *osd_base, int bytes_per_line) {
   if (!active) {
      return;
   }
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

// This is a stripped down version of the above that is significantly
// faster, but assumes all the osd pixel bits are initially zero.
//
// This is used in mode 0..6, and is called by the rgb_to_fb code
// after the RGB data has been written into the frame buffer.
//
// It's a shame we have had to duplicate code here, but speed matters!

void osd_update_fast(uint32_t *osd_base, int bytes_per_line) {
   if (!active) {
      return;
   }
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
            // Bail at the first zero character
            if (c == 0) {
               break;
            }
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
               *word_ptr |= *map_ptr;
               *(word_ptr + words_per_line) |= *map_ptr;
               word_ptr++;
               map_ptr++;
               *word_ptr |= *map_ptr;
               *(word_ptr + words_per_line) |= *map_ptr;
               word_ptr++;
               map_ptr++;
               *word_ptr |= *map_ptr;
               *(word_ptr + words_per_line) |= *map_ptr;
               word_ptr++;
            } else {
               // Map to two 32-bit words in frame buffer format
               if (i & 1) {
                  // odd character
                  uint32_t *map_ptr = normal_size_map + (data << 2) + 2;
                  *word_ptr |= *map_ptr;
                  word_ptr++;
                  map_ptr++;
                  *word_ptr |= *map_ptr;
                  word_ptr++;
               } else {
                  // even character
                  uint32_t *map_ptr = normal_size_map + (data << 2);
                  *word_ptr |= *map_ptr;
                  word_ptr++;
                  map_ptr++;
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

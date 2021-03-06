#include <stdio.h>
#include "geometry.h"

enum {
   H_OFFSET,
   V_OFFSET,
   H_WIDTH,
   V_HEIGHT,
   FB_WIDTH,
   FB_HEIGHT,
   FB_BPP,
   CLOCK,
   LINE_LEN,
   CLOCK_PPM,
   PX_SAMPLING
};

static const char *px_sampling_names[] = {
   "Normal",
   "Subsample Even",
   "Subsample Odd",
   "Pixel Double"
};

static param_t params[] = {
   {    H_OFFSET,        "H offset",         0,        59, 1 },
   {    V_OFFSET,        "V offset",         0,        39, 1 },
   {     H_WIDTH,         "H width",         1,       100, 1 },
   {    V_HEIGHT,        "V height",         1,       300, 1 },
   {    FB_WIDTH,        "FB width",       400,       800, 1 },
   {   FB_HEIGHT,       "FB height",       320,       600, 1 },
   {      FB_BPP,   "FB bits/pixel",         4,         8, 4 },
   {       CLOCK,      "Clock freq",  75000000, 100000000, 1 },
   {    LINE_LEN,     "Line length",      1000,      9999, 1 },
   {   CLOCK_PPM, "Clock tolerance",         0,    100000, 1 },
   { PX_SAMPLING,  "Pixel sampling",         0,  NUM_PS-1, 1 },
   {          -1,              NULL,         0,         0, 0 }
};

typedef struct {
   int h_offset;      // horizontal offset (in psync clocks)
   int v_offset;      // vertical offset (in lines)
   int h_width;       // active horizontal width (in 8-bit characters)
   int v_height;      // active vertical height (in lines)
   int fb_width;      // framebuffer width in pixels
   int fb_height;     // framebuffer height in pixels
   int fb_bpp;        // framebuffer bits per pixel
   int clock;         // cpld clock (in Hz)
   int line_len;      // number of clocks per horizontal line
   int clock_ppm;     // cpld tolerance (in ppm)
   int px_sampling;   // pixel sampling mode
} geometry_t;

static int mode7;
static geometry_t *geometry;
static geometry_t default_geometry;
static geometry_t mode7_geometry;

static void update_param_range() {
   int max;
   // Set the range of the H_WIDTH param based on FB_WIDTH
   max = geometry->fb_width / (32 / geometry->fb_bpp);
   params[H_WIDTH].max = max;
   if (geometry->h_width > max) {
      geometry->h_width = max;
   }
   // Set the range of the V_HEIGHT param based on FB_HEIGHT
   max = geometry->fb_height / 2;
   params[V_HEIGHT].max = max;
   if (geometry->v_height > max) {
      geometry->v_height = max;
   }
}

void geometry_init(int version) {
   // These are Beeb specific defaults so the geometry property can be ommitted
   mode7_geometry.h_offset      =        24;
   mode7_geometry.v_offset      =        21;
   mode7_geometry.h_width       =  504 / (32 / 4);
   mode7_geometry.v_height      =       270;
   mode7_geometry.fb_width      =       504;
   mode7_geometry.fb_height     =       540;
   mode7_geometry.fb_bpp        =         4;
   mode7_geometry.clock         =  96000000;
   mode7_geometry.line_len      =   96 * 64;
   mode7_geometry.clock_ppm     =      5000;
   mode7_geometry.px_sampling   = PS_NORMAL;
   default_geometry.h_offset    =        32;
   default_geometry.v_offset    =        21;
   default_geometry.h_width     =  672 / (32 / 4);
   default_geometry.v_height    =       270;
   default_geometry.fb_width    =       672;
   default_geometry.fb_height   =       540;
   default_geometry.fb_bpp      =         4;
   default_geometry.clock       =  96000000;
   default_geometry.line_len    =   96 * 64;
   default_geometry.clock_ppm   =      5000;
   default_geometry.px_sampling = PS_NORMAL;
   // For backwards compatibility with CPLDv1
   int supports_delay = (((version >> VERSION_DESIGN_BIT) & 0x0F) == 0) &&
                        (((version >> VERSION_MAJOR_BIT ) & 0x0F) >= 2);
   if (!supports_delay) {
      mode7_geometry.h_offset = 0;
      default_geometry.h_offset = 0;
   }
   geometry_set_mode(0);
}

void geometry_set_mode(int mode) {
   mode7 = mode;
   geometry = mode ? &mode7_geometry : &default_geometry;
   update_param_range();

}

int geometry_get_value(int num) {
   switch (num) {
   case H_OFFSET:
      return geometry->h_offset;
   case V_OFFSET:
      return geometry->v_offset;
   case H_WIDTH:
      return geometry->h_width;
   case V_HEIGHT:
      return geometry->v_height;
   case FB_WIDTH:
      return geometry->fb_width;
   case FB_HEIGHT:
      return geometry->fb_height;
   case FB_BPP:
      return geometry->fb_bpp;
   case CLOCK:
      return geometry->clock;
   case LINE_LEN:
      return geometry->line_len;
   case CLOCK_PPM:
      return geometry->clock_ppm;
   case PX_SAMPLING:
      return geometry->px_sampling;
   }
   return -1;
}

const char *geometry_get_value_string(int num) {
   if (num == PX_SAMPLING) {
      return px_sampling_names[geometry_get_value(num)];
   }
   return NULL;
}

void geometry_set_value(int num, int value) {
   switch (num) {
   case H_OFFSET:
      geometry->h_offset = value;
      break;
   case V_OFFSET:
      geometry->v_offset = value;
      break;
   case H_WIDTH:
      geometry->h_width = value;
      break;
   case V_HEIGHT:
      geometry->v_height = value;
      break;
   case FB_WIDTH:
      geometry->fb_width = value;
      update_param_range();
      break;
   case FB_HEIGHT:
      geometry->fb_height = value;
      update_param_range();
   case FB_BPP:
      geometry->fb_bpp = value;
      break;
   case CLOCK:
      geometry->clock = value;
      break;
   case LINE_LEN:
      geometry->line_len = value;
      break;
   case CLOCK_PPM:
      geometry->clock_ppm = value;
      break;
   case PX_SAMPLING:
      geometry->px_sampling = value;
      break;
   }
}

param_t *geometry_get_params() {
   return params;
}

void geometry_get_fb_params(capture_info_t *capinfo) {
   capinfo->h_offset       = geometry->h_offset;
   capinfo->v_offset       = geometry->v_offset;
   capinfo->chars_per_line = geometry->h_width;
   capinfo->nlines         = geometry->v_height;
   capinfo->width          = geometry->fb_width;
   capinfo->height         = geometry->fb_height;
   capinfo->bpp            = geometry->fb_bpp;
   capinfo->px_sampling    = geometry->px_sampling;
}

void geometry_get_clk_params(clk_info_t *clkinfo) {
   clkinfo->clock        = geometry->clock;
   clkinfo->line_len     = geometry->line_len;
   clkinfo->clock_ppm    = geometry->clock_ppm;
}

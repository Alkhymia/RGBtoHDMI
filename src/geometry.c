#include <stdio.h>
#include "geometry.h"
#include "cpld.h"
#include "osd.h"
#include "defs.h"
#include "logging.h"

static const char *px_sampling_names[] = {
   "Normal",
   "Odd",
   "Even",
   "Half Odd",
   "Half Even",
};

static const char *sync_names[] = {
   "-H-V",
   "+H-V",
   "-H+V",
   "+H+V",
   "Composite",
   "Inverted",
   "Composite -V",
   "Inverted -V"
};

static const char *fb_sizex2_names[] = {
   "Normal",
   "Double Height",
   "Double Width",
   "Double Height+Width",
};

static param_t params[] = {
   {    H_OFFSET,           "H Offset",           "h_offset",         0,       512, 4 },
   {    V_OFFSET,           "V Offset",           "v_offset",         0,       512, 1 },
   {     H_WIDTH,        "Min H Width",        "min_h_width",       200,      1920, 8 },
   {    V_HEIGHT,       "Min V Height",       "min_v_height",       150,      1200, 2 },
   {    FB_WIDTH,        "Max H Width",        "max_h_width",       100,      1920, 8 },
   {   FB_HEIGHT,       "Max V Height",       "max_v_height",       100,      1200, 2 },
   {   FB_SIZEX2,            "FB Size",            "fb_size",         0,         3, 1 },
   {      FB_BPP,      "FB Bits/Pixel",      "fb_bits_pixel",         4,         8, 4 },
   {       CLOCK,    "Clock Frequency",    "clock_frequency",   1000000,  40000000, 1000 },
   {    LINE_LEN,        "Line Length",        "line_length",       100,      5000, 1 },
   {   CLOCK_PPM,    "Clock Tolerance",    "clock_tolerance",         0,    100000, 100 },
   { LINES_FRAME,    "Lines per Frame",    "lines_per_frame",       250,      1200, 1 },
   {   SYNC_TYPE,          "Sync Type",          "sync_type",         0,NUM_SYNC-1, 1 },
   { PX_SAMPLING,     "Pixel Sampling",     "pixel_sampling",         0,  NUM_PS-1, 1 },
   {          -1,                 NULL,                 NULL,         0,         0, 0 }
};

typedef struct {
   int h_offset;          // horizontal offset (in psync clocks)
   int v_offset;          // vertical offset (in lines)
   int h_width;           // active horizontal width (in 8-bit characters)
   int v_height;          // active vertical height (in lines)
   int fb_width;          // framebuffer width in pixels
   int fb_height;         // framebuffer height (in pixels, before any doubling is applied)
   int fb_sizex2;         // if 1 then double frame buffer height if 2 double width if 3 then both
   int fb_bpp;            // framebuffer bits per pixel
   int clock;             // cpld clock (in Hz)
   int line_len;          // number of clocks per horizontal line
   int clock_ppm;         // cpld tolerance (in ppm)
   int lines_per_frame;   // number of lines per frame
   int sync_type;         // sync type and polarity
   int px_sampling;       // pixel sampling mode
} geometry_t;

static int mode7;
static geometry_t *geometry;
static geometry_t default_geometry;
static geometry_t mode7_geometry;
static int scaling = 0;
static int capture = 0;

void geometry_init(int version) {
   // These are Beeb specific defaults so the geometry property can be ommitted
   mode7_geometry.v_offset      =        18;
   mode7_geometry.h_width       =       504 & 0xfffffff8;
   mode7_geometry.v_height      =       270 & 0xfffffffe;
   mode7_geometry.fb_width      =       504 & 0xfffffff8;
   mode7_geometry.fb_height     =       270 & 0xfffffffe;
   mode7_geometry.fb_sizex2     =         1;
   mode7_geometry.fb_bpp        =         4;
   mode7_geometry.clock         =  12000000;
   mode7_geometry.line_len      =   12 * 64;
   mode7_geometry.clock_ppm     =      5000;
   mode7_geometry.lines_per_frame   =       625;
   mode7_geometry.sync_type     = SYNC_COMP;
   mode7_geometry.px_sampling   = PS_NORMAL;
   default_geometry.v_offset    =        21;
   default_geometry.h_width     =       672 & 0xfffffff8;
   default_geometry.v_height    =       270 & 0xfffffffe;
   default_geometry.fb_width    =       672 & 0xfffffff8;
   default_geometry.fb_height   =       270 & 0xfffffffe;
   default_geometry.fb_sizex2   =         1;
   default_geometry.fb_bpp      =         8;
   default_geometry.clock       =  16000000;
   default_geometry.line_len    =   16 * 64;
   default_geometry.clock_ppm   =      5000;
   default_geometry.lines_per_frame =       625;
   default_geometry.sync_type   = SYNC_COMP;
   default_geometry.px_sampling = PS_NORMAL;

   int firmware_support = cpld->old_firmware_support();

   if (firmware_support & BIT_NORMAL_FIRMWARE_V1) {
      // For backwards compatibility with CPLDv1
      mode7_geometry.h_offset   = 0;
      default_geometry.h_offset = 0;
   } else if (firmware_support & BIT_NORMAL_FIRMWARE_V2) {
      // For backwards compatibility with CPLDv2
      mode7_geometry.h_offset   = 96 & 0xfffffffc;
      default_geometry.h_offset = 128 & 0xfffffffc;
   } else {
      // For CPLDv3 onwards
      mode7_geometry.h_offset   = 140 & 0xfffffffc;
      default_geometry.h_offset = 160 & 0xfffffffc;
   }
   geometry_set_mode(0);
}

void geometry_set_mode(int mode) {
   mode7 = mode;
   geometry = mode ? &mode7_geometry : &default_geometry;
}
int geometry_get_mode() {
   return mode7;
}
int geometry_get_value(int num) {
   switch (num) {
   case H_OFFSET:
      return geometry->h_offset & 0xfffffffc;
   case V_OFFSET:
      return geometry->v_offset;
   case H_WIDTH:
      return geometry->h_width & 0xfffffff8;
   case V_HEIGHT:
      return geometry->v_height & 0xfffffffe;
   case FB_WIDTH:
      return geometry->fb_width & 0xfffffff8;
   case FB_HEIGHT:
      return geometry->fb_height & 0xfffffffe;
   case FB_SIZEX2:
      return geometry->fb_sizex2;
   case FB_BPP:
      return geometry->fb_bpp;
   case CLOCK:
      return geometry->clock;
   case LINE_LEN:
      return geometry->line_len;
   case CLOCK_PPM:
      return geometry->clock_ppm;
   case LINES_FRAME:
      return geometry->lines_per_frame;
   case SYNC_TYPE:
      return geometry->sync_type;
   case PX_SAMPLING:
      return geometry->px_sampling;
   }
   return -1;
}

const char *geometry_get_value_string(int num) {
   if (num == PX_SAMPLING) {
      return px_sampling_names[geometry_get_value(num)];
   }
   if (num == SYNC_TYPE) {
      return sync_names[geometry_get_value(num)];
   }
   if (num == FB_SIZEX2) {
      return fb_sizex2_names[geometry_get_value(num)];
   }
   return NULL;
}

void geometry_set_value(int num, int value) {
   if (value < params[num].min) {
      value = params[num].min;
   }
   if (value > params[num].max) {
      value = params[num].max;
   }
   switch (num) {
   case H_OFFSET:
      geometry->h_offset = value & 0xfffffffc;
      break;
   case V_OFFSET:
      geometry->v_offset = value;
      break;
   case H_WIDTH:
      geometry->h_width = value & 0xfffffff8;
      break;
   case V_HEIGHT:
      geometry->v_height = value & 0xfffffffe;
      break;
   case FB_WIDTH:
      geometry->fb_width = value & 0xfffffff8;
      break;
   case FB_HEIGHT:
      geometry->fb_height = value & 0xfffffffe;
      break;
   case FB_SIZEX2:
      geometry->fb_sizex2 = value;
      break;
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
   case LINES_FRAME:
      geometry->lines_per_frame = value;
      break;
   case SYNC_TYPE:
      geometry->sync_type = value;
      break;
   case PX_SAMPLING:
      geometry->px_sampling = value;
      break;
   }
}

param_t *geometry_get_params() {
   return params;
}

void set_gscaling(int value) {
   scaling = value;
}

void set_capture(int value) {
   capture = value;
}

int get_capture() {
   return capture;
}

void geometry_get_fb_params(capture_info_t *capinfo) {
    capinfo->sizex2 = geometry->fb_sizex2;
    int double_width = (capinfo->sizex2 & 2) >> 1;
    int double_height = capinfo->sizex2 & 1;

    int geometry_h_offset = geometry->h_offset;
    int geometry_v_offset = geometry->v_offset;
    int geometry_h_width = geometry->h_width;
    int geometry_v_height = geometry->v_height;
    int geometry_fb_width = geometry->fb_width;
    int geometry_fb_height = geometry->fb_height;

    if (geometry_fb_width < geometry_h_width) {
        geometry_fb_width = geometry_h_width;
    }
    if (geometry_fb_height < geometry_v_height) {
        geometry_fb_height = geometry_v_height;
    }

    capinfo->bpp            = geometry->fb_bpp;
    capinfo->px_sampling    = geometry->px_sampling;
    capinfo->sync_type      = geometry->sync_type;

    uint32_t h_size = (*PIXELVALVE2_HORZB) & 0xFFFF;
    uint32_t v_size = (*PIXELVALVE2_VERTB) & 0xFFFF;

    //log_info("           H-Total: %d pixels", h_size);
    //log_info("           V-Total: %d pixels", v_size);

    double ratio = (double) h_size / v_size;
    int h_size43 = h_size;
    int v_size43 = v_size;
    if (ratio > 1.34) {
       h_size43 = v_size * 4 / 3;
    }
    if (ratio < 1.24) {               // was 1.32 but don't correct 5:4 aspect ratio (1.25) to 4:3 as it does good integer scaling for 640x256 and 640x200
       v_size43 = h_size * 3 / 4;
    }

    if (v_size43 == v_size && h_size > h_size43) {
        h_size43 = (h_size43 * 800) / 720;           //adjust 4:3 ratio on widescreen resolutions to account for 800 pixel wide integer sample capture 
        if (h_size43 > h_size) {
            h_size43 = h_size;
        }
    }

    //log_info("unadujusted integer = %d, %d, %d, %d, %d, %d", geometry_h_offset, geometry_v_offset, geometry_h_width, geometry_v_height, geometry_fb_width, geometry_fb_height);

    if (capture == CAPTURE_AUTO && scaling == SCALING_INTEGER) {
        int h_size43_adj = mode7 ? (h_size43 * 3 / 4) : h_size43;
        int hs = h_size43_adj / geometry_h_width;
        int new_geometry_h_width = h_size43_adj / hs;
        if (new_geometry_h_width > geometry_fb_width) {
            new_geometry_h_width = geometry_fb_width;
        }
        int vs = v_size43 / geometry_v_height;
        int new_geometry_v_height = v_size43 / vs;
        if (new_geometry_v_height > geometry_fb_height) {
            new_geometry_v_height = geometry_fb_height;
        }
        geometry_h_offset = geometry_h_offset - (((new_geometry_h_width - geometry_h_width) >> 3) << 2);
        geometry_v_offset = geometry_v_offset - ((new_geometry_v_height - geometry_v_height) >> 1);
        geometry_h_width = new_geometry_h_width;
        geometry_v_height = new_geometry_v_height;
    }

    if (capture == CAPTURE_MIN) {
        geometry_fb_width = geometry_h_width;
        geometry_fb_height = geometry_v_height;
    }

    if (capture == CAPTURE_HALF) {
        geometry_fb_width = (((geometry_fb_width - geometry_h_width) >> 1) + geometry_h_width) & 0xfffffff8;
        geometry_fb_height = (((geometry_fb_height - geometry_v_height) >> 1) + geometry_v_height) & 0xfffffffe;
        geometry_h_offset = geometry_h_offset - (((geometry_fb_width - geometry_h_width) >> 3) << 2);
        geometry_v_offset = geometry_v_offset - ((geometry_fb_height - geometry_v_height) >> 1);
        geometry_h_width = geometry_fb_width;
        geometry_v_height = geometry_fb_height;
    }

    if (capture == CAPTURE_MAX
    || (capture == CAPTURE_AUTO && (scaling == SCALING_MANUAL43 || scaling == SCALING_MANUAL))) {
        geometry_h_offset = geometry_h_offset - (((geometry_fb_width - geometry_h_width) >> 3) << 2);
        geometry_v_offset = geometry_v_offset - ((geometry_fb_height - geometry_v_height) >> 1);
        geometry_h_width = geometry_fb_width;
        geometry_v_height = geometry_fb_height;
    }

    //log_info("  adujusted integer = %d, %d, %d, %d", geometry_h_offset, geometry_v_offset, geometry_h_width, geometry_v_height);

    if (geometry_h_offset < 0) {
       geometry_h_offset = 0;
    }
    if (geometry_v_offset < 0) {
       geometry_v_offset = 0;
    }

    capinfo->h_offset = ((geometry_h_offset >> 2) - (cpld->get_delay() >> 2));
    if (capinfo->h_offset < 0) {
       capinfo->h_offset = 0;
    }
    capinfo->v_offset       = geometry_v_offset;
    capinfo->chars_per_line = (geometry_h_width >> 3) << double_width;
    capinfo->nlines         = geometry_v_height;
    capinfo->width          = geometry_fb_width << double_width;          //adjust the width for capinfo according to sizex2 setting;
    capinfo->height         = geometry_fb_height << double_height;        //adjust the height for capinfo according to sizex2 setting



    int standard_width = mode7 ? (geometry_h_width * 4 / 3) : geometry_h_width;    // workaround mode 7 width so it looks like other modes
    int standard_height = geometry_v_height;
    int adjusted_width = geometry_h_width << double_width;
    int adjusted_height = geometry_v_height << double_height;

    double hscalef = (double) h_size43 / standard_width;
    int hscale = (int) hscalef;
    int hborder = ((h_size - standard_width * hscale) << double_width) / hscale;     // (h_size - adjusted_width * hscale) / hscale;

    double vscalef = (double) v_size43 / standard_height;
    int vscale = (int) vscalef;
    int vborder = ((v_size - standard_height * vscale) << double_height) / vscale;

    //log_info("scaling size = %d, %d, %d, %f",standard_width, adjusted_width, adjusted_height, ratio);
    //log_info("scaling h = %d, %d, %f, %d, %d, %d, %d",h_size, h_size43, hscalef, hscale, hborder, hborder43, newhborder43);
    //log_info("scaling v = %d, %d, %f, %d, %d, %d, %d",v_size, v_size43, vscalef, vscale, vborder, vborder43, newvborder43);

    switch (scaling) {
        case    SCALING_INTEGER:
            capinfo->width = adjusted_width + hborder;
            capinfo->height = adjusted_height + vborder;
        break;
        case    SCALING_MANUAL43:
            capinfo->width = (geometry_fb_width << double_width ) + (int)((double)((h_size - h_size43) <<  double_width) / hscalef);
            capinfo->height = (geometry_fb_height << double_height) + (int)((double)((v_size - v_size43) << double_height)  / vscalef);
        break;
        case    SCALING_MANUAL:
        break;
    };

    if (capinfo->chars_per_line > (capinfo->width >> 3)) {
       capinfo->chars_per_line = (capinfo->width >> 3);
    }

    if (capinfo->nlines > (capinfo->height >> double_height)) {
       capinfo->nlines = (capinfo->height >> double_height);
    }

    //log_info("size= %d, %d, %d, %d, %d, %d, %d",capinfo->chars_per_line, capinfo->nlines, geometry_h_width, geometry_v_height,capinfo->width,  capinfo->height, capinfo->sizex2);

}

void geometry_get_clk_params(clk_info_t *clkinfo) {
   clkinfo->clock           = geometry->clock;
   clkinfo->line_len        = geometry->line_len;
   clkinfo->clock_ppm       = geometry->clock_ppm;
   clkinfo->lines_per_frame = geometry->lines_per_frame;
}

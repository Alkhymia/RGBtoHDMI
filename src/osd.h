#ifndef OSD_H
#define OSD_H

#define OSD_SW1     1
#define OSD_SW2     2
#define OSD_SW3     3
#define OSD_EXPIRED 4

#define ATTR_DOUBLE_SIZE (1 << 0)

extern int clock_error_ppm;

enum {
   HDMI_ORIGINAL,
   HDMI_SLOW_2000PPM,
   HDMI_SLOW_1000PPM,
   HDMI_EXACT,
   HDMI_FAST_1000PPM,
   HDMI_FAST_2000PPM
};

enum {
   PALETTE_DEFAULT,
   PALETTE_INVERSE,
   PALETTE_MONO1,
   PALETTE_MONO2,
   PALETTE_RED,
   PALETTE_GREEN,
   PALETTE_BLUE,
   PALETTE_NOT_RED,
   PALETTE_NOT_GREEN,
   PALETTE_NOT_BLUE,
   PALETTE_ATOM_COLOUR_NORMAL,
   PALETTE_ATOM_COLOUR_EXTENDED,
   PALETTE_ATOM_COLOUR_ACORN,
   PALETTE_ATOM_MONO,
   NUM_PALETTES
};

enum {
   DEINTERLACE_NONE,
   DEINTERLACE_BOB,
   DEINTERLACE_MA1,
   DEINTERLACE_MA2,
   DEINTERLACE_MA3,
   DEINTERLACE_MA4,
   DEINTERLACE_ADV,
   NUM_DEINTERLACES
};

void osd_init();
void osd_clear();
void osd_set(int line, int attr, char *text);
void osd_refresh();

void osd_update(uint32_t *osd_base, int bytes_per_line);
void osd_update_fast(uint32_t *osd_base, int bytes_per_line);
int  osd_active();
int  osd_key(int key);
void osd_update_palette();

#endif

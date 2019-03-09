#ifndef CPLD_H
#define CPLD_H

#define VERSION_MINOR_BIT   0
#define VERSION_MAJOR_BIT   4
#define VERSION_DESIGN_BIT  8

#define DESIGN_NORMAL       0
#define DESIGN_ALTERNATIVE  1   // This has now been removed from the code base
#define DESIGN_ATOM         2

enum {
   // Sampling params
   ALL_OFFSETS,
   A_OFFSET,
   B_OFFSET,
   C_OFFSET,
   D_OFFSET,
   E_OFFSET,
   F_OFFSET,
   HALF,
   DIVIDER,
   DELAY,
   SIXBIT
};

typedef struct {
   int key;
   const char *name;
   int min;
   int max;
   int step;
} param_t;

// Define a common interface to abstract the calibration code
// for the two different CPLD implementations
typedef struct {
   const char *name;
   void (*init)(int cpld_version);
   int (*get_version)();
   void (*set_mode)(capture_info_t *capinfo, int mode7);
   void (*calibrate)(capture_info_t *capinfo, int elk);
   // Support for the UI
   param_t *(*get_params)();
   int (*get_value)(int num);
   void (*set_value)(int num, int value);
   // Support for info page
   void (*show_cal_summary)(int line);
   void (*show_cal_details)(int line);
   void (*show_cal_raw)(int line);
} cpld_t;

int diff_N_frames(capture_info_t *capinfo, int n, int mode7, int elk);
int *diff_N_frames_by_sample(capture_info_t *capinfo, int n, int mode7, int elk);
int analyze_default_alignment(capture_info_t *capinfo);
int analyze_mode7_alignment(capture_info_t *capinfo);

// These are global variables defined in rgb_to_hdmi
extern cpld_t         *cpld;
extern capture_info_t *capinfo;

#endif

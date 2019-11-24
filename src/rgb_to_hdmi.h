#ifndef RGB_TO_HDMI_H
#define RGB_TO_HDMI_H

// Property setters/getters
void set_profile(int value);
int  get_profile();
void set_subprofile(int value);
int  get_subprofile();
void set_paletteControl(int value);
int  get_paletteControl();
void set_resolution(int mode, const char *name, int reboot);
int get_resolution();
void set_scaling(int mode, int reboot);
int get_scaling();
void set_deinterlace(int value);
int  get_deinterlace();
void set_scanlines(int on);
int  get_scanlines();
void set_scanlines_intensity(int value);
int  get_scanlines_intensity();
void set_colour(int value);
int  get_colour();
void set_invert(int value);
int  get_invert();
void set_border(int value);
int  get_border();
void set_fontsize(int value);
int  get_fontsize();
void set_elk(int on);
int  get_elk();
void set_vsync(int on);
int  get_vsync();
void set_vlockmode(int val);
int  get_vlockmode();
void set_vlockline(int val);
int  get_vlockline();
void set_vlockspeed(int val);
int  get_vlockspeed();
void set_vlockadj(int val);
int  get_vlockadj();
#ifdef MULTI_BUFFER
void set_nbuffers(int val);
int  get_nbuffers();
#endif
void set_autoswitch(int on);
int  get_autoswitch();
void set_debug(int on);
int  get_debug();

// Actions
void action_calibrate_clocks();
void action_calibrate_auto();

// Status
int is_genlocked();
void set_status_message(char *msg);
void force_reinit();

// Reboot the system immediately
void reboot();

#endif

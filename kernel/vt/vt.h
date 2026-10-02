#ifndef KERNEL_VT_H
#define KERNEL_VT_H











#define VT_GUI      1
#define VT_TEXT_MIN 2
#define VT_TEXT_MAX 6


void vt_request_switch(int number);


void vt_tick(void);

int  vt_active_number(void);
int  vt_is_gui(void);


const char *vt_app_path(void);

#endif 

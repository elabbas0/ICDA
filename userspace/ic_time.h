









#ifndef USERSPACE_IC_TIME_H
#define USERSPACE_IC_TIME_H

#include <stdint.h>





void ic_time_init(void);


uint64_t ic_time_ns(void);
uint64_t ic_time_us(void);
uint32_t ic_time_ms(void);


float ic_time_s(void);



typedef struct {
    int year, month, day;       
    int hour, minute, second;
    int weekday;                
} ic_datetime_t;


int  ic_wallclock(ic_datetime_t *out);

void ic_format_hm(const ic_datetime_t *t, char *buf, int cap);

void ic_format_day(const ic_datetime_t *t, char *buf, int cap);

#endif 

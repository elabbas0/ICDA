#ifndef FPU_H
#define FPU_H

#include <stdint.h>











#define FPU_STATE_SIZE 512





void fpu_init(void);



void fpu_state_init(uint8_t *area);


void fpu_switch(uint8_t *prev, const uint8_t *next);

#endif

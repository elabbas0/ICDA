#ifndef PF_H
#define PF_H

#include <stdint.h>
#include "vmm.h"


#define PF_PRESENT   (1ULL << 0)  
#define PF_WRITE     (1ULL << 1)  
#define PF_USER      (1ULL << 2)  
#define PF_RESERVED  (1ULL << 3)  
#define PF_IFETCH    (1ULL << 4)  




#define USER_STACK_TOP    0x00007FFFFFFFE000ULL  
#define USER_STACK_LIMIT  0x00007FFFFBFFE000ULL   /* 64 MB of stack, mapped as it is touched */







#define USER_STACK_INITIAL_PAGES 4





void pf_set_current_as(addr_space_t *as);




void pf_init(void);

#endif

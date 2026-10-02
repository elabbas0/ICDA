#ifndef GPU_H
#define GPU_H

#include <stdint.h>












#define GPU_MAX_MODES 8
#define GPU_NAME_MAX  32

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;    
    uint32_t bpp;
} gpu_mode_t;

typedef struct gpu_device {
    char        name[GPU_NAME_MAX];
    uint32_t    mode_count;
    gpu_mode_t  modes[GPU_MAX_MODES];
    int         current_mode;

    

    uint64_t    fb_phys;
    uint64_t    fb_size;

    
    int         hw_cursor;      
    int         present_supported;
    int         needs_present;  

    
    int (*present)(struct gpu_device *dev);              
    int (*set_cursor)(struct gpu_device *dev, int x, int y,
                      const uint32_t *image, int w, int h); 

    void *priv;
    struct gpu_device *next;
} gpu_device_t;


int  gpu_register_device(gpu_device_t *dev);


gpu_device_t *gpu_primary(void);


gpu_device_t *gpu_find(const char *name);



int gpu_init(void *multiboot_info);

#endif

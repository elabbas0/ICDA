#ifndef DEVOPS_H
#define DEVOPS_H























#include <stdint.h>

typedef struct dev_calls {
    
    uint64_t (*con_write)(const char *text);
    void     (*con_clear)(void);
    void     (*con_backspace)(void);
    void     (*con_set_cursor)(int x, int y);
    void     (*con_get_cursor)(int *x, int *y);
    int      (*con_columns)(void);
    int      (*con_rows)(void);
    
    int      (*in_read_char)(void);
    
    uint64_t (*fb_claim_map)(void *info);
    int      (*fb_claimed)(void);
    int      (*gpu_query)(void *out);
    int      (*gpu_present)(void);
    int      (*gpu_set_cursor)(int x, int y, const uint32_t *image,
                               int w, int h);
    


    uint64_t (*node_read)(char *buf, uint64_t cap);
    /* writes to a device node (e.g. commands for /dev/wifi) */
    uint64_t (*node_write)(const char *buf, uint64_t len);
} dev_calls_t;



int devops_register(const char *path, const dev_calls_t *calls);


const dev_calls_t *devops_lookup(const char *path);


static inline const dev_calls_t *dev_console(void) {
    const dev_calls_t *d = devops_lookup("/dev/console");
    return (d && d->con_write) ? d : 0;
}

static inline const dev_calls_t *dev_input(void) {
    const dev_calls_t *d = devops_lookup("/dev/input");
    return (d && d->in_read_char) ? d : 0;
}

static inline const dev_calls_t *dev_fb(void) {
    const dev_calls_t *d = devops_lookup("/dev/fb0");
    return (d && d->fb_claim_map) ? d : 0;
}




int dev_populate(void);

#endif

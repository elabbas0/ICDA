































#ifndef USERSPACE_LIBICDA_H
#define USERSPACE_LIBICDA_H

#include <stddef.h>
#include <stdint.h>
#include "ic_version.h"
#include "icda_sys.h"
#include "gui.h"      
#include "gui_proto.h"


#include "ic_mem.h"
#include "ic_time.h"    
#include "ic_anim.h"    
#include "ic_gfx.h"     
#include "ic_font.h"    
#include "ic_theme.h"   
#include "ic_ui.h"      
#include "ic_app.h"     










void ic_memcpy(void *dst, const void *src, uint64_t n);


void ic_memmove(void *dst, const void *src, uint64_t n);


void ic_memset(void *dst, int value, uint64_t n);


int ic_memcmp(const void *a, const void *b, uint64_t n);


void ic_memzero(void *dst, uint64_t n);



uint64_t ic_strlen(const char *s);
int      ic_strcmp(const char *a, const char *b);
int      ic_streq(const char *a, const char *b);
char    *ic_strcpy(char *dst, const char *src, uint64_t cap);
char    *ic_strcat(char *dst, const char *src, uint64_t cap);
int      ic_strprefix(const char *s, const char *prefix);
char     ic_lower(char c);
void     ic_uint_to_str(uint64_t v, char *out, uint64_t cap);
int      ic_parse_uint(const char *s, uint64_t *out);




uint64_t ic_strnlen(const char *s, uint64_t cap);



char *ic_strncpy(char *dst, const char *src, uint64_t n, uint64_t cap);




uint64_t ic_strlcat(char *dst, const char *src, uint64_t cap);



uint64_t ic_snprintf_u64(char *buf, uint64_t cap, uint64_t val);



uint64_t ic_snprintf_hex(char *buf, uint64_t cap, uint64_t val);




int ic_ato_u64(const char *s, uint64_t *out);



int ic_is_digit(char c);
int ic_is_space(char c);
int ic_is_alpha(char c);




uint64_t ic_utf8_len(const char *s);




int ic_utf8_valid(const char *s);







#define IC_ARENA_ALIGN_MAX 64

typedef struct {
    uint8_t  *buf;    
    uint64_t  cap;    
    uint64_t  offset; 
} ic_arena_t;



int ic_arena_init(ic_arena_t *a, uint8_t *buf, uint64_t cap);




void *ic_arena_alloc(ic_arena_t *a, uint64_t size, uint64_t align);


void ic_arena_reset(ic_arena_t *a);


uint64_t ic_arena_used(const ic_arena_t *a);


uint64_t ic_arena_remaining(const ic_arena_t *a);









typedef struct {
    uint8_t  *buf;   
    uint64_t  cap;   
    uint64_t  head;  
    uint64_t  tail;  
} ic_ring_u8_t;



int ic_ring_u8_init(ic_ring_u8_t *r, uint8_t *buf, uint64_t cap);


int ic_ring_u8_push(ic_ring_u8_t *r, uint8_t byte);


int ic_ring_u8_pop(ic_ring_u8_t *r, uint8_t *byte_out);


uint64_t ic_ring_u8_count(const ic_ring_u8_t *r);


uint64_t ic_ring_u8_free_cap(const ic_ring_u8_t *r);


void ic_ring_u8_reset(ic_ring_u8_t *r);











typedef struct {
    uint16_t       w;
    uint16_t       h;
    const uint8_t *rgba;
} ic_icon_t;

int  ic_icon_parse(const uint8_t *blob, uint64_t size, ic_icon_t *out);
int  ic_icon_valid(const ic_icon_t *icon);

void ic_icon_draw(ic_canvas_t *c, int x, int y, int dw, int dh, const ic_icon_t *icon);





int ic_icon_load_folder(const char *dir);





int ic_ico_parse(const uint8_t *blob, uint64_t size, int max_decode,
                 ic_icon_t *out, uint8_t *rgba_out, uint64_t rgba_cap);



const ic_icon_t *ic_icon_builtin(const char *name);





#ifndef U_ENOENT
#define U_ENOENT  2   
#define U_EBADF   9   
#define U_ENOMEM  12  
#define U_EACCES  13  
#define U_EFAULT  14  
#define U_EINVAL  22  
#endif









int ic_read_file_b(const char *path, char *buf, uint64_t cap, uint64_t *len_out);



int ic_write_file_b(const char *path, const char *buf, uint64_t len);



int ic_stat_b(const char *path, icda_stat_t *out);



int ic_getcwd_b(char *buf, uint64_t cap);


int ic_mkdir_b(const char *path);


int ic_create_b(const char *path);





int ic_path_join(char *dst, uint64_t cap, const char *a, const char *b);




int ic_path_normalize(char *dst, uint64_t cap, const char *src);




int ic_list_dir_b(const char *path, char *buf, uint64_t cap, uint64_t *len_out);







typedef struct {
    const char *buf;
    uint64_t    len;
    uint64_t    pos;
} ic_dir_cursor_t;

void ic_dir_cursor_init(ic_dir_cursor_t *cur, const char *buf, uint64_t len);
int  ic_dir_next(ic_dir_cursor_t *cur, const char **name_out,
                 uint64_t *name_len_out, int *is_dir_out);






uint64_t ic_spawn_b(const char *path);


uint64_t ic_spawn_args_b(const char *path, const char *args);


int ic_wait_b(uint64_t pid);


void ic_sleep_ticks(uint64_t ticks);


uint64_t ic_ticks_b(void);


void ic_yield_b(void);


_Noreturn void ic_exit_b(uint64_t code);



typedef struct {
    uint64_t handle;
    uint64_t addr;
    uint64_t size;
    int      valid;
} ic_shm_t;


int  ic_shm_acquire(uint64_t size, ic_shm_t *out);


void ic_shm_release(ic_shm_t *t);




uint64_t ic_msg_open_b(const char *name);
int      ic_msg_send_b(uint64_t handle, const void *msg);
int      ic_msg_recv_b(uint64_t handle, void *out, int block);
int      ic_msg_poll_b(uint64_t handle);









int ic_url_split(const char *url, char *host_out, uint64_t host_cap,
                 uint16_t *port_out, char *path_out, uint64_t path_cap,
                 int *use_tls_out);



int ic_dns_b(const char *host, uint32_t *ipv4_out);






int ic_http_fetch_to_file(const char *host, uint16_t port, int use_tls,
                          const char *path, const char *out_path,
                          uint64_t *bytes_out);





int ic_http_fetch_mem(const char *url, char *buf, uint64_t cap,
                      uint64_t *len_out, const char *scratch_path);







int ic_layout_row(ic_rect_t parent, int pad, int gap,
                  const int *widths, int count,
                  ic_rect_t *out, int out_cap);

int ic_layout_col(ic_rect_t parent, int pad, int gap,
                  const int *heights, int count,
                  ic_rect_t *out, int out_cap);


/* "Version 1.6.1": the installed release (/etc/icda-release.txt, written by
 * OTA patches), or the version this program was built as. */
const char *ic_version_label(void);

#endif

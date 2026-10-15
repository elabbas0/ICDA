#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userspace/icda_sys.h"
#include "../userspace/surfer/http.h"
#include "../kernel/crypto/sha256.h"
#define ICDA_MODEL_CATALOG_H
typedef struct {const char *name,*label,*url,*sha;uint64_t size;} icda_model_t;
static icda_model_t icda_models[]={{"test.gguf","Test Qwen","https://huggingface.co/Qwen/test/resolve/pinned/test.gguf",0,8}};
#define ICDA_MODEL_COUNT 1
static char status[512], staged[32], installed[32];
static int mode, step, removed, renamed, writes, opens;
static http_req_t request;
static uint64_t fake_stat(const char *path,icda_stat_t *st) {
    memset(st,0,sizeof(*st));
    if(mode!=4 && strstr(path,"/volumes/fat32-0/ICDACFG.TXT")) return 0;
    return (uint64_t)-1;
}
static uint64_t fake_read(const char *path,char *buf,uint64_t cap){(void)path;(void)buf;(void)cap;return (uint64_t)-1;}
static uint64_t fake_write(const char *path,const char *buf,uint64_t size){
    assert(!strstr(path,"/UPDATE/"));
    if(strstr(path,".status")){assert(size<sizeof(status));memcpy(status,buf,size);status[size]=0;}
    return size;
}
static long fake_write_at(const char *path,uint64_t off,const void *buf,uint64_t size){
    assert(strstr(path,"/volumes/fat32-0/linux/usr/share/icda/models/test.gguf.download"));
    if(mode==2) return -1;
    assert(off+size<=8);memcpy(staged+off,buf,size);writes++;return size;
}
static long fake_remove(const char *path){assert(strstr(path,".download"));removed++;return 0;}
static long fake_rename(const char *from,const char *to){assert(strstr(from,".download"));assert(strstr(to,"/linux/usr/share/icda/models/test.gguf"));memcpy(installed,staged,8);renamed++;return 0;}
static http_req_t *fake_open(const char *url,const char *headers){
    assert(!strncmp(url,"https://huggingface.co/Qwen/",28));opens++;step=0;
    memset(&request,0,sizeof(request));request.headers_done=1;request.status=200;
    if(mode>=5 && opens>1){assert(headers && !strcmp(headers,"Range: bytes=4-\r\n"));
        if(mode!=6){request.status=206;request.raw_headers=mode==7?"Content-Range: bytes 0-7/8\r\n":"Content-Range: bytes 4-7/8\r\n";step=1;}}
    return &request;
}
static int fake_poll(http_req_t *r,int timeout){(void)timeout;static unsigned char content[]="GGUFtest";
    if(mode>=5 && opens==1 && step==1){r->body_len=0;return HTTP_ERROR;}
    r->body=content+step*4;r->body_len=4;step++;if(mode==3)r->status=503;return step==2?HTTP_DONE:HTTP_PENDING;
}
static void fake_free(http_req_t *r){(void)r;}
static uint64_t fake_mkdir(const char *path){(void)path;return 0;}
static uint64_t fake_ticks(void){return 100;}
static uint64_t fake_pid(void){return 42;}
static uint64_t fake_sync(void){return 0;}
static void fake_sleep(uint64_t ticks){(void)ticks;}
#define icda_stat fake_stat
#define icda_read_file fake_read
#define icda_write_file fake_write
#define icda_write_file_at fake_write_at
#define icda_remove fake_remove
#define icda_rename fake_rename
#define icda_mkdir fake_mkdir
#define icda_ticks fake_ticks
#define icda_get_pid fake_pid
#define icda_sync fake_sync
#define icda_sleep fake_sleep
#define http_open_follow fake_open
#define http_poll fake_poll
#define http_free fake_free
#define main model_install_main
#include "../userspace/model-install.c"
#undef main
int main(void){
    uint8_t digest[32];char hex[65];sha256_hash((const uint8_t*)"GGUFtest",8,digest);
    for(int i=0;i<32;i++)sprintf(hex+i*2,"%02x",digest[i]);icda_models[0].sha=hex;
    char *args[]={"model-install","test.gguf",0};
    for(mode=0;mode<8;mode++){
        opens=step=removed=renamed=writes=0;memset(staged,0,sizeof(staged));strcpy(installed,"old-model");
        if(mode==1)icda_models[0].sha="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";else icda_models[0].sha=hex;
        int result=model_install_main(2,args);
        if(!mode || mode==5 || mode==6){assert(!result && renamed==1 && writes==(mode==6?3:2) && !memcmp(installed,"GGUFtest",8));}
        else {assert(result==1 && !renamed && removed==(mode==4?0:1) && !strcmp(installed,"old-model"));}
    }
    assert(model_install_main(1,args)==1);
    puts("Model streaming, hash verification, reconnect/resume, ignored/wrong ranges, failure preservation and invalid selection passed");return 0;
}

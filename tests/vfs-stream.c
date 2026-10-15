#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../kernel/fs/vfs.c"
static size_t largest;
void *kmalloc(size_t n){if(n>largest)largest=n;return malloc(n);}
void *kcalloc(size_t n,size_t s){if(n*s>largest)largest=n*s;return calloc(n,s);}
void kfree(void *p){free(p);}
uint64_t sched_ticks(void){return 1;}
static int fail_write;
static int external(int op,uint8_t mount,const char *path,const char *data,uint64_t size,uint64_t off){
    (void)op;(void)data;(void)size;(void)off;assert(mount==1 && !strncmp(path,"/volumes/test/",14));return fail_write?-1:0;
}
static int64_t load(uint8_t mount,const char *path,uint64_t ref,uint64_t off,char *buf,uint64_t size){
    (void)mount;(void)path;(void)ref;(void)off;memset(buf,'q',size);return size;
}
int main(void){
    vfs_init();vfs_node_t *root=vfs_root();
    assert(!vfs_mkdir(root,"/volumes"));assert(!vfs_mkdir(root,"/home"));
    assert(!vfs_mkdir(root,"/volumes/test"));assert(!vfs_set_mount("/volumes/test",1));
    vfs_set_external_hook(external);vfs_set_loader(load);
    assert(!vfs_write(root,"/volumes/test/model.download","",0));
    vfs_node_t *file=vfs_resolve(root,"/volumes/test/model.download");assert(file&&file->lazy&&!file->data);
    char data[4096];memset(data,'q',sizeof(data));
    const uint64_t offsets[]={0,64ULL<<20,500ULL<<20,1100000000};
    for(unsigned i=0;i<4;i++){assert(!vfs_node_write_at(file,offsets[i],data,sizeof(data)));assert(file->lazy&&!file->data&&file->cap==0);}
    assert(file->size==1100004096 && largest<65536);
    char got[32];assert(vfs_node_read_at(file,1000000000,got,sizeof(got))==32&&!memcmp(got,data,32));
    fail_write=1;assert(vfs_node_write_at(file,1200000000,data,4096)==-1&&file->size==1100004096);fail_write=0;
    assert(!vfs_node_truncate(file,0)&&file->lazy&&!file->size);
    assert(!vfs_node_write_at(file,0,data,4096)&&!file->data);
    assert(!vfs_write(root,"/home/ram-file","abc",3));file=vfs_resolve(root,"/home/ram-file");
    assert(file&&!file->lazy&&file->size==3&&!memcmp(file->data,"abc",3));
    puts("Mounted files stream beyond 1 GiB without RAM growth; readback, failed writes, truncation and RAM files passed");return 0;
}

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char root[80], path[120];
    snprintf(root,sizeof(root),"/tmp/icda-mkdir-%ld",(long)getpid());
    snprintf(path,sizeof(path),"%s/nested",root);
    errno=0; assert(mkdir(path,0700)==-1 && errno==ENOENT);
    assert(mkdir(root,0700)==0);
    assert(mkdir(path,0700)==0);
    errno=0; assert(mkdir(path,0700)==-1 && errno==EEXIST);
    char child[160]; snprintf(child,sizeof(child),"%s/child/",path);
    assert(mkdir(child,0700)==0);
    assert(rmdir(child)==0); assert(rmdir(path)==0); assert(rmdir(root)==0);
    puts("mkdir parent errors, recursive creation, EEXIST and trailing slash passed");
}

/* A dynamically linked program: musl's loader, libz from the Linux root,
 * threads, dlopen.  Prints "dyntest: ALL PASS" when everything works. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("PASS %s\n", what); else { printf("FAIL %s\n", what); failures++; } fflush(stdout); } while (0)

static void *worker(void *arg) {
    return (void *)((long)arg * 2);
}

int main(void) {
    unsigned char src[4096], packed[8192], back[4096];
    uLongf plen = sizeof packed, blen = sizeof back;
    void *ret = 0;
    pthread_t t;

    printf("dyntest: zlib %s\n", zlibVersion());
    for (int i = 0; i < (int)sizeof src; i++) src[i] = (unsigned char)(i % 7);
    CHECK(compress(packed, &plen, src, sizeof src) == Z_OK && uncompress(back, &blen, packed, plen) == Z_OK &&
          blen == sizeof src && !memcmp(src, back, sizeof src), "zlib (shared library) round trip");

    CHECK(pthread_create(&t, 0, worker, (void *)21) == 0 && pthread_join(t, &ret) == 0 && (long)ret == 42, "thread");

    {
        void *h = dlopen("libz.so.1", RTLD_NOW);
        const char *(*ver)(void) = h ? (const char *(*)(void))dlsym(h, "zlibVersion") : 0;
        CHECK(h && ver && !strcmp(ver(), zlibVersion()), "dlopen / dlsym");
    }

    if (failures) printf("dyntest: %d FAILED\n", failures);
    else printf("dyntest: ALL PASS\n");
    return failures != 0;
}

#include <assert.h>
#include <string.h>
#define main icda_updater_main
#include "../userspace/updated.c"
#undef main
int main(void) {
    static manifest_t have, want;
    const char *catalog="version 1.8.24\nmodel linux/usr/share/icda/models/test.gguf 1117320736 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa https://huggingface.co/Qwen/test/resolve/pinned/test.gguf\n";
    assert(parse_manifest(catalog,&have)==0 && have.count==0);
    assert(parse_manifest("version 1.8.24\nfile SYSTEM/sbin/updated 42 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n",&want)==0);
    assert(want.count==1 && !strcmp(want.files[0].rel,"SYSTEM/sbin/updated"));
    assert(!find(&have,"linux/usr/share/icda/models/test.gguf"));
    return 0;
}

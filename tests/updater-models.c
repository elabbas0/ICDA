#include <assert.h>
#include <string.h>
#include "../userspace/icda_sys.h"
static int present;
static uint64_t fake_size = 4;
static uint64_t fake_stat(const char *path, icda_stat_t *st) {
    if (!present || strcmp(path, "/linux/usr/share/icda/models/test.gguf")) return (uint64_t)-1;
    memset(st, 0, sizeof(*st)); st->size = fake_size; return 0;
}
static uint64_t fake_read(const char *path, char *out, uint64_t cap) {
    (void)path; if (cap < 4) return (uint64_t)-1; memcpy(out, "GGUF", 4); return 4;
}
#define icda_stat fake_stat
#define icda_read_file fake_read
#define main icda_updater_main
#include "../userspace/updated.c"
#undef main
int main(void) {
    static manifest_t m;
    const char *catalog = "version 1.8.18\nmodel linux/usr/share/icda/models/test.gguf 4 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa https://huggingface.co/Qwen/test/resolve/pinned/test.gguf\n";
    assert(parse_manifest(catalog, &m) == 0 && m.count == 1);
    assert(m.files[0].webkit && m.files[0].source_url[0] && !m.files[0].packed);
    assert(!entry_current(&m.files[0], &m.files[0])); /* stored catalog, model not downloaded */
    present = 1; assert(entry_current(&m.files[0], &m.files[0]));
    fake_size = 5; assert(!entry_current(&m.files[0], &m.files[0]));
    assert(parse_manifest("version 1.8.18\nmodel linux/usr/share/icda/models/test.gguf 4 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ftp://invalid/model\n", &m) == 0 && m.count == 0);
    return 0;
}

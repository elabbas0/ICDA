/* Assistant-owned model downloads. No OTA staging or Windows model imports. */
#include <stdio.h>
#include <string.h>
#include "icda_sys.h"
#include "http.h"
#include "sha256.h"
#include "model_catalog.h"

#define STATUS "/tmp/ai/model-install.status"
#define PID "/tmp/ai/model-install.pid"
static void report(const char *s) { (void)icda_write_file(STATUS, s, strlen(s)); }
static int range_matches(const char *headers, uint64_t offset) {
    const char *key = "content-range: bytes ";
    if (!headers) return 0;
    for (const char *p = headers; *p; p++) {
        if (p != headers && p[-1] != '\n') continue;
        unsigned i = 0;
        while (key[i] && p[i] && ((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]) == key[i]) i++;
        if (!key[i]) {
            const char *n = p + i; uint64_t value = 0;
            if (*n < '0' || *n > '9') return 0;
            while (*n >= '0' && *n <= '9') { if (value > (UINT64_MAX - 9) / 10) return 0; value = value * 10 + (*n++ - '0'); }
            return *n == '-' && value == offset;
        }
    }
    return 0;
}
static int owned_dir(char *dir, size_t cap) {
    icda_stat_t st;
    char marker[128];
    for (int i = 0; i < 32; i++) {
        snprintf(marker, sizeof(marker), "/volumes/fat32-%d/ICDACFG.TXT", i);
        if ((long)icda_stat(marker, &st) < 0) continue;
        snprintf(dir, cap, "/volumes/fat32-%d/linux", i); (void)icda_mkdir(dir);
        const char *parts[] = { "/usr", "/usr/share", "/usr/share/icda", "/usr/share/icda/models" };
        char path[256];
        for (int j = 0; j < 4; j++) { snprintf(path, sizeof(path), "%s%s", dir, parts[j]); (void)icda_mkdir(path); }
        snprintf(dir, cap, "/volumes/fat32-%d/linux/usr/share/icda/models", i);
        return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    const icda_model_t *model = 0;
    char dir[256], target[320], part[336], line[256], hex[65], id[32];
    uint8_t digest[32]; sha256_ctx_t hash;
    uint64_t offset = 0, last = 0;
    icda_stat_t st;
    (void)icda_mkdir("/tmp"); (void)icda_mkdir("/tmp/ai");
    /* The PID remains after exit, but only an active worker owns the lock. */
    long len = (long)icda_read_file(PID, id, sizeof(id) - 1);
    if (len > 0) {
        uint64_t other = 0; id[len] = 0;
        for (char *p = id; *p >= '0' && *p <= '9'; p++) other = other * 10 + (*p - '0');
        icda_proc_info_t info;
        if (other && other != icda_get_pid() && (long)icda_proc_info(other, &info) >= 0 && info.state < 5) return 2;
    }
    snprintf(id, sizeof(id), "%llu", (unsigned long long)icda_get_pid());
    (void)icda_write_file(PID, id, strlen(id));
    for (unsigned i = 0; argc == 2 && i < ICDA_MODEL_COUNT; i++)
        if (!strcmp(argv[1], icda_models[i].name)) model = &icda_models[i];
    if (!model) { report("Choose Qwen 0.5B or 1.5B in Settings > Assistant."); return 1; }
    if (owned_dir(dir, sizeof(dir))) { report("Install ICDA to its own system partition before downloading models."); return 1; }
    snprintf(target, sizeof(target), "%s/%s", dir, model->name);
    snprintf(part, sizeof(part), "%s.download", target);
    if ((long)icda_stat(target, &st) >= 0 && st.size == model->size) {
        char magic[4];
        if ((long)icda_read_file(target, magic, 4) == 4 && !memcmp(magic, "GGUF", 4)) {
            report("Selected model is already installed. Ask the assistant to use it."); return 0;
        }
    }
    report("Connecting to official Hugging Face...");
    if ((long)icda_write_file(part, "", 0) < 0) { report("Could not create model download on the ICDA partition."); return 1; }
    sha256_init(&hash);
    int state = HTTP_ERROR, disk_error = 0;
    /* Hotspots can drop long HTTPS transfers. Resume the pinned object and
     * retain its running hash; never append a server's full response to a
     * partial file when it ignores Range. */
    for (int attempt = 0; attempt < 16; attempt++) {
      char range[96];
      uint64_t start = offset, active = icda_ticks();
      snprintf(range, sizeof(range), "Range: bytes=%llu-\r\n", (unsigned long long)offset);
      http_req_t *r = http_open_follow(model->url, offset ? range : 0);
      if (!r) { icda_sleep(100); continue; }
      if (offset && r->status == 200) {
          if ((long)icda_write_file(part, "", 0) < 0) { disk_error = 1; http_free(r); break; }
          offset = start = 0; sha256_init(&hash);
      }
      if (start && !range_matches(r->raw_headers, start)) { http_free(r); icda_sleep(100); continue; }
      state = HTTP_PENDING;
      do {
        state = http_poll(r, 200);
        if (r->headers_done && r->status != (start ? 206 : 200)) { state = HTTP_ERROR; break; }
        if (r->body_len) {
            size_t n = r->body_len;
            if (offset + n > model->size) { state = HTTP_ERROR; break; }
            if ((long)icda_write_file_at(part, offset, r->body, n) != (long)n) { disk_error = 1; state = HTTP_ERROR; break; }
            sha256_update(&hash, r->body, (uint32_t)n);
            offset += n; r->body_len = 0; active = icda_ticks();
        }
        if (icda_ticks() - active > 3000) { state = HTTP_ERROR; break; }
        if (icda_ticks() - last >= 100) {
            snprintf(line, sizeof(line), "Qwen: %llu%% (%llu / %llu MiB)",
                     (unsigned long long)(offset * 100 / model->size),
                     (unsigned long long)(offset >> 20), (unsigned long long)((model->size + 1048575) >> 20));
            report(line); last = icda_ticks();
        }
      } while (state == HTTP_PENDING);
      snprintf(line,sizeof(line),"Attempt %d; HTTP %d; transfer %d; bytes %llu/%llu; disk error %d; %s\n",
               attempt + 1,r->status,state,(unsigned long long)offset,(unsigned long long)model->size,disk_error,r->error);
      (void)icda_write_file("/tmp/ai/model-install.log",line,strlen(line));
      http_free(r);
      if (disk_error || offset == model->size) break;
      snprintf(line, sizeof(line), "Reconnecting; resuming at %llu MiB...", (unsigned long long)(offset >> 20)); report(line);
      icda_sleep(100);
    }
    sha256_final(&hash, digest);
    for (int i = 0; i < 32; i++) { hex[i * 2] = "0123456789abcdef"[digest[i] >> 4]; hex[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15]; } hex[64] = 0;
    if (state != HTTP_DONE || offset != model->size || strcmp(hex, model->sha)) {
        (void)icda_remove(part); report("Download failed or checksum did not match. Existing model preserved. Try Download again."); return 1;
    }
    /* Publish only a completely verified file. Never remove a valid model first. */
    if (icda_rename(part, target) < 0) { report("Verified download could not be installed. Check free space on ICDA."); return 1; }
    (void)icda_sync(); report("Qwen installed and verified. Ready to use.");
    return 0;
}

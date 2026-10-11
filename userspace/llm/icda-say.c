/* icda-say - the assistant's voice (a Linux program, run by ICDA's Linux
 * personality; built against Flite by userspace/llm/build-say.sh).
 *
 *   icda-say <text file> <wav file>
 *
 * Speaks the text into a WAV file (16 kHz mono) that the assistant plays
 * through ICDA's mixer.  The voice (CMU SLT) is compiled in: one file, no data
 * folder.  Markdown marks and code are left out of what is spoken. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <flite/flite.h>

cst_voice *register_cmu_us_slt(const char *voxdir);

int main(int argc, char **argv) {
    static char text[8192], clean[8192];
    size_t n, k = 0;
    int in_code = 0;
    FILE *f;
    cst_voice *v;
    if (argc < 3) {
        fprintf(stderr, "usage: icda-say <text file> <wav file>\n");
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) return 1;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    for (size_t i = 0; i < n && k + 1 < sizeof(clean); i++) {
        if (!strncmp(text + i, "```", 3)) {          /* code blocks are not read out */
            in_code = !in_code;
            i += 2;
            continue;
        }
        if (in_code || text[i] == '*' || text[i] == '#' || text[i] == '`' || text[i] == '_') continue;
        clean[k++] = text[i] == '\n' ? ' ' : text[i];
    }
    clean[k] = 0;
    flite_init();
    v = register_cmu_us_slt(NULL);
    if (!v) return 1;
    {   /* written under another name, then renamed: the file appears only when whole */
        char part[512];
        snprintf(part, sizeof(part), "%s.part", argv[2]);
        flite_text_to_speech(clean, v, part);
        return rename(part, argv[2]) == 0 ? 0 : 1;
    }
}

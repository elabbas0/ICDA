/* dr_wav and dr_flac implementations (public domain / MIT-0) */
#define DRWAV_ASSERT(x) ((void)0)
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include "third_party/dr_wav.h"
#define DRFLAC_ASSERT(x) ((void)0)
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_SIMD
#define DR_FLAC_IMPLEMENTATION
#include "third_party/dr_flac.h"

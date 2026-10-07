#include "vpx/vpx_codec.h"
static const char *cfg = "--target=x86_64-linux-gcc decoders only (ICDA)";
const char *vpx_codec_build_config(void) { return cfg; }

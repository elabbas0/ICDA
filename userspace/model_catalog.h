#ifndef ICDA_MODEL_CATALOG_H
#define ICDA_MODEL_CATALOG_H
#include <stdint.h>
typedef struct { const char *name, *label, *url, *sha; uint64_t size; } icda_model_t;
static const icda_model_t icda_models[] = {
    { "qwen2.5-0.5b-instruct-q4_k_m.gguf", "Qwen 2.5 0.5B (469 MiB)",
      "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/9217f5db79a29953eb74d5343926648285ec7e67/qwen2.5-0.5b-instruct-q4_k_m.gguf",
      "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db", 491400032ULL },
    { "qwen2.5-1.5b-instruct-q4_k_m.gguf", "Qwen 2.5 1.5B (1066 MiB)",
      "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/91cad51170dc346986eccefdc2dd33a9da36ead9/qwen2.5-1.5b-instruct-q4_k_m.gguf",
      "6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e", 1117320736ULL },
    /* bigger: much better at using ICDA (files, the web, apps) - slower, and they want 6 GB of RAM or more */
    { "qwen2.5-3b-instruct-q4_k_m.gguf", "Qwen 2.5 3B (2007 MiB)",
      "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/7dabda4d13d513e3e842b20f0d435c732f172cbe/qwen2.5-3b-instruct-q4_k_m.gguf",
      "626b4a6678b86442240e33df819e00132d3ba7dddfe1cdc4fbb18e0a9615c62d", 2104932768ULL },
    { "Qwen3-4B-Q4_K_M.gguf", "Qwen3 4B (2382 MiB, best)",
      "https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/bc640142c66e1fdd12af0bd68f40445458f3869b/Qwen3-4B-Q4_K_M.gguf",
      "7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5", 2497280256ULL }
};
#define ICDA_MODEL_COUNT (sizeof(icda_models) / sizeof(icda_models[0]))
#endif

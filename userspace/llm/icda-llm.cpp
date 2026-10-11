// icda-llm - the assistant's local model (a Linux program, run by ICDA's Linux
// personality; built against llama.cpp by userspace/llm/build.sh).
//
// It loads a GGUF model once and answers through two files, like the rest of
// the assistant (no server sockets needed):
//
//   /tmp/ai/local-request   "seq\n" then messages, each "role\x1fcontent\x1e"
//   /tmp/ai/local-reply     "seq state\n" then the answer so far
//                            (state: answer, done or error)
//
// Usage: icda-llm -m model.gguf [-t threads] [-c context]
// Based on llama.cpp's examples/simple-chat (MIT).

#include "llama.h"
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

static const char *REQ = "/tmp/ai/local-request";
static const char *REPLY = "/tmp/ai/local-reply";

static std::string read_file(const char *path) {
    std::string s;
    FILE *f = fopen(path, "rb");
    if (!f) return s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static void write_reply(unsigned long seq, const char *state, const std::string &text) {
    std::string out = std::to_string(seq) + " " + state + "\n" + text;
    std::string tmp = std::string(REPLY);
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

static void batch_set_tokens(llama_batch_ext *batch, const llama_token *tokens, int32_t n_tokens, llama_pos pos_0) {
    llama_batch_ext_clear(batch);
    for (int32_t i = 0; i < n_tokens; ++i) {
        const int32_t idx = llama_batch_ext_add_token(batch, 0, tokens[i]);
        const llama_pos pos = pos_0 + i;
        llama_batch_ext_set_pos(batch, idx, &pos);
    }
    llama_batch_ext_set_output_logits(batch, n_tokens - 1, true);
}

// what the model shows of its answer: reasoning ("<think>...</think>") left out
static std::string visible(const std::string &s) {
    size_t a = s.find("<think>");
    if (a == std::string::npos) return s;
    size_t b = s.find("</think>", a);
    if (b == std::string::npos) return s.substr(0, a);
    std::string rest = s.substr(b + 8);
    size_t k = rest.find_first_not_of(" \n");
    return s.substr(0, a) + (k == std::string::npos ? "" : rest.substr(k));
}

int main(int argc, char **argv) {
    std::setlocale(LC_NUMERIC, "C");
    // started by assistantd there is no terminal: what it has to say goes to a log
    if (!isatty(2)) {
        freopen("/tmp/ai/llm.log", "w", stderr);
        freopen("/tmp/ai/llm.log", "a", stdout);
    }
    std::string model_path;
    int n_ctx = 4096, threads = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-m")) model_path = argv[++i];
        else if (!strcmp(argv[i], "-c")) n_ctx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t")) threads = atoi(argv[++i]);
    }
    if (model_path.empty()) {
        fprintf(stderr, "usage: icda-llm -m model.gguf [-t threads] [-c context]\n");
        return 1;
    }
    llama_log_set([](enum ggml_log_level level, const char *text, void *) {
        if (level >= GGML_LOG_LEVEL_ERROR) fprintf(stderr, "%s", text);
    }, nullptr);
    llama_backend_init();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    llama_model *model = llama_model_load_from_file(model_path.c_str(), mp);
    if (!model) {
        write_reply(0, "error", "The local model could not be loaded: " + model_path);
        return 1;
    }
    const llama_vocab *vocab = llama_model_get_vocab(model);
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx;
    cp.n_batch = 512;
    if (threads > 0) {
        cp.n_threads = threads;
        cp.n_threads_batch = threads;
    }
    llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        write_reply(0, "error", "The local model has no room for its context");
        return 1;
    }
    llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    llama_batch_ext *batch = llama_batch_ext_init(ctx);
    const char *tmpl = llama_model_chat_template(model, nullptr);
    fprintf(stderr, "icda-llm: ready (%s)\n", model_path.c_str());

    unsigned long done_seq = 0;
    {   // a request already answered (by an earlier start) is not news; one that
        // arrived while the model loaded is - assistantd asks right after starting us
        std::string r = read_file(REPLY);
        done_seq = strtoul(r.c_str(), nullptr, 10);
    }
    for (;;) {
        std::string req = read_file(REQ);
        unsigned long seq = strtoul(req.c_str(), nullptr, 10);
        size_t nl = req.find('\n');
        if (!seq || seq == done_seq || nl == std::string::npos) {
            usleep(100000);
            continue;
        }
        done_seq = seq;

        // the conversation: "role\x1fcontent\x1e" ...
        std::vector<std::string> roles, texts;
        for (size_t p = nl + 1; p < req.size();) {
            size_t us = req.find('\x1f', p), rs = req.find('\x1e', p);
            if (us == std::string::npos || rs == std::string::npos || us > rs) break;
            roles.push_back(req.substr(p, us - p));
            texts.push_back(req.substr(us + 1, rs - us - 1));
            p = rs + 1;
        }
        std::vector<llama_chat_message> msgs;
        for (size_t i = 0; i < roles.size(); i++) msgs.push_back({roles[i].c_str(), texts[i].c_str()});
        std::vector<char> fmt(16384);
        int len = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, fmt.data(), fmt.size());
        if (len > (int)fmt.size()) {
            fmt.resize(len);
            len = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, fmt.data(), fmt.size());
        }
        if (len < 0) {
            write_reply(seq, "error", "The local model's chat format could not be applied");
            continue;
        }
        std::string prompt(fmt.data(), len);

        // every request starts from an empty context: the conversation comes with it
        llama_memory_clear(llama_get_memory(ctx), true);
        int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), NULL, 0, true, true);
        std::vector<llama_token> toks(n_prompt);
        if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), toks.data(), toks.size(), true, true) < 0 ||
            n_prompt + 64 > (int)llama_n_ctx(ctx)) {
            write_reply(seq, "error", "The question and conversation are too long for the local model");
            continue;
        }
        write_reply(seq, "answer", "");
        std::string out;
        const llama_token *tokens = toks.data();
        int n_tokens = (int)toks.size(), pos = 0;
        llama_token tok;
        int failed = 0;
        for (int produced = 0; produced < 2048; produced++) {
            // the prompt in pieces of n_batch, then one token at a time
            while (n_tokens > 0) {
                int take = n_tokens > 512 ? 512 : n_tokens;
                batch_set_tokens(batch, tokens, take, pos);
                if (llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0) { failed = 1; break; }
                tokens += take;
                n_tokens -= take;
                pos += take;
            }
            if (failed || pos + 1 >= (int)llama_n_ctx(ctx)) break;
            tok = llama_sampler_sample(smpl, ctx, -1);
            if (llama_vocab_is_eog(vocab, tok)) break;
            char buf[256];
            int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf), 0, true);
            if (n > 0) out.append(buf, n);
            if (produced % 3 == 0) write_reply(seq, "answer", visible(out));
            tokens = &tok;
            n_tokens = 1;
        }
        write_reply(seq, failed ? "error" : "done", failed ? std::string("The local model failed while answering") : visible(out));
    }
}

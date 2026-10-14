#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fake_verb(uint8_t codec, uint8_t nid, uint16_t verb, uint16_t parm, uint32_t *out);
#define ICDA_HDA_CODEC_TEST 1
#define hda_exec_verb fake_verb
#include "../kernel/drivers/audio/hda.c"

typedef struct {
    int exists;
    uint32_t wcaps, pincaps, cfg;
    uint8_t conns[4], n;
    uint16_t sel, pinctl, eapd, stream, format;
    uint16_t input_gain[16], output_gain;
} node_t;
static node_t graph[2][256];
static unsigned wake_reads[2][256];
static uint64_t fake_us;
static int stuck_power;
uint64_t tsc_us(void) { return fake_us; }
void udelay(uint64_t us) { fake_us += us; }

static int fake_verb(uint8_t codec, uint8_t nid, uint16_t verb, uint16_t parm, uint32_t *out) {
    uint32_t value = 0;
    if (codec > 1) return -1;
    node_t *node = &graph[codec][nid];
    if (verb == HDA_VERB_SET_POWER_STATE) wake_reads[codec][nid] = 3;
    else if (verb == HDA_VERB_GET_POWER_STATE) {
        if (stuck_power || wake_reads[codec][nid]) value = 0x30;
        if (wake_reads[codec][nid]) wake_reads[codec][nid]--;
    } else if (verb == HDA_VERB_GET_PARAM) {
        if (nid == 0 && parm == HDA_PARAM_NODE_COUNT) value = (1U << 16) | 1;
        else if (nid == 1 && parm == HDA_PARAM_FUNC_TYPE) value = 1;
        else if (nid == 1 && parm == HDA_PARAM_NODE_COUNT) value = (2U << 16) | 30;
        else if (parm == HDA_PARAM_AMP_IN_CAP || parm == HDA_PARAM_AMP_OUT_CAP) value = 3 | (15U << 8) | (1U << 31);
        else if (!node->exists) return -1;
        else if (parm == HDA_PARAM_WIDGET_CAPS) value = node->wcaps;
        else if (parm == HDA_PARAM_CONN_LEN) value = node->n;
        else if (parm == HDA_PARAM_PIN_CAP) value = node->pincaps;
    } else if (verb == HDA_VERB_GET_CONN_LIST) {
        for (unsigned i = 0; i < 4 && parm + i < node->n; i++) value |= (uint32_t)node->conns[parm + i] << (i * 8);
    } else if (verb == HDA_VERB_GET_CFG_DEFAULT) value = node->cfg;
    else if (verb == HDA_VERB_SET_SEL_INPUT) node->sel = parm;
    else if (verb == HDA_VERB_SET_PIN_CONTROL) node->pinctl = parm;
    else if (verb == HDA_VERB_SET_EAPD) node->eapd = parm;
    else if (verb == HDA_VERB_SET_FORMAT) node->format = parm;
    else if (verb == HDA_VERB_SET_CONV_STREAM) node->stream = parm;
    else if (verb == HDA_VERB_SET_AMP_GAIN_MUTE) {
        if (parm & HDA_AMP_SET_INPUT) node->input_gain[(parm >> 8) & 15] = parm;
        else node->output_gain = parm;
    }
    if (out) *out = value;
    return 0;
}
static void node(unsigned codec, unsigned nid, unsigned type, uint32_t flags, int a, int b) {
    node_t *n = &graph[codec][nid];
    n->exists = 1; n->wcaps = (type << 20) | flags;
    if (a >= 0) n->conns[n->n++] = (uint8_t)a;
    if (b >= 0) n->conns[n->n++] = (uint8_t)b;
    for (unsigned i=0;i<16;i++) n->input_gain[i] = 0x80;
}
void serial_write(const char *s) { (void)s; }

int main(void) {
    /* A display codec is reported first; playback must choose the analog one. */
    node(0, 2, HDA_WIDGET_OUTPUT, 0, -1, -1);
    node(0, 3, HDA_WIDGET_PIN, 0, 2, -1);
    graph[0][3].pincaps = HDA_PINCAP_OUTPUT; graph[0][3].cfg = 4U << 20;
    node(1, 2, HDA_WIDGET_OUTPUT, HDA_WCAP_OUT_AMP, -1, -1);
    node(1, 3, HDA_WIDGET_OUTPUT, HDA_WCAP_OUT_AMP, -1, -1);
    node(1, 4, HDA_WIDGET_PIN, 0, 3, -1); /* disconnected speaker before the real one */
    graph[1][4].cfg = (1U << 30) | (1U << 20); graph[1][4].pincaps = HDA_PINCAP_OUTPUT;
    node(1, 5, HDA_WIDGET_PIN, 0, -1, -1);
    node(1, 6, HDA_WIDGET_PIN, 0, -1, -1);
    node(1, 0x10, HDA_WIDGET_SELECTOR, 0, 5, 0x0c);
    node(1, 0x0c, HDA_WIDGET_MIXER, HDA_WCAP_IN_AMP | HDA_WCAP_OUT_AMP, 6, 0x11);
    graph[1][0x0c].input_gain[0] = 0; /* firmware left analog loopback open */
    node(1, 0x11, HDA_WIDGET_SELECTOR, 0, 2, -1);
    node(1, 0x14, HDA_WIDGET_PIN, HDA_WCAP_OUT_AMP, 0x10, -1);
    graph[1][0x14].cfg = 0x90170110; graph[1][0x14].pincaps = HDA_PINCAP_OUTPUT | HDA_PINCAP_EAPD;
    node(1, 0x15, HDA_WIDGET_PIN, 0, 0x10, -1);
    graph[1][0x15].cfg = 2U << 20; graph[1][0x15].pincaps = HDA_PINCAP_OUTPUT | HDA_PINCAP_HP_DRV;
    node(1, 0x17, HDA_WIDGET_PIN, 0, 3, -1);
    graph[1][0x17].cfg = 0xaU << 20; /* microphone: never force it into output mode */
    assert(hda_choose_codec(3) == 0 && hda_codec == 1 && hda_pin == 0x14 && hda_dac == 2);
    assert(fake_us >= 600); /* graph probes wait for each codec to wake */
    assert(hda_configure_codec_path(48000) == 0);
    assert(graph[1][0x10].sel == 1);
    assert((graph[1][0x0c].input_gain[1] & 0x80) == 0);
    assert((graph[1][0x0c].input_gain[0] & 0x80) != 0);
    assert(graph[1][0x14].pinctl == HDA_PINCTL_OUT_ENABLE && graph[1][0x14].eapd == HDA_EAPD_ENABLE);
    assert(graph[1][0x15].pinctl == (HDA_PINCTL_OUT_ENABLE | HDA_PINCTL_HP_ENABLE));
    assert(graph[1][4].pinctl == 0 && graph[1][0x17].pinctl == 0);
    assert(graph[1][2].stream == 0x10 && graph[1][2].format == hda_format_for_rate(48000));
    assert(graph[1][3].stream == 0); /* unrelated DAC remains alone */
    assert(hda_amp_zero_db(70 | (5U << 8)) == 5);
    assert(hda_choose_codec(1) == 0 && hda_codec == 0); /* digital output is still a fallback */
    graph[0][3].cfg |= 1U << 30;
    assert(hda_choose_codec(1) < 0); /* no invented path when every output is disconnected */
    hda_codec = 1; stuck_power = 1;
    uint64_t started = fake_us;
    assert(hda_power_up(1) < 0 && fake_us - started == 20000);
    puts("HDA codec selection, speaker/headphone routing, selector preservation and amplifier checks passed");
    return 0;
}

#include "hda.h"

#include "../pci/pci.h"
#include "../console/console.h"
#include "../../memory/vmm.h"

#define HDA_CLASS_CODE         0x04
#define HDA_SUBCLASS           0x03

#define HDA_REG_GCAP           0x00
#define HDA_REG_STATESTS       0x0E
#define HDA_REG_GCTL           0x08
#define HDA_REG_INTCTL         0x20
#define HDA_REG_INTSTS         0x24
#define HDA_REG_ICOI           0x60
#define HDA_REG_ICII           0x64
#define HDA_REG_ICIS           0x68

#define HDA_GCTL_CRST          0x00000001U

#define HDA_ICIS_ICB           0x0001U
#define HDA_ICIS_IRV           0x0002U

#define HDA_SD_CTL_RUN         0x02U
#define HDA_SD_CTL_SRST        0x01U

#define HDA_WIDGET_OUTPUT      0x0
#define HDA_WIDGET_MIXER       0x2
#define HDA_WIDGET_SELECTOR    0x3
#define HDA_WIDGET_PIN         0x4

#define HDA_PARAM_NODE_COUNT   0x04
#define HDA_PARAM_FUNC_TYPE    0x05
#define HDA_PARAM_WIDGET_CAPS  0x09
#define HDA_PARAM_PIN_CAP      0x0C
#define HDA_PARAM_AMP_IN_CAP   0x0D
#define HDA_PARAM_CONN_LEN     0x0E
#define HDA_PARAM_AMP_OUT_CAP  0x12

#define HDA_VERB_GET_PARAM         0xF00
#define HDA_VERB_GET_CONN_LIST     0xF02
#define HDA_VERB_SET_SEL_INPUT     0x701
#define HDA_VERB_SET_POWER_STATE   0x705
#define HDA_VERB_SET_CONV_STREAM   0x706
#define HDA_VERB_SET_PIN_CONTROL   0x707
#define HDA_VERB_SET_EAPD          0x70C
#define HDA_VERB_SET_CHANNELS      0x72D
#define HDA_VERB_GET_CFG_DEFAULT   0xF1C
#define HDA_VERB_AFG_RESET         0x7FF
#define HDA_VERB_SET_FORMAT        0x200

#define HDA_PINCTL_OUT_ENABLE  0x40
#define HDA_PINCTL_HP_ENABLE   0x80
#define HDA_EAPD_ENABLE        0x02

#define HDA_WCAP_IN_AMP        (1U << 1)
#define HDA_WCAP_OUT_AMP       (1U << 2)
#define HDA_PINCAP_HP_DRV      (1U << 3)
#define HDA_PINCAP_EAPD        (1U << 16)

#define HDA_VERB_SET_AMP_GAIN_MUTE 0x300
#define HDA_AMP_SET_RIGHT      (1U << 12)
#define HDA_AMP_SET_LEFT       (1U << 13)
#define HDA_AMP_SET_INPUT      (1U << 14)
#define HDA_AMP_SET_OUTPUT     (1U << 15)

#define HDA_STREAM_TAG         1U
#define HDA_RING_BYTES         65536U
#define HDA_BDL_COUNT          16U
#define HDA_MAX_CONNECTIONS    32U
#define HDA_BDL_ALIGN          1024U
#define HDA_SEG_BYTES          (HDA_RING_BYTES / HDA_BDL_COUNT)
#define HDA_MMIO_MAP_BYTES     0x4000U

#define HDA_FMT_CHAN(v)        ((uint16_t)(((v) - 1U) & 0x0FU))
#define HDA_FMT_BITS_16        (1U << 4)
#define HDA_FMT_DIV(v)         ((uint16_t)(((v) - 1U) << 8))
#define HDA_FMT_MULT(v)        ((uint16_t)(((v) - 1U) << 11))
#define HDA_FMT_BASE_44K       (1U << 14)
#define HDA_FMT_BASE_48K       (0U << 14)

typedef struct __attribute__((packed, aligned(16))) {
    uint64_t addr;
    uint32_t length;
    uint32_t flags;
} hda_bdl_entry_t;

typedef struct {
    uint8_t nodes[16];
    uint8_t select_index[16];
    uint8_t length;
} hda_path_t;

static volatile uint8_t *hda_mmio = 0;
static const pci_device_t *hda_pci = 0;
static uint32_t hda_stream_base = 0;
static uint16_t hda_format = 0;
static uint32_t hda_buffer_len = 0;
static uint8_t hda_codec = 0;
static uint8_t hda_afg = 0;
static uint8_t hda_pin = 0;
static uint8_t hda_dac = 0;
static int hda_present = 0;
static int hda_error = 0;
static int hda_generic_fallback = 0;

static uint16_t hda_format_for_rate(uint16_t sample_rate);

static hda_bdl_entry_t hda_bdl[HDA_BDL_COUNT]
    __attribute__((aligned(HDA_BDL_ALIGN), section(".dma_low")));
static uint8_t hda_ring[HDA_RING_BYTES]
    __attribute__((aligned(128), section(".dma_low")));

static inline void mmio_write8(uint32_t off, uint8_t value) {
    *((volatile uint8_t *)(hda_mmio + off)) = value;
}

static inline void mmio_write16(uint32_t off, uint16_t value) {
    *((volatile uint16_t *)(hda_mmio + off)) = value;
}

static inline void mmio_write32(uint32_t off, uint32_t value) {
    *((volatile uint32_t *)(hda_mmio + off)) = value;
}

static inline uint8_t mmio_read8(uint32_t off) {
    return *((volatile uint8_t *)(hda_mmio + off));
}

static inline uint16_t mmio_read16(uint32_t off) {
    return *((volatile uint16_t *)(hda_mmio + off));
}

static inline uint32_t mmio_read32(uint32_t off) {
    return *((volatile uint32_t *)(hda_mmio + off));
}

static uint64_t kernel_buffer_phys(const void *ptr) {
    uint64_t virt = (uint64_t)ptr;
    uint64_t phys = vmm_virt_to_phys(vmm_kernel_address_space(), virt);
    if (phys) {
        return phys;
    }
    if (virt < PHYSICAL_BASE) {
        return virt;
    }
    return 0;
}

static void cpu_relax(void) {
    __asm__ volatile("" ::: "memory");
}

static int wait_mask32(uint32_t off, uint32_t mask, uint32_t expect) {
    for (uint32_t i = 0; i < 1000000U; i++) {
        if ((mmio_read32(off) & mask) == expect) {
            return 0;
        }
        cpu_relax();
    }
    return -1;
}

static int wait_mask16(uint32_t off, uint16_t mask, uint16_t expect) {
    for (uint32_t i = 0; i < 1000000U; i++) {
        if ((mmio_read16(off) & mask) == expect) {
            return 0;
        }
        cpu_relax();
    }
    return -1;
}

static uint32_t hda_build_cmd(uint8_t codec, uint8_t nid, uint16_t verb, uint16_t parm) {
    return ((uint32_t)codec << 28) |
           ((uint32_t)nid << 20) |
           ((uint32_t)verb << 8) |
           (uint32_t)parm;
}

static int hda_exec_verb(uint8_t codec, uint8_t nid, uint16_t verb, uint16_t parm, uint32_t *resp_out) {
    uint16_t status;

    if (!hda_mmio) {
        return -1;
    }

    if (wait_mask16(HDA_REG_ICIS, HDA_ICIS_ICB, 0) != 0) {
        return -1;
    }

    mmio_write16(HDA_REG_ICIS, HDA_ICIS_IRV);
    mmio_write32(HDA_REG_ICOI, hda_build_cmd(codec, nid, verb, parm));
    mmio_write16(HDA_REG_ICIS, HDA_ICIS_ICB);

    /* Bounded hard: a codec that is present but unresponsive must not
     * hold the boot hostage for minutes.  A healthy codec answers the
     * first poll, so the cap only bites on wedged hardware. */
    for (uint32_t i = 0; i < 200000U; i++) {
        status = mmio_read16(HDA_REG_ICIS);
        if ((status & HDA_ICIS_ICB) == 0 && (status & HDA_ICIS_IRV) != 0) {
            if (resp_out) {
                *resp_out = mmio_read32(HDA_REG_ICII);
            }
            mmio_write16(HDA_REG_ICIS, HDA_ICIS_IRV);
            return 0;
        }
        cpu_relax();
    }

    return -1;
}

static int hda_get_param(uint8_t nid, uint8_t param, uint32_t *resp_out) {
    return hda_exec_verb(hda_codec, nid, HDA_VERB_GET_PARAM, param, resp_out);
}

static int hda_widget_type(uint8_t nid, uint8_t *type_out) {
    uint32_t caps;
    if (hda_get_param(nid, HDA_PARAM_WIDGET_CAPS, &caps) != 0) {
        return -1;
    }
    *type_out = (uint8_t)((caps >> 20) & 0x0FU);
    return 0;
}

static int hda_widget_caps(uint8_t nid, uint32_t *caps_out) {
    return hda_get_param(nid, HDA_PARAM_WIDGET_CAPS, caps_out);
}

static uint8_t hda_amp_zero_db_gain(uint8_t nid, uint8_t input, uint8_t *ok_out) {
    uint32_t caps = 0;
    uint8_t gain = 0;
    uint8_t ok = 0;

    if (hda_get_param(nid, input ? HDA_PARAM_AMP_IN_CAP : HDA_PARAM_AMP_OUT_CAP, &caps) == 0) {
        gain = (uint8_t)(caps & 0x7FU);
        ok = 1;
    }

    if (ok_out) {
        *ok_out = ok;
    }
    return gain;
}

static void hda_unmute_output_amp(uint8_t nid) {
    uint8_t ok = 0;
    uint16_t gain = hda_amp_zero_db_gain(nid, 0, &ok);
    if (!ok) {
        return;
    }
    (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_AMP_GAIN_MUTE,
                        (uint16_t)(HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | gain), 0);
}

static void hda_unmute_input_amp(uint8_t nid, uint8_t index) {
    uint8_t ok = 0;
    uint16_t gain = hda_amp_zero_db_gain(nid, 1, &ok);
    if (!ok) {
        return;
    }
    (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_AMP_GAIN_MUTE,
                        (uint16_t)(HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                                   ((uint16_t)index << 8) | gain), 0);
}

static int hda_get_connections(uint8_t nid, uint8_t *out, uint8_t *count_out) {
    uint32_t parm;
    uint8_t count;
    uint8_t long_form;
    uint8_t conns = 0;
    uint8_t prev = 0;
    int overflow = 0;

    if (hda_get_param(nid, HDA_PARAM_CONN_LEN, &parm) != 0) {
        return -1;
    }

    count = (uint8_t)(parm & 0x7FU);
    long_form = (uint8_t)((parm >> 7) & 0x01U);
    if (count == 0) {
        *count_out = 0;
        return 0;
    }

    for (uint8_t idx = 0; idx < count;) {
        uint32_t resp = 0;
        if (hda_exec_verb(hda_codec, nid, HDA_VERB_GET_CONN_LIST, idx, &resp) != 0) {
            return -1;
        }

        /* Real codecs (e.g. Realtek ALC-series mixers) can report far
         * more connections than the caller's buffer holds, and ranges
         * expand on top of that.  Cap the output so a large connection
         * list cannot overflow the stack and corrupt the graph walk. */
        if (long_form) {
            for (uint8_t slot = 0; slot < 2 && idx < count && conns < HDA_MAX_CONNECTIONS; slot++, idx++) {
                uint16_t raw = (uint16_t)((resp >> (slot * 16U)) & 0xFFFFU);
                uint8_t range = (raw & 0x8000U) ? 1U : 0U;
                uint8_t val = (uint8_t)(raw & 0x00FFU);
                if (range && conns > 0 && val >= prev) {
                    for (uint8_t n = (uint8_t)(prev + 1U); n <= val && conns < HDA_MAX_CONNECTIONS; n++) {
                        out[conns++] = n;
                    }
                    if (conns >= HDA_MAX_CONNECTIONS) {
                        overflow = 1;
                    }
                } else {
                    out[conns++] = val;
                }
                prev = val;
            }
        } else {
            for (uint8_t slot = 0; slot < 4 && idx < count && conns < HDA_MAX_CONNECTIONS; slot++, idx++) {
                uint8_t raw = (uint8_t)((resp >> (slot * 8U)) & 0xFFU);
                uint8_t range = (raw & 0x80U) ? 1U : 0U;
                uint8_t val = (uint8_t)(raw & 0x7FU);
                if (range && conns > 0 && val >= prev) {
                    for (uint8_t n = (uint8_t)(prev + 1U); n <= val && conns < HDA_MAX_CONNECTIONS; n++) {
                        out[conns++] = n;
                    }
                    if (conns >= HDA_MAX_CONNECTIONS) {
                        overflow = 1;
                    }
                } else {
                    out[conns++] = val;
                }
                prev = val;
            }
        }
        if (overflow) {
            break;
        }
    }

    *count_out = conns;
    return 0;
}

static int hda_find_dac_path_from(uint8_t nid, uint8_t *visited, hda_path_t *path) {
    uint8_t type = 0;
    uint8_t conns[32];
    uint8_t count = 0;

    if (path->length >= sizeof(path->nodes) || visited[nid]) {
        return -1;
    }

    visited[nid] = 1;
    if (hda_widget_type(nid, &type) != 0) {
        visited[nid] = 0;
        return -1;
    }

    path->nodes[path->length] = nid;
    path->select_index[path->length] = 0;
    path->length++;

    if (type == HDA_WIDGET_OUTPUT) {
        return 0;
    }

    if (hda_get_connections(nid, conns, &count) == 0) {
        uint8_t path_index = (uint8_t)(path->length - 1U);
        for (uint8_t i = 0; i < count; i++) {
            path->select_index[path_index] = i;
            if (hda_find_dac_path_from(conns[i], visited, path) == 0) {
                return 0;
            }
        }
    }

    path->length--;
    visited[nid] = 0;
    return -1;
}

static int hda_pin_priority(uint32_t cfg_default) {
    uint8_t device = (uint8_t)((cfg_default >> 20) & 0x0FU);
    switch (device) {
        case 0x1: return 4; /* speaker */
        case 0x2: return 3; /* headphone */
        case 0x0: return 2; /* line out */
        case 0x4: return 1; /* SPDIF out */
        case 0x5: return 1; /* digital other out */
        default: return 0;
    }
}

static int hda_find_first_widget(uint8_t wanted_type, uint8_t *nid_out) {
    uint32_t parm;
    uint8_t start = 0;
    uint8_t count = 0;

    if (!nid_out) {
        return -1;
    }
    if (hda_afg == 0) {
        return -1;
    }
    if (hda_get_param(hda_afg, HDA_PARAM_NODE_COUNT, &parm) != 0) {
        return -1;
    }

    start = (uint8_t)((parm >> 16) & 0x7FU);
    count = (uint8_t)(parm & 0x7FU);
    for (uint8_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(start + i);
        uint8_t type = 0;
        if (hda_widget_type(nid, &type) == 0 && type == wanted_type) {
            *nid_out = nid;
            return 0;
        }
    }
    return -1;
}

static int hda_find_first_widget_global(uint8_t wanted_type, uint8_t *nid_out) {
    if (!nid_out) {
        return -1;
    }
    for (uint8_t nid = 1; nid < 0x40; nid++) {
        uint8_t type = 0;
        if (hda_widget_type(nid, &type) == 0 && type == wanted_type) {
            *nid_out = nid;
            return 0;
        }
    }
    return -1;
}

static int hda_find_output_path(hda_path_t *out_path) {
    uint32_t parm;
    uint8_t start = 0;
    uint8_t count = 0;
    int best_score = -1;
    hda_path_t best_path = {0};

    if (hda_get_param(0, HDA_PARAM_NODE_COUNT, &parm) == 0) {
        start = (uint8_t)((parm >> 16) & 0x7FU);
        count = (uint8_t)(parm & 0x7FU);
        for (uint8_t i = 0; i < count; i++) {
            uint8_t nid = (uint8_t)(start + i);
            uint32_t fg_type = 0;
            if (hda_get_param(nid, HDA_PARAM_FUNC_TYPE, &fg_type) == 0 && (fg_type & 0xFFU) == 0x01U) {
                hda_afg = nid;
                break;
            }
        }
    }
    if (!hda_afg) {
        for (uint8_t nid = 1; nid < 0x20; nid++) {
            uint32_t fg_type = 0;
            if (hda_get_param(nid, HDA_PARAM_FUNC_TYPE, &fg_type) == 0 && (fg_type & 0xFFU) == 0x01U) {
                hda_afg = nid;
                break;
            }
        }
    }
    if (!hda_afg) {
        hda_afg = 1;
    }
    if (!hda_afg) {
        return -1;
    }

    if (hda_exec_verb(hda_codec, hda_afg, HDA_VERB_AFG_RESET, 0, 0) != 0) {
        return -1;
    }
    (void)hda_exec_verb(hda_codec, hda_afg, HDA_VERB_SET_POWER_STATE, 0, 0);

    if (hda_get_param(hda_afg, HDA_PARAM_NODE_COUNT, &parm) != 0) {
        return -1;
    }

    start = (uint8_t)((parm >> 16) & 0x7FU);
    count = (uint8_t)(parm & 0x7FU);
    for (uint8_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(start + i);
        uint8_t type = 0;
        uint32_t cfg = 0;
        int score;
        uint8_t visited[256] = {0};
        hda_path_t path = {0};

        if (hda_widget_type(nid, &type) != 0 || type != HDA_WIDGET_PIN) {
            continue;
        }
        if (hda_exec_verb(hda_codec, nid, HDA_VERB_GET_CFG_DEFAULT, 0, &cfg) != 0) {
            cfg = 0;
        }
        score = hda_pin_priority(cfg);
        if (hda_find_dac_path_from(nid, visited, &path) != 0) {
            continue;
        }
        if (path.length < 2) {
            continue;
        }
        if (score > best_score) {
            best_score = score;
            best_path = path;
        }
    }

    if (best_score < 0) {
        /*
         * Some VBox HDA codec graphs do not expose a clean pin->DAC path
         * through the connection-list walk we use for QEMU. Fall back to a
         * coarse first-pin/first-output choice so the rest of the codec setup
         * can still attempt broad enablement.
         */
        uint8_t fallback_pin = 0;
        uint8_t fallback_dac = 0;

        if (hda_find_first_widget(HDA_WIDGET_PIN, &fallback_pin) != 0 &&
            hda_find_first_widget_global(HDA_WIDGET_PIN, &fallback_pin) != 0) {
            return -1;
        }
        if (hda_find_first_widget(HDA_WIDGET_OUTPUT, &fallback_dac) != 0 &&
            hda_find_first_widget_global(HDA_WIDGET_OUTPUT, &fallback_dac) != 0) {
            return -1;
        }

        for (uint8_t i = 0; i < (uint8_t)sizeof(best_path.nodes); i++) {
            best_path.nodes[i] = 0;
            best_path.select_index[i] = 0;
        }
        best_path.nodes[0] = fallback_pin;
        best_path.nodes[1] = fallback_dac;
        best_path.length = 2;
        best_score = 0;
    }

    *out_path = best_path;
    hda_pin = best_path.nodes[0];
    hda_dac = best_path.nodes[best_path.length - 1U];
    return 0;
}

static int hda_configure_codec_generic(uint16_t sample_rate) {
    static const uint8_t candidate_dacs[] = { 0x02, 0x03, 0x04, 0x05, 0x06 };
    static const uint8_t candidate_pins[] = { 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15 };
    uint8_t stream_id = HDA_STREAM_TAG;
    uint32_t pin_caps = 0;
    uint8_t pin_ctl = HDA_PINCTL_OUT_ENABLE;

    hda_format = hda_format_for_rate(sample_rate);

    for (uint32_t i = 0; i < sizeof(candidate_dacs); i++) {
        uint8_t nid = candidate_dacs[i];
        (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_POWER_STATE, 0, 0);
        (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_FORMAT, hda_format, 0);
        (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_CHANNELS, 1, 0);
        (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_CONV_STREAM, (uint16_t)(stream_id << 4), 0);
    }

    for (uint32_t i = 0; i < sizeof(candidate_pins); i++) {
        uint8_t nid = candidate_pins[i];
        pin_ctl = HDA_PINCTL_OUT_ENABLE;
        (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_POWER_STATE, 0, 0);
        if (hda_get_param(nid, HDA_PARAM_PIN_CAP, &pin_caps) == 0) {
            if (pin_caps & HDA_PINCAP_HP_DRV) {
                pin_ctl |= HDA_PINCTL_HP_ENABLE;
            }
            if (pin_caps & HDA_PINCAP_EAPD) {
                (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_EAPD, HDA_EAPD_ENABLE, 0);
            }
            (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_PIN_CONTROL, pin_ctl, 0);
        }
    }

    hda_dac = candidate_dacs[0];
    hda_pin = candidate_pins[0];
    return 0;
}

static uint16_t hda_format_for_rate(uint16_t sample_rate) {
    uint16_t rate_bits;

    switch (sample_rate) {
        case 8000:  rate_bits = HDA_FMT_BASE_48K | HDA_FMT_MULT(1) | HDA_FMT_DIV(6); break;
        case 11025: rate_bits = HDA_FMT_BASE_44K | HDA_FMT_MULT(1) | HDA_FMT_DIV(4); break;
        case 16000: rate_bits = HDA_FMT_BASE_48K | HDA_FMT_MULT(1) | HDA_FMT_DIV(3); break;
        case 22050: rate_bits = HDA_FMT_BASE_44K | HDA_FMT_MULT(1) | HDA_FMT_DIV(2); break;
        case 32000: rate_bits = HDA_FMT_BASE_48K | HDA_FMT_MULT(2) | HDA_FMT_DIV(3); break;
        case 44100: rate_bits = HDA_FMT_BASE_44K | HDA_FMT_MULT(1) | HDA_FMT_DIV(1); break;
        case 48000:
        default:
            rate_bits = HDA_FMT_BASE_48K | HDA_FMT_MULT(1) | HDA_FMT_DIV(1);
            break;
    }

    return (uint16_t)(rate_bits | HDA_FMT_BITS_16 | HDA_FMT_CHAN(2));
}

static int hda_configure_codec_path(uint16_t sample_rate) {
    hda_path_t path = {0};
    uint8_t conns[32];
    uint8_t conn_count = 0;
    uint8_t stream_id = HDA_STREAM_TAG;
    uint32_t pin_caps = 0;
    uint8_t pin_ctl = HDA_PINCTL_OUT_ENABLE;
    uint32_t parm = 0;
    uint8_t start = 0;
    uint8_t count = 0;

    if (hda_find_output_path(&path) != 0) {
        return hda_configure_codec_generic(sample_rate);
    }

    for (uint8_t i = 0; i < path.length; i++) {
        (void)hda_exec_verb(hda_codec, path.nodes[i], HDA_VERB_SET_POWER_STATE, 0, 0);
    }

    for (uint8_t i = 0; i + 1U < path.length; i++) {
        if (hda_get_connections(path.nodes[i], conns, &conn_count) == 0 && conn_count > 1U) {
            (void)hda_exec_verb(hda_codec, path.nodes[i], HDA_VERB_SET_SEL_INPUT, path.select_index[i], 0);
        }
    }

    hda_format = hda_format_for_rate(sample_rate);
    if (hda_exec_verb(hda_codec, hda_dac, HDA_VERB_SET_FORMAT, hda_format, 0) != 0) {
        return -1;
    }
    (void)hda_exec_verb(hda_codec, hda_dac, HDA_VERB_SET_CHANNELS, 1, 0);
    if (hda_exec_verb(hda_codec, hda_dac, HDA_VERB_SET_CONV_STREAM, (uint16_t)(stream_id << 4), 0) != 0) {
        return -1;
    }
    if (hda_get_param(hda_pin, HDA_PARAM_PIN_CAP, &pin_caps) == 0) {
        if (pin_caps & HDA_PINCAP_HP_DRV) {
            pin_ctl |= HDA_PINCTL_HP_ENABLE;
        }
        if (pin_caps & HDA_PINCAP_EAPD) {
            (void)hda_exec_verb(hda_codec, hda_pin, HDA_VERB_SET_EAPD, HDA_EAPD_ENABLE, 0);
        }
    }
    (void)hda_exec_verb(hda_codec, hda_pin, HDA_VERB_SET_PIN_CONTROL, pin_ctl, 0);

    if (hda_get_param(hda_afg, HDA_PARAM_NODE_COUNT, &parm) == 0) {
        start = (uint8_t)((parm >> 16) & 0x7FU);
        count = (uint8_t)(parm & 0x7FU);
        for (uint8_t i = 0; i < count; i++) {
            uint8_t nid = (uint8_t)(start + i);
            uint8_t type = 0;
            if (hda_widget_type(nid, &type) != 0) {
                continue;
            }

            (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_POWER_STATE, 0, 0);

            if (type == HDA_WIDGET_OUTPUT) {
                (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_FORMAT, hda_format, 0);
                (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_CHANNELS, 1, 0);
                (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_CONV_STREAM, (uint16_t)(stream_id << 4), 0);
            } else if (type == HDA_WIDGET_SELECTOR) {
                (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_SEL_INPUT, 0, 0);
            } else if (type == HDA_WIDGET_PIN) {
                uint8_t ctl = HDA_PINCTL_OUT_ENABLE;
                uint32_t caps_local = 0;
                if (hda_get_param(nid, HDA_PARAM_PIN_CAP, &caps_local) == 0) {
                    if (caps_local & HDA_PINCAP_HP_DRV) {
                        ctl |= HDA_PINCTL_HP_ENABLE;
                    }
                    if (caps_local & HDA_PINCAP_EAPD) {
                        (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_EAPD, HDA_EAPD_ENABLE, 0);
                    }
                }
                (void)hda_exec_verb(hda_codec, nid, HDA_VERB_SET_PIN_CONTROL, ctl, 0);
            }
        }
    }
    return 0;
}

static uint32_t hda_output_stream_base(void) {
    uint16_t gcap = mmio_read16(HDA_REG_GCAP);
    uint8_t iss = (uint8_t)((gcap >> 8) & 0x0FU);
    return 0x80U + (0x20U * iss);
}

static int hda_wait_for_codec_graph(void) {
    hda_path_t dummy = {0};
    uint32_t probe_resp = 0;

    /* Liveness check first.  Each failed verb costs up to 200k MMIO
     * polls, and a full graph walk issues hundreds of verbs, so a codec
     * that never answers would otherwise hold the boot silent for
     * minutes.  If the codec cannot answer a single root-node read, it
     * is wedged - skip the expensive walk entirely. */
    {
        int alive = 0;
        for (uint32_t retry = 0; retry < 8U; retry++) {
            if (hda_exec_verb(hda_codec, 0, HDA_VERB_GET_PARAM,
                              HDA_PARAM_NODE_COUNT, &probe_resp) == 0 &&
                probe_resp != 0xFFFFFFFFU) {
                alive = 1;
                break;
            }
            for (uint32_t spin = 0; spin < 10000U; spin++) {
                cpu_relax();
            }
        }
        if (!alive) {
            return -1;
        }
    }

    /* A live codec answers the graph walk quickly; a codec that misses
     * the first attempts will not magically appear later, so a couple of
     * retries are plenty. */
    for (uint32_t attempt = 0; attempt < 4U; attempt++) {
        if (hda_find_output_path(&dummy) == 0) {
            return 0;
        }
        for (uint32_t spin = 0; spin < 10000U; spin++) {
            cpu_relax();
        }
    }
    return -1;
}

static int hda_reset_stream(void) {
    uint32_t base = hda_stream_base;
    uint8_t ctl0;

    ctl0 = mmio_read8(base + 0x00U);
    ctl0 &= (uint8_t)~HDA_SD_CTL_RUN;
    mmio_write8(base + 0x00U, ctl0);

    mmio_write8(base + 0x00U, (uint8_t)(ctl0 | HDA_SD_CTL_SRST));
    if (wait_mask16(base + 0x00U, HDA_SD_CTL_SRST, HDA_SD_CTL_SRST) != 0) {
        return -1;
    }
    mmio_write8(base + 0x00U, ctl0);
    if (wait_mask16(base + 0x00U, HDA_SD_CTL_SRST, 0) != 0) {
        return -1;
    }
    mmio_write8(base + 0x03U, 0x1CU);
    return 0;
}

static void hda_build_bdl(uint32_t length) {
    uint64_t phys = kernel_buffer_phys(hda_ring);
    uint32_t offset = 0;

    if (length > HDA_RING_BYTES) {
        length = HDA_RING_BYTES;
    }

    for (uint32_t i = 0; i < HDA_BDL_COUNT; i++) {
        uint32_t chunk = HDA_SEG_BYTES;
        if (offset + chunk > length) {
            chunk = length - offset;
        }
        if (chunk == 0) {
            chunk = HDA_SEG_BYTES;
        }
        hda_bdl[i].addr = phys + offset;
        hda_bdl[i].length = chunk;
        hda_bdl[i].flags = 0;
        offset += chunk;
        if (offset >= length) {
            offset = 0;
        }
    }
}

int hda_init(void) {
    uint32_t bar0;
    uint32_t gctl;
    uint16_t statests;

    hda_present = 0;
    hda_error = 0;
    hda_mmio = 0;
    hda_stream_base = 0;
    hda_buffer_len = 0;
    hda_codec = 0xFFU;
    hda_afg = 0;
    hda_pin = 0;
    hda_dac = 0;
    hda_generic_fallback = 0;
    hda_pci = pci_find_class(HDA_CLASS_CODE, HDA_SUBCLASS);
    if (!hda_pci) {
        hda_error = 1;
        return -1;
    }

    if (pci_enable_memory_busmaster(hda_pci) != 0) {
        hda_error = 2;
        return -1;
    }

    bar0 = pci_read_config32(hda_pci, 0x10);
    if ((bar0 & 0xFFFFFFF0U) == 0) {
        hda_error = 3;
        return -1;
    }

    hda_mmio = (volatile uint8_t *)vmm_map_physical((uint64_t)(bar0 & 0xFFFFFFF0U), HDA_MMIO_MAP_BYTES, VMM_FLAGS_KERNEL_RW);
    if (!hda_mmio) {
        hda_error = 4;
        return -1;
    }

    gctl = mmio_read32(HDA_REG_GCTL);
    mmio_write32(HDA_REG_GCTL, gctl & ~HDA_GCTL_CRST);
    if (wait_mask32(HDA_REG_GCTL, HDA_GCTL_CRST, 0) != 0) {
        hda_error = 5;
        return -1;
    }
    mmio_write32(HDA_REG_GCTL, gctl | HDA_GCTL_CRST);
    if (wait_mask32(HDA_REG_GCTL, HDA_GCTL_CRST, HDA_GCTL_CRST) != 0) {
        hda_error = 6;
        return -1;
    }

    mmio_write32(HDA_REG_INTCTL, 0);
    mmio_write32(HDA_REG_INTSTS, 0xFFFFFFFFU);
    statests = 0;
    for (uint32_t attempt = 0; attempt < 128U; attempt++) {
        statests = mmio_read16(HDA_REG_STATESTS);
        if ((statests & 0x7FFFU) != 0) {
            break;
        }
        for (uint32_t spin = 0; spin < 50000U; spin++) {
            cpu_relax();
        }
    }
    if ((statests & 0x7FFFU) == 0) {
        hda_error = 7;
        return -1;
    }
    mmio_write16(HDA_REG_STATESTS, statests);

    for (uint8_t codec = 0; codec < 15; codec++) {
        if (statests & (1U << codec)) {
            hda_codec = codec;
            break;
        }
    }

    if (hda_codec == 0xFFU || hda_codec > 14) {
        hda_error = 8;
        return -1;
    }

    if (hda_wait_for_codec_graph() != 0) {
        hda_generic_fallback = 1;
    }

    hda_stream_base = hda_output_stream_base();
    hda_buffer_len = HDA_RING_BYTES;
    hda_present = 1;
    return 0;
}

int hda_available(void) {
    return hda_present;
}

int hda_last_error(void) {
    return hda_error;
}

int hda_stream_start_s16_stereo(uint16_t sample_rate, uint32_t buffer_len) {
    uint64_t bdl_phys = kernel_buffer_phys(hda_bdl);
    uint8_t ctl2;

    if (!hda_present) {
        return -1;
    }
    if (buffer_len == 0 || buffer_len > HDA_RING_BYTES) {
        return -1;
    }

    if (hda_configure_codec_path(sample_rate) != 0) {
        hda_error = 10;
        return -1;
    }

    hda_buffer_len = buffer_len;
    for (uint32_t i = 0; i < hda_buffer_len; i++) {
        hda_ring[i] = 0;
    }

    hda_build_bdl(hda_buffer_len);
    if (hda_reset_stream() != 0) {
        hda_error = 11;
        return -1;
    }

    mmio_write32(hda_stream_base + 0x18U, (uint32_t)(bdl_phys & 0xFFFFFFFFU));
    mmio_write32(hda_stream_base + 0x1CU, (uint32_t)(bdl_phys >> 32));
    mmio_write32(hda_stream_base + 0x08U, hda_buffer_len);
    mmio_write16(hda_stream_base + 0x0CU, (uint16_t)(HDA_BDL_COUNT - 1U));
    mmio_write16(hda_stream_base + 0x12U, hda_format);
    ctl2 = mmio_read8(hda_stream_base + 0x02U);
    ctl2 &= 0x0FU;
    ctl2 |= (uint8_t)(HDA_STREAM_TAG << 4);
    mmio_write8(hda_stream_base + 0x02U, ctl2);
    return 0;
}

int hda_stream_run(void) {
    if (!hda_present || !hda_stream_base) {
        return -1;
    }
    mmio_write8(hda_stream_base + 0x00U, (uint8_t)(mmio_read8(hda_stream_base + 0x00U) | HDA_SD_CTL_RUN));
    return 0;
}

int hda_stream_write(uint32_t offset, const uint8_t *samples, uint32_t length) {
    if (!hda_present || !samples || length == 0 || offset >= hda_buffer_len) {
        return -1;
    }
    for (uint32_t i = 0; i < length; i++) {
        hda_ring[(offset + i) % hda_buffer_len] = samples[i];
    }
    return 0;
}

void hda_stop_playback(void) {
    uint8_t ctl0;

    if (!hda_present || !hda_stream_base) {
        return;
    }

    ctl0 = mmio_read8(hda_stream_base + 0x00U);
    ctl0 &= (uint8_t)~HDA_SD_CTL_RUN;
    mmio_write8(hda_stream_base + 0x00U, ctl0);
    mmio_write8(hda_stream_base + 0x03U, 0x1CU);
}

uint32_t hda_debug_lpi_b(void) {
    if (!hda_present || !hda_stream_base) {
        return 0;
    }
    return mmio_read32(hda_stream_base + 0x04U);
}

uint8_t hda_debug_status(void) {
    if (!hda_present || !hda_stream_base) {
        return 0;
    }
    return mmio_read8(hda_stream_base + 0x03U);
}

uint8_t hda_debug_ctl0(void) {
    if (!hda_present || !hda_stream_base) {
        return 0;
    }
    return mmio_read8(hda_stream_base + 0x00U);
}

uint8_t hda_debug_ctl2(void) {
    if (!hda_present || !hda_stream_base) {
        return 0;
    }
    return mmio_read8(hda_stream_base + 0x02U);
}

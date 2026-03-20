#include "audio.h"
#include "pci.h"
#include "timer.h"
#include "../arch/io.h"
#include "../klog.h"
#include "../mm/slab.h"
#include <aevos/config.h>

/* ═══════════════════════════════════════════════════════════
 *  PC Speaker (via PIT channel 2)
 * ═══════════════════════════════════════════════════════════ */

#define PIT_BASE_FREQ    1193182UL
#define PIT_CHANNEL2     0x42
#define PIT_COMMAND      0x43
#define SPEAKER_PORT     0x61

void speaker_on(uint32_t freq_hz)
{
    if (freq_hz == 0) return;
    uint32_t div = PIT_BASE_FREQ / freq_hz;
    if (div == 0) div = 1;

    /* PIT channel 2, lobyte/hibyte, square wave */
    outb(PIT_COMMAND, 0xB6);
    outb(PIT_CHANNEL2, (uint8_t)(div & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((div >> 8) & 0xFF));

    /* Enable speaker (bits 0 = gate, 1 = data) */
    uint8_t val = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, val | 0x03);
}

void speaker_off(void)
{
    uint8_t val = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, val & ~0x03);
}

void speaker_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    speaker_on(freq_hz);
    timer_sleep_ms(duration_ms);
    speaker_off();
}

/* ═══════════════════════════════════════════════════════════
 *  Intel HD Audio (HDA)
 * ═══════════════════════════════════════════════════════════ */

#define HDA_PCI_CLASS    0x04
#define HDA_PCI_SUBCLASS 0x03

#define HDA_CORB_SIZE    256
#define HDA_RIRB_SIZE    256
#define HDA_DMA_BUF_SIZE (64 * 1024)
#define HDA_BDL_ENTRIES  32

static hda_device_t hda_dev;

/* ── MMIO helpers ── */

static inline uint8_t hda_read8(uint32_t off)
{
    return *(volatile uint8_t *)(hda_dev.base + off);
}

static inline uint16_t hda_read16(uint32_t off)
{
    return *(volatile uint16_t *)(hda_dev.base + off);
}

static inline uint32_t hda_read32(uint32_t off)
{
    return *(volatile uint32_t *)(hda_dev.base + off);
}

static inline void hda_write8(uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(hda_dev.base + off) = val;
}

static inline void hda_write16(uint32_t off, uint16_t val)
{
    *(volatile uint16_t *)(hda_dev.base + off) = val;
}

static inline void hda_write32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(hda_dev.base + off) = val;
}

hda_device_t *hda_get_device(void)
{
    return hda_dev.initialized ? &hda_dev : NULL;
}

/* ── Codec verb send/receive via CORB/RIRB ── */

static void hda_corb_write(uint32_t verb)
{
    hda_dev.corb_wp = (hda_dev.corb_wp + 1) % hda_dev.corb_entries;
    hda_dev.corb[hda_dev.corb_wp] = verb;
    hda_write16(HDA_REG_CORBWP, hda_dev.corb_wp);
}

static bool hda_rirb_read(uint64_t *response, uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < timeout_ms * 1000; i++) {
        uint16_t wp = hda_read16(HDA_REG_RIRBWP);
        if (wp != hda_dev.rirb_rp) {
            hda_dev.rirb_rp = (hda_dev.rirb_rp + 1) % hda_dev.rirb_entries;
            if (response)
                *response = hda_dev.rirb[hda_dev.rirb_rp];
            return true;
        }
        for (volatile int d = 0; d < 100; d++);
    }
    return false;
}

static bool hda_send_verb(uint32_t verb, uint32_t *response)
{
    hda_corb_write(verb);
    uint64_t rirb_resp = 0;
    bool ok = hda_rirb_read(&rirb_resp, 1000);
    if (ok && response)
        *response = (uint32_t)rirb_resp;
    return ok;
}

/*
 * Build a codec verb:
 *   [31:28] = codec addr (usually 0)
 *   [27:20] = NID (node ID)
 *   [19:0]  = verb + payload
 */
#define HDA_VERB(cad, nid, verb, payload) \
    (((uint32_t)(cad) << 28) | ((uint32_t)(nid) << 20) | ((uint32_t)(verb) << 8) | (payload))

#define HDA_VERB_SET_STREAM       0x706
#define HDA_VERB_SET_FORMAT       0x200  /* 16-bit verb */
#define HDA_VERB_SET_AMP_GAIN_OUT 0x300  /* 12-bit verb (SET_AMP_GAIN_MUTE) */
#define HDA_VERB_SET_PIN_WIDGET   0x707
#define HDA_VERB_SET_EAPD        0x70C
#define HDA_VERB_SET_POWER_STATE  0x705
#define HDA_VERB_GET_PARAM        0xF00

/* ── Reset controller ── */

static bool hda_reset(void)
{
    /* Assert reset */
    hda_write32(HDA_REG_GCTL, hda_read32(HDA_REG_GCTL) & ~HDA_GCTL_CRST);
    for (int i = 0; i < 100000; i++) {
        if (!(hda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST)) break;
    }

    /* De-assert reset */
    hda_write32(HDA_REG_GCTL, hda_read32(HDA_REG_GCTL) | HDA_GCTL_CRST);
    for (int i = 0; i < 100000; i++) {
        if (hda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) return true;
    }

    klog("[hda] reset failed\n");
    return false;
}

/* ── CORB/RIRB setup ── */

static bool hda_setup_corb_rirb(void)
{
    /* CORB size: try 256 entries */
    hda_dev.corb_entries = HDA_CORB_SIZE;
    hda_dev.corb = (uint32_t *)kcalloc(hda_dev.corb_entries, sizeof(uint32_t));
    if (!hda_dev.corb) return false;

    hda_write32(HDA_REG_CORBLBASE, (uint32_t)(uintptr_t)hda_dev.corb);
    hda_write32(HDA_REG_CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)hda_dev.corb >> 32));

    /* Reset CORB read pointer */
    hda_write16(HDA_REG_CORBRP, 0x8000);
    for (int i = 0; i < 100000; i++) {
        if (hda_read16(HDA_REG_CORBRP) & 0x8000) break;
    }
    hda_write16(HDA_REG_CORBRP, 0x0000);
    for (int i = 0; i < 100000; i++) {
        if (!(hda_read16(HDA_REG_CORBRP) & 0x8000)) break;
    }
    hda_dev.corb_wp = hda_read16(HDA_REG_CORBWP);

    /* Start CORB DMA */
    hda_write8(HDA_REG_CORBCTL, 0x02);

    /* RIRB */
    hda_dev.rirb_entries = HDA_RIRB_SIZE;
    hda_dev.rirb = (uint64_t *)kcalloc(hda_dev.rirb_entries, sizeof(uint64_t));
    if (!hda_dev.rirb) return false;

    hda_write32(HDA_REG_RIRBLBASE, (uint32_t)(uintptr_t)hda_dev.rirb);
    hda_write32(HDA_REG_RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)hda_dev.rirb >> 32));

    /* Reset RIRB write pointer */
    hda_write16(HDA_REG_RIRBWP, 0x8000);
    hda_dev.rirb_rp = 0;

    /* Start RIRB DMA */
    hda_write8(HDA_REG_RIRBCTL, 0x02);

    return true;
}

/* ── Encode HDA stream format register ──
 *
 * Bits [15]    = type (0=PCM)
 * Bits [14]    = base rate (0=48kHz, 1=44.1kHz)
 * Bits [13:11] = mult
 * Bits [10:8]  = div
 * Bits [7:4]   = bits per sample (001=16, 010=20, 011=24, 100=32)
 * Bits [3:0]   = channels - 1
 */
static uint16_t hda_encode_format(uint32_t rate, uint16_t channels, uint16_t bits)
{
    uint16_t fmt = 0;

    /* Base rate + multiplier/divider for common sample rates */
    switch (rate) {
    case 44100:  fmt |= (1 << 14); break;                          /* 44.1 kHz base */
    case 88200:  fmt |= (1 << 14) | (1 << 11); break;              /* 44.1 * 2 */
    case 48000:  break;                                              /* 48 kHz base */
    case 96000:  fmt |= (1 << 11); break;                           /* 48 * 2 */
    case 192000: fmt |= (2 << 11); break;                           /* 48 * 4 */
    case 16000:  fmt |= (2 << 8); break;                            /* 48 / 3 */
    case 8000:   fmt |= (5 << 8); break;                            /* 48 / 6 */
    default:     break;                                              /* fallback 48 kHz */
    }

    switch (bits) {
    case 16: fmt |= (1 << 4); break;
    case 20: fmt |= (2 << 4); break;
    case 24: fmt |= (3 << 4); break;
    case 32: fmt |= (4 << 4); break;
    default: fmt |= (1 << 4); break;
    }

    fmt |= (channels - 1) & 0x0F;
    return fmt;
}

/* ── Initialization ── */

bool hda_init(void)
{
    hda_dev.initialized = false;
    hda_dev.volume = 80;

    pci_device_t *pdev = pci_find_class(HDA_PCI_CLASS, HDA_PCI_SUBCLASS);
    if (!pdev) {
        klog("[hda] no HD Audio controller found\n");
        return false;
    }

    klog("[hda] found controller at PCI %u:%u.%u (vendor=%x device=%x)\n",
         pdev->bus, pdev->device, pdev->function,
         pdev->vendor_id, pdev->device_id);

    /* Enable bus mastering + memory space */
    uint32_t cmd = pci_read_config(pdev->bus, pdev->device, pdev->function, PCI_CFG_COMMAND);
    cmd |= (1 << 1) | (1 << 2);
    pci_write_config(pdev->bus, pdev->device, pdev->function, PCI_CFG_COMMAND, cmd);

    hda_dev.base = (volatile uint8_t *)(uintptr_t)pci_get_bar(pdev, 0);
    if (!hda_dev.base) {
        klog("[hda] BAR0 is null\n");
        return false;
    }

    if (!hda_reset()) return false;

    /* Wait for codec detection */
    for (int i = 0; i < 100000; i++) {
        if (hda_read16(HDA_REG_STATESTS) & 0x01) break;
    }
    if (!(hda_read16(HDA_REG_STATESTS) & 0x01)) {
        klog("[hda] no codec detected\n");
        return false;
    }

    /* Read capabilities */
    uint16_t gcap = hda_read16(HDA_REG_GCAP);
    hda_dev.oss = (gcap >> 12) & 0x0F;
    hda_dev.iss = (gcap >> 8) & 0x0F;
    hda_dev.bss = (gcap >> 3) & 0x1F;

    klog("[hda] GCAP=%x oss=%u iss=%u bss=%u\n",
         gcap, hda_dev.oss, hda_dev.iss, hda_dev.bss);

    if (hda_dev.oss == 0) {
        klog("[hda] no output streams available\n");
        return false;
    }
    hda_dev.out_stream = hda_dev.iss;

    if (!hda_setup_corb_rirb()) {
        klog("[hda] CORB/RIRB setup failed\n");
        return false;
    }

    hda_dev.initialized = true;
    klog("[hda] HD Audio initialized\n");
    return true;
}

/* ── Setup output stream ── */

bool hda_setup_output(uint32_t sample_rate, uint16_t channels, uint16_t bits)
{
    if (!hda_dev.initialized) return false;

    int sd = hda_dev.out_stream;
    uint32_t sd_base = HDA_SD_BASE(sd);

    /* Stop stream */
    hda_write8(sd_base + HDA_SD_CTL, 0);
    for (int i = 0; i < 100000; i++) {
        if (!(hda_read8(sd_base + HDA_SD_CTL) & HDA_SD_CTL_RUN)) break;
    }

    /* Clear status */
    hda_write8(sd_base + HDA_SD_STS, 0x1C);

    /* Allocate DMA buffer */
    hda_dev.dma_buf_size = HDA_DMA_BUF_SIZE;
    if (!hda_dev.dma_buf) {
        hda_dev.dma_buf = (int16_t *)kcalloc(1, hda_dev.dma_buf_size);
        if (!hda_dev.dma_buf) return false;
    }

    /* Allocate BDL */
    if (!hda_dev.bdl) {
        hda_dev.bdl = (hda_bdl_entry_t *)kcalloc(HDA_BDL_ENTRIES, sizeof(hda_bdl_entry_t));
        if (!hda_dev.bdl) return false;
    }

    /* Fill BDL: single entry pointing to entire DMA buffer */
    hda_dev.bdl[0].addr   = (uint64_t)(uintptr_t)hda_dev.dma_buf;
    hda_dev.bdl[0].length = hda_dev.dma_buf_size;
    hda_dev.bdl[0].ioc    = 1;

    /* Set stream format */
    uint16_t fmt = hda_encode_format(sample_rate, channels, bits);
    hda_write16(sd_base + HDA_SD_FMT, fmt);

    /* Set CBL (cyclic buffer length) */
    hda_write32(sd_base + HDA_SD_CBL, hda_dev.dma_buf_size);

    /* Set LVI (last valid index) */
    hda_write16(sd_base + HDA_SD_LVI, 0);

    /* Set BDL address */
    hda_write32(sd_base + HDA_SD_BDPL, (uint32_t)(uintptr_t)hda_dev.bdl);
    hda_write32(sd_base + HDA_SD_BDPU, (uint32_t)((uint64_t)(uintptr_t)hda_dev.bdl >> 32));

    /* Configure DAC widget (NID 2 typically) for stream 1 */
    uint8_t stream_tag = (uint8_t)(sd - hda_dev.iss + 1);
    uint32_t verb;

    /* Set stream/channel on DAC (NID=0x02, typical for many codecs) */
    verb = HDA_VERB(0, 0x02, HDA_VERB_SET_STREAM, (stream_tag << 4) | 0);
    hda_send_verb(verb, NULL);

    /* Set format on DAC */
    verb = ((uint32_t)0 << 28) | ((uint32_t)0x02 << 20) | ((uint32_t)HDA_VERB_SET_FORMAT << 8) | 0;
    verb = (0u << 28) | (0x02u << 20) | (0x2000u) | fmt;
    hda_send_verb(verb, NULL);

    /* Set stream CTL: stream tag */
    uint32_t ctl = hda_read32(sd_base + HDA_SD_CTL);
    ctl &= ~(0xF << 20);
    ctl |= HDA_SD_CTL_STREAM(stream_tag);
    hda_write32(sd_base + HDA_SD_CTL, ctl);

    klog("[hda] output stream %d configured: rate=%u ch=%u bits=%u fmt=%x\n",
         sd, sample_rate, channels, bits, fmt);
    return true;
}

/* ── Playback ── */

bool hda_play_buffer(const int16_t *samples, size_t count, uint32_t sample_rate)
{
    if (!hda_dev.initialized || !samples || count == 0)
        return false;

    if (!hda_setup_output(sample_rate, 2, 16))
        return false;

    /* Copy samples into DMA buffer */
    size_t bytes = count * sizeof(int16_t);
    if (bytes > hda_dev.dma_buf_size)
        bytes = hda_dev.dma_buf_size;

    int16_t *dst = hda_dev.dma_buf;
    for (size_t i = 0; i < bytes / sizeof(int16_t); i++)
        dst[i] = samples[i];

    /* Update BDL entry length */
    hda_dev.bdl[0].length = (uint32_t)bytes;

    int sd = hda_dev.out_stream;
    uint32_t sd_base = HDA_SD_BASE(sd);

    /* Update CBL */
    hda_write32(sd_base + HDA_SD_CBL, (uint32_t)bytes);

    /* Start stream */
    uint32_t ctl = hda_read32(sd_base + HDA_SD_CTL);
    ctl |= HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
    hda_write32(sd_base + HDA_SD_CTL, ctl);

    klog("[hda] playing %u samples at %u Hz\n", (uint32_t)(bytes / sizeof(int16_t)), sample_rate);
    return true;
}

void hda_stop(void)
{
    if (!hda_dev.initialized) return;

    int sd = hda_dev.out_stream;
    uint32_t sd_base = HDA_SD_BASE(sd);

    uint32_t ctl = hda_read32(sd_base + HDA_SD_CTL);
    ctl &= ~(HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE);
    hda_write32(sd_base + HDA_SD_CTL, ctl);

    klog("[hda] stream stopped\n");
}

void hda_set_volume(uint8_t volume)
{
    if (!hda_dev.initialized) return;
    if (volume > 100) volume = 100;
    hda_dev.volume = volume;

    uint8_t gain = (uint8_t)((uint32_t)volume * 127 / 100);

    uint32_t verb = (0u << 28) | (0x02u << 20) | (0x3u << 16)
                  | (1u << 15) | (1u << 13) | (1u << 12) | gain;
    hda_send_verb(verb, NULL);

    verb = (0u << 28) | (0x03u << 20) | (0x3u << 16)
         | (1u << 15) | (1u << 13) | (1u << 12) | gain;
    hda_send_verb(verb, NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  Tone Generator — sine wave via HDA or PC speaker fallback
 * ═══════════════════════════════════════════════════════════ */

static void generate_sine_tone(int16_t *buf, size_t samples,
                               uint32_t freq, uint32_t sample_rate, uint8_t vol)
{
    /* Taylor-approximated sine: avoids floating point in kernel */
    for (size_t i = 0; i < samples; i++) {
        /* phase angle: 0..65535 maps to 0..2*pi */
        uint32_t phase = (uint32_t)((uint64_t)i * freq * 65536 / sample_rate) & 0xFFFF;
        int32_t x = (int32_t)phase - 32768;
        /* triangle wave approximation of sine (±32768 range) */
        int32_t val;
        if (x < 0) x = -x;
        val = 32768 - x * 2;
        /* scale by volume */
        val = val * (int32_t)vol / 100;
        /* apply fade-in/fade-out envelope */
        if (i < 512)
            val = val * (int32_t)i / 512;
        if (i > samples - 512)
            val = val * (int32_t)(samples - i) / 512;
        buf[i * 2]     = (int16_t)val;     /* left */
        buf[i * 2 + 1] = (int16_t)val;     /* right */
    }
}

void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume)
{
    if (hda_dev.initialized) {
        uint32_t rate = 48000;
        size_t samples = (size_t)(rate * duration_ms / 1000);
        if (samples > HDA_DMA_BUF_SIZE / (2 * sizeof(int16_t)))
            samples = HDA_DMA_BUF_SIZE / (2 * sizeof(int16_t));

        int16_t *tone_buf = (int16_t *)kmalloc(samples * 2 * sizeof(int16_t));
        if (tone_buf) {
            generate_sine_tone(tone_buf, samples, freq_hz, rate, volume);
            hda_play_buffer(tone_buf, samples * 2, rate);
            kfree(tone_buf);
            return;
        }
    }
    speaker_beep(freq_hz, duration_ms);
}

void audio_play_boot_chime(void)
{
    audio_play_tone(880, 100, 60);
    timer_sleep_ms(120);
    audio_play_tone(1320, 100, 50);
    timer_sleep_ms(120);
    audio_play_tone(1760, 200, 40);
}

void audio_play_error_tone(void)
{
    audio_play_tone(200, 300, 80);
    timer_sleep_ms(100);
    audio_play_tone(150, 300, 80);
}

void audio_play_click(void)
{
    audio_play_tone(4000, 10, 30);
}

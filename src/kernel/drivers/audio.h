#pragma once

#include <aevos/types.h>

/* ═══════════════════════ PC Speaker ═══════════════════════ */

void speaker_on(uint32_t freq_hz);
void speaker_off(void);
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);

/* ═══════════════════════ Intel HD Audio ═══════════════════ */

/* HDA controller global registers */
#define HDA_REG_GCAP      0x00
#define HDA_REG_VMIN      0x02
#define HDA_REG_VMAJ      0x03
#define HDA_REG_OUTPAY    0x04
#define HDA_REG_INPAY     0x06
#define HDA_REG_GCTL      0x08
#define HDA_REG_WAKEEN    0x0C
#define HDA_REG_STATESTS   0x0E
#define HDA_REG_GSTS      0x10
#define HDA_REG_INTCTL    0x20
#define HDA_REG_INTSTS    0x24

/* CORB / RIRB */
#define HDA_REG_CORBLBASE 0x40
#define HDA_REG_CORBUBASE 0x44
#define HDA_REG_CORBWP    0x48
#define HDA_REG_CORBRP    0x4A
#define HDA_REG_CORBCTL   0x4C
#define HDA_REG_CORBSTS   0x4D
#define HDA_REG_CORBSIZE  0x4E
#define HDA_REG_RIRBLBASE 0x50
#define HDA_REG_RIRBUBASE 0x54
#define HDA_REG_RIRBWP    0x58
#define HDA_REG_RINTCNT   0x5A
#define HDA_REG_RIRBCTL   0x5C
#define HDA_REG_RIRBSTS   0x5D
#define HDA_REG_RIRBSIZE  0x5E

/* Output stream descriptor base (stream 0) – offset depends on GCAP */
#define HDA_SD_BASE(n)    (0x80 + (n) * 0x20)
#define HDA_SD_CTL        0x00
#define HDA_SD_STS        0x03
#define HDA_SD_LPIB       0x04
#define HDA_SD_CBL        0x08
#define HDA_SD_LVI        0x0C
#define HDA_SD_FMT        0x12
#define HDA_SD_BDPL       0x18
#define HDA_SD_BDPU       0x1C

/* GCTL bits */
#define HDA_GCTL_CRST     (1 << 0)

/* Stream descriptor CTL bits */
#define HDA_SD_CTL_RUN    (1 << 1)
#define HDA_SD_CTL_IOCE   (1 << 2)
#define HDA_SD_CTL_STRIPE1 0
#define HDA_SD_CTL_STREAM(n) ((n) << 20)

/* BDL entry */
typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t ioc;
} PACKED hda_bdl_entry_t;

typedef struct {
    volatile uint8_t *base;
    uint32_t         *corb;
    uint64_t         *rirb;
    uint16_t          corb_entries;
    uint16_t          rirb_entries;
    uint16_t          corb_wp;
    uint16_t          rirb_rp;
    uint8_t           oss;  /* number of output streams */
    uint8_t           iss;  /* number of input streams */
    uint8_t           bss;  /* bidirectional streams */
    int               out_stream;
    hda_bdl_entry_t  *bdl;
    int16_t          *dma_buf;
    uint32_t          dma_buf_size;
    uint8_t           volume;
    bool              initialized;
} hda_device_t;

bool hda_init(void);
bool hda_setup_output(uint32_t sample_rate, uint16_t channels, uint16_t bits);
bool hda_play_buffer(const int16_t *samples, size_t count, uint32_t sample_rate);
void hda_stop(void);
void hda_set_volume(uint8_t volume);
hda_device_t *hda_get_device(void);

/* ═══════════════════════ Tone Generator ═══════════════════ */

void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume);
void audio_play_boot_chime(void);
void audio_play_error_tone(void);
void audio_play_click(void);

/*
 * Chiptune DSP Plugin for Move Anything
 *
 * NES 2A03 and Game Boy DMG APU emulation for chiptune synthesis.
 * Plugin API v2 - instance-based for multi-instance support.
 *
 * Uses Nes_Snd_Emu (Shay Green) for NES APU emulation
 * and gb_apu (TotalJustice) for Game Boy APU emulation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <new>

/* Plugin API definitions */
extern "C" {

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"

} /* extern "C" */

/* NES APU */
#include "nes_apu/Nes_Apu.h"
#include "nes_apu/Blip_Buffer.h"

/* GB APU (blargg's Gb_Snd_Emu via C wrapper) */
extern "C" {
#include "gb_apu_wrapper.h"
}

/* Parameter helper */
#include "param_helper.h"

/* =====================================================================
 * Constants
 * ===================================================================== */

#define NES_CPU_CLOCK   1789773
#define GB_CPU_CLOCK    4194304
#define SAMPLE_RATE     44100
#define FRAMES_PER_BLOCK 128
#define MAX_VOICES      5
#define NUM_PRESETS      32
#define NUM_WAVETABLES   8

/* NES cycles per audio block: 128 * 1789773 / 44100 */
#define NES_CYCLES_PER_BLOCK ((FRAMES_PER_BLOCK * NES_CPU_CLOCK + SAMPLE_RATE / 2) / SAMPLE_RATE)
/* GB cycles per audio block: 128 * 4194304 / 44100 */
#define GB_CYCLES_PER_BLOCK  ((FRAMES_PER_BLOCK * GB_CPU_CLOCK + SAMPLE_RATE / 2) / SAMPLE_RATE)

/* Chip types */
#define CHIP_NES 0
#define CHIP_GB  1

/* Allocation modes */
#define ALLOC_AUTO   0
#define ALLOC_LEAD   1
#define ALLOC_LOCKED 2

/* Channel types */
#define CHAN_PULSE1   0
#define CHAN_PULSE2   1
#define CHAN_TRIANGLE 2
#define CHAN_WAVE     2  /* GB wave = channel index 2, same slot as triangle */
#define CHAN_NOISE    3
#define CHAN_DMC      4  /* NES only, not used for voices */

/* Envelope stages (ADSR) */
#define ENV_IDLE    0
#define ENV_ATTACK  1
#define ENV_DECAY   2
#define ENV_SUSTAIN 3
#define ENV_RELEASE 4

/* =====================================================================
 * Host API reference
 * ===================================================================== */

static const host_api_v1_t *g_host = NULL;

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[chiptune] %s", msg);
        g_host->log(buf);
    }
}

/* =====================================================================
 * Parameter definitions
 * ===================================================================== */

enum ChiptuneParam {
    P_DUTY = 0,
    P_ENV_ATTACK,
    P_ENV_DECAY,
    P_ENV_SUSTAIN,
    P_ENV_RELEASE,
    P_SWEEP,
    P_VIBRATO_DEPTH,
    P_VIBRATO_RATE,
    P_NOISE_MODE,
    P_WAVETABLE,
    P_CHANNEL_MASK,
    P_DETUNE,
    P_VOLUME,
    P_OCTAVE_TRANSPOSE,
    P_ALLOC_MODE,
    P_PITCH_ENV_DEPTH,
    P_PITCH_ENV_SPEED,
    P_COUNT
};

static const param_def_t g_param_defs[] = {
    {"duty",             "Duty Cycle",    PARAM_TYPE_INT,   P_DUTY,             0.0f, 3.0f},
    {"env_attack",       "Attack",        PARAM_TYPE_INT,   P_ENV_ATTACK,       0.0f, 15.0f},
    {"env_decay",        "Decay",         PARAM_TYPE_INT,   P_ENV_DECAY,        0.0f, 15.0f},
    {"env_sustain",      "Sustain",       PARAM_TYPE_INT,   P_ENV_SUSTAIN,      0.0f, 15.0f},
    {"env_release",      "Release",       PARAM_TYPE_INT,   P_ENV_RELEASE,      0.0f, 15.0f},
    {"sweep",            "Sweep",         PARAM_TYPE_INT,   P_SWEEP,            0.0f, 7.0f},
    {"vibrato_depth",    "Vibrato Depth", PARAM_TYPE_INT,   P_VIBRATO_DEPTH,    0.0f, 12.0f},
    {"vibrato_rate",     "Vibrato Rate",  PARAM_TYPE_INT,   P_VIBRATO_RATE,     0.0f, 10.0f},
    {"noise_mode",       "Noise Mode",    PARAM_TYPE_INT,   P_NOISE_MODE,       0.0f, 1.0f},
    {"wavetable",        "Wavetable (GB)",PARAM_TYPE_INT,   P_WAVETABLE,        0.0f, 7.0f},
    {"channel_mask",     "Channel Mask",  PARAM_TYPE_INT,   P_CHANNEL_MASK,     0.0f, 15.0f},
    {"detune",           "Detune",        PARAM_TYPE_INT,   P_DETUNE,           0.0f, 50.0f},
    {"volume",           "Volume",        PARAM_TYPE_INT,   P_VOLUME,           0.0f, 15.0f},
    {"octave_transpose", "Octave",        PARAM_TYPE_INT,   P_OCTAVE_TRANSPOSE, -3.0f, 3.0f},
    {"alloc_mode",       "Voice Mode",    PARAM_TYPE_INT,   P_ALLOC_MODE,       0.0f, 2.0f},
    {"pitch_env_depth",  "PEnv Depth",    PARAM_TYPE_INT,   P_PITCH_ENV_DEPTH,  0.0f, 24.0f},
    {"pitch_env_speed",  "PEnv Speed",    PARAM_TYPE_INT,   P_PITCH_ENV_SPEED,  0.0f, 15.0f},
};

/* =====================================================================
 * GB Wavetables
 * ===================================================================== */

static const uint8_t g_wavetables[NUM_WAVETABLES][16] = {
    /* 0: Sawtooth */
    {0xFF,0xEE,0xDD,0xCC,0xBB,0xAA,0x99,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00},
    /* 1: Square */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 2: Triangle */
    {0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF,0xFE,0xDC,0xBA,0x98,0x76,0x54,0x32,0x10},
    /* 3: Sine-ish */
    {0x89,0xBC,0xDE,0xEF,0xFF,0xFE,0xED,0xCB,0xA8,0x76,0x43,0x21,0x10,0x01,0x12,0x34},
    /* 4: Pulse */
    {0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00},
    /* 5: Bass */
    {0xFD,0xEC,0xDB,0xCA,0xB9,0xA8,0x97,0x86,0x75,0x64,0x53,0x42,0x31,0x20,0x10,0x00},
    /* 6: Growl */
    {0xFF,0xDD,0xBB,0x99,0xFF,0xDD,0xBB,0x99,0x77,0x55,0x33,0x11,0x77,0x55,0x33,0x11},
    /* 7: Metallic */
    {0xF0,0xF0,0x00,0x00,0xF0,0xF0,0x00,0x00,0xF0,0x00,0xF0,0x00,0x0F,0x0F,0x0F,0x0F},
};

/* =====================================================================
 * Factory presets
 * ===================================================================== */

struct chiptune_preset_t {
    const char *name;
    uint8_t chip;
    uint8_t alloc_mode;
    uint8_t duty;
    uint8_t env_attack;
    uint8_t env_decay;
    uint8_t env_sustain;       /* Sustain level 0-15 (0=off/AD only, 15=full) */
    uint8_t env_release;       /* Release time 0-15 (0=instant, 15=long) */
    uint8_t sweep;
    uint8_t vibrato_depth;
    uint8_t vibrato_rate;
    uint8_t noise_mode;
    uint8_t wavetable_idx;
    uint8_t channel_mask;
    uint8_t detune;
    uint8_t volume;
    uint8_t pitch_env_depth;   /* Semitones above base note at attack (0-24) */
    uint8_t pitch_env_speed;   /* How fast pitch drops back (0=off, 1=fast..15=slow) */
};

/*
 * Presets inspired by classic NES/GB game sounds.
 * Fields: name, chip, alloc_mode, duty, env_attack, env_decay, env_sustain, env_release,
 *         sweep, vibrato_depth, vibrato_rate, noise_mode, wavetable_idx,
 *         channel_mask, detune, volume, pitch_env_depth, pitch_env_speed
 *
 * Duty: 0=12.5% (thin/nasal), 1=25% (bright), 2=50% (warm/round), 3=75% (=25%)
 * ADSR: A 0=instant..15=~250ms; D 0=instant..15=~1s; S 0=off(AD)..15=full; R 0=instant..15=long
 * Channel mask: bit0=pulse1/sq1, bit1=pulse2/sq2, bit2=tri/wave, bit3=noise
 *   0x01=mono sq1, 0x03=2-note poly, 0x07=3-note poly, 0x0F=4-note poly
 * Detune >0 with mask 0x03: auto-doubles note to both pulse channels for unison
 * Pitch env: depth=semitones above note at attack, speed=decay rate (1=fast, 15=slow)
 */
static const chiptune_preset_t g_factory_presets[NUM_PRESETS] = {
    /*                                      du at dc su re sw vD vR nM wT mask det vol pD pS */
    /* ==== NES presets (0-15) ==== */
    /* Warm 50% lead, full sustain */
    /*  0 */ {"NES Lead",          CHIP_NES, ALLOC_LEAD,   2, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x01,  0, 15, 0, 0},
    /* Bright 25% lead */
    /*  1 */ {"NES Bright",        CHIP_NES, ALLOC_LEAD,   1, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x01,  0, 14, 0, 0},
    /* Thin nasal 12.5% lead */
    /*  2 */ {"NES Thin",          CHIP_NES, ALLOC_LEAD,   0, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x01,  0, 14, 0, 0},
    /* Slow swell pad with vibrato */
    /*  3 */ {"NES Pad",           CHIP_NES, ALLOC_LEAD,   2, 6,  5,12, 8, 0, 3, 5, 0, 0, 0x01,  0, 12, 0, 0},
    /* Short pluck, no sustain */
    /*  4 */ {"NES Pluck",         CHIP_NES, ALLOC_LEAD,   1, 0,  3, 0, 0, 0, 0, 0, 0, 0, 0x01,  0, 15, 0, 0},
    /* Very short stab */
    /*  5 */ {"NES Stab",          CHIP_NES, ALLOC_LEAD,   0, 0,  1, 0, 0, 0, 0, 0, 0, 0, 0x01,  0, 15, 0, 0},
    /* 3-note poly: warm 50%, both pulses + triangle */
    /*  6 */ {"NES Poly",          CHIP_NES, ALLOC_AUTO,   2, 0,  3,15, 5, 0, 0, 0, 0, 0, 0x07,  0, 13, 0, 0},
    /* 3-note bright poly: 25% duty, snappier */
    /*  7 */ {"NES Poly Bright",   CHIP_NES, ALLOC_AUTO,   1, 0,  2,12, 3, 0, 0, 0, 0, 0, 0x07,  0, 14, 0, 0},
    /* Thick detuned unison */
    /*  8 */ {"NES Unison",        CHIP_NES, ALLOC_AUTO,   2, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x03,  8, 13, 0, 0},
    /* Slow brass: attack swell, rich, full sustain */
    /*  9 */ {"NES Brass",         CHIP_NES, ALLOC_LEAD,   2, 4,  2,15, 6, 0, 0, 0, 0, 0, 0x01,  0, 14, 0, 0},
    /* Triangle bass with punch */
    /* 10 */ {"Tri Bass",          CHIP_NES, ALLOC_LEAD,   2, 0,  6,10, 3, 0, 0, 0, 0, 0, 0x04,  0, 15, 0, 0},
    /* Kick: triangle with pitch drop */
    /* 11 */ {"Tri Kick",          CHIP_NES, ALLOC_LEAD,   2, 0,  2, 0, 0, 0, 0, 0, 0, 0, 0x04,  0, 15, 24, 1},
    /* Bell: thin duty, medium decay, slight sustain */
    /* 12 */ {"NES Bell",          CHIP_NES, ALLOC_LEAD,   0, 0,  8, 4, 5, 0, 0, 0, 0, 0, 0x01,  0, 13, 0, 0},
    /* Closed hi-hat: short noise */
    /* 13 */ {"NES Hat",           CHIP_NES, ALLOC_LEAD,   0, 0,  1, 0, 0, 0, 0, 0, 1, 0, 0x08,  0, 15, 0, 0},
    /* Snare: white noise */
    /* 14 */ {"NES Snare",         CHIP_NES, ALLOC_LEAD,   0, 0,  5, 0, 0, 0, 0, 0, 0, 0, 0x08,  0, 15, 0, 0},
    /* Zap: noise with pitch drop */
    /* 15 */ {"NES Zap",           CHIP_NES, ALLOC_LEAD,   0, 0,  3, 0, 0, 0, 0, 0, 1, 0, 0x08,  0, 15, 12, 2},

    /* ==== GB presets (16-31) ==== */
    /* Classic GB warm lead */
    /* 16 */ {"GB Lead",           CHIP_GB,  ALLOC_LEAD,   2, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x01,  0, 14, 0, 0},
    /* Bright 25% lead */
    /* 17 */ {"GB Bright",         CHIP_GB,  ALLOC_LEAD,   1, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x01,  0, 14, 0, 0},
    /* Thin nasal 12.5% */
    /* 18 */ {"GB Thin",           CHIP_GB,  ALLOC_LEAD,   0, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x01,  0, 14, 0, 0},
    /* 3-note poly: warm 50%, squares + wave */
    /* 19 */ {"GB Poly",           CHIP_GB,  ALLOC_AUTO,   2, 0,  3,15, 5, 0, 0, 0, 0, 0, 0x07,  0, 13, 0, 0},
    /* 3-note bright poly: 25%, snappier envelope */
    /* 20 */ {"GB Poly Bright",    CHIP_GB,  ALLOC_AUTO,   1, 0,  2,12, 3, 0, 0, 0, 0, 0, 0x07,  0, 14, 0, 0},
    /* Detuned unison on both squares */
    /* 21 */ {"GB Unison",         CHIP_GB,  ALLOC_AUTO,   2, 0,  3,15, 4, 0, 0, 0, 0, 0, 0x03,  8, 13, 0, 0},
    /* Vibrato melody */
    /* 22 */ {"GB Vibrato",        CHIP_GB,  ALLOC_LEAD,   2, 0,  3,15, 5, 0, 4, 6, 0, 0, 0x01,  0, 13, 0, 0},
    /* Short pluck, no sustain */
    /* 23 */ {"GB Pluck",          CHIP_GB,  ALLOC_LEAD,   0, 0,  3, 0, 0, 0, 0, 0, 0, 0, 0x01,  0, 15, 0, 0},
    /* Slow pad with vibrato */
    /* 24 */ {"GB Pad",            CHIP_GB,  ALLOC_LEAD,   2, 6,  5,12, 8, 0, 3, 5, 0, 0, 0x01,  0, 12, 0, 0},
    /* Wave bass: sawtooth */
    /* 25 */ {"Wave Bass",         CHIP_GB,  ALLOC_LEAD,   2, 0,  4,12, 3, 0, 0, 0, 0, 0, 0x04,  0, 15, 0, 0},
    /* Wave pad: sine, slow attack, vibrato */
    /* 26 */ {"Wave Pad",          CHIP_GB,  ALLOC_LEAD,   2, 6,  5,12, 8, 0, 2, 4, 0, 3, 0x04,  0, 15, 0, 0},
    /* Wave sub: triangle, long sustain */
    /* 27 */ {"Wave Sub",          CHIP_GB,  ALLOC_LEAD,   2, 0,  0,15, 6, 0, 0, 0, 0, 2, 0x04,  0, 15, 0, 0},
    /* Growl: aggressive bass */
    /* 28 */ {"Wave Growl",        CHIP_GB,  ALLOC_LEAD,   2, 0,  6, 0, 0, 0, 0, 0, 0, 6, 0x04,  0, 15, 0, 0},
    /* Metallic wave texture */
    /* 29 */ {"Wave Metal",        CHIP_GB,  ALLOC_LEAD,   2, 0,  8, 0, 0, 0, 0, 0, 0, 7, 0x04,  0, 15, 0, 0},
    /* GB brass: slow attack, full sustain */
    /* 30 */ {"GB Brass",          CHIP_GB,  ALLOC_LEAD,   2, 4,  2,15, 6, 0, 0, 0, 0, 0, 0x01,  0, 14, 0, 0},
    /* GB bell: thin duty, ringing decay */
    /* 31 */ {"GB Bell",           CHIP_GB,  ALLOC_LEAD,   0, 0,  8, 4, 5, 0, 0, 0, 0, 0, 0x01,  0, 13, 0, 0},
};

/* =====================================================================
 * Voice and envelope structures
 * ===================================================================== */

struct voice_envelope_t {
    float level;
    int stage;          /* ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE */
    float attack_inc;   /* per-sample increment during attack */
    float decay_dec;    /* per-sample decrement during decay */
    float sustain_level; /* sustain level 0.0-1.0 */
    float release_dec;  /* per-sample decrement during release */
};

struct voice_t {
    int active;
    int note;          /* MIDI note (after octave transpose) */
    int velocity;
    int channel_idx;   /* Which APU channel this voice is on (0-3) */
    int channel_type;  /* CHAN_PULSE1, CHAN_PULSE2, CHAN_TRIANGLE/WAVE, CHAN_NOISE */
    int age;
    int triggered;     /* 1 = already triggered this note, skip re-trigger */
    voice_envelope_t env;
    float pitch_env;   /* Current pitch offset in semitones (decays toward 0) */
};

/* =====================================================================
 * Instance structure
 * ===================================================================== */

typedef struct {
    char module_dir[256];

    /* Chip selection */
    uint8_t chip;  /* CHIP_NES or CHIP_GB */

    /* NES APU */
    Nes_Apu nes_apu;
    Blip_Buffer nes_blip;

    /* GB APU (blargg) */
    gb_apu_wrapper_t *gb_apu;

    /* Voice allocator */
    voice_t voices[MAX_VOICES];
    int voice_age_counter;

    /* LFO */
    float lfo_phase;  /* 0.0 to 1.0 */

    /* Pitch bend */
    float pitch_bend_semitones;

    /* Parameters */
    float params[P_COUNT];
    int current_preset;
    char preset_name[64];

    /* Temp buffers */
    int16_t nes_mono_buf[FRAMES_PER_BLOCK + 64];
    int16_t gb_stereo_buf[(FRAMES_PER_BLOCK + 64) * 2];
} chiptune_instance_t;

/* =====================================================================
 * Utility functions
 * ===================================================================== */

static float midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

/* NES pulse period from frequency: period = 1789773 / (16 * freq) - 1 */
static int nes_pulse_period(float freq) {
    if (freq < 1.0f) freq = 1.0f;
    int period = (int)(NES_CPU_CLOCK / (16.0f * freq) - 1.0f + 0.5f);
    if (period < 0) period = 0;
    if (period > 0x7FF) period = 0x7FF;
    return period;
}

/* NES triangle period from frequency: period = 1789773 / (32 * freq) - 1 */
static int nes_triangle_period(float freq) {
    if (freq < 1.0f) freq = 1.0f;
    int period = (int)(NES_CPU_CLOCK / (32.0f * freq) - 1.0f + 0.5f);
    if (period < 0) period = 0;
    if (period > 0x7FF) period = 0x7FF;
    return period;
}

/* NES noise period lookup: MIDI note to noise period index (0=highest pitch, 15=lowest)
 * Move pads send notes 68-99 (32 notes). Spread 16 periods across this range
 * so every 2 adjacent pads get a different pitch. */
static int nes_noise_period_from_note(int note) {
    /* Map pad range 68-99 to period indices 15 (low) down to 0 (high) */
    int idx = 15 - ((note - 68) / 2);
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return idx;
}

/* GB square frequency register from frequency: reg = 2048 - (131072 / freq) */
static int gb_square_freq_reg(float freq) {
    if (freq < 1.0f) freq = 1.0f;
    int reg = (int)(2048.0f - 131072.0f / freq + 0.5f);
    if (reg < 0) reg = 0;
    if (reg > 2047) reg = 2047;
    return reg;
}

/* GB wave frequency register from frequency: reg = 2048 - (65536 / freq) */
static int gb_wave_freq_reg(float freq) {
    if (freq < 1.0f) freq = 1.0f;
    int reg = (int)(2048.0f - 65536.0f / freq + 0.5f);
    if (reg < 0) reg = 0;
    if (reg > 2047) reg = 2047;
    return reg;
}

/* GB noise frequency from MIDI note
 * Move pads send notes 68-99. We use a lookup table that maps each pair of
 * adjacent notes to a unique (shift, divisor) combination for maximum variety.
 * GB noise freq = 524288 / (divisor * 2^(shift+1)) Hz, where divisor=8*(code) for code>0.
 * With 14 shifts * 8 divisors = 112 unique frequencies, we pick 16 well-spaced ones. */
static void gb_noise_params_from_note(int note, int short_mode, uint8_t *out_reg) {
    /* 16 entries: shift (4 bits) and divisor code (3 bits), from low pitch to high */
    static const uint8_t noise_table[16][2] = {
        {13, 1}, /* 0  - very low rumble */
        {12, 1}, /* 1 */
        {11, 1}, /* 2 */
        {10, 1}, /* 3 */
        { 9, 1}, /* 4 */
        { 8, 1}, /* 5 */
        { 7, 1}, /* 6 */
        { 6, 1}, /* 7 - mid */
        { 5, 1}, /* 8 */
        { 4, 1}, /* 9 */
        { 3, 1}, /* 10 */
        { 3, 0}, /* 11 - divisor 0 = special (freq/2) */
        { 2, 1}, /* 12 */
        { 2, 0}, /* 13 */
        { 1, 1}, /* 14 */
        { 0, 1}, /* 15 - highest pitch hiss */
    };
    int idx = (note - 68) / 2;
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    uint8_t shift = noise_table[idx][0];
    uint8_t div_code = noise_table[idx][1];
    *out_reg = (uint8_t)((shift << 4) | (short_mode ? 0x08 : 0x00) | (div_code & 0x07));
}

/* JSON helpers (minimal, same pattern as braids) */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    if (*pos != '"') return -1;
    pos++;
    int i = 0;
    while (*pos && *pos != '"' && i < out_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i;
}

/* =====================================================================
 * Envelope functions
 * ===================================================================== */

static void env_init(voice_envelope_t *env) {
    env->level = 0.0f;
    env->stage = ENV_IDLE;
    env->attack_inc = 0.0f;
    env->decay_dec = 0.0f;
    env->sustain_level = 0.0f;
    env->release_dec = 0.0f;
}

static void env_configure(voice_envelope_t *env, int attack_param, int decay_param,
                           int sustain_param, int release_param) {
    /* Attack: param 0 = instant, 1-15 = progressively slower
     * At param=0, instant attack (1 sample). At param=15, ~0.25 seconds. */
    if (attack_param <= 0) {
        env->attack_inc = 1.0f; /* instant */
    } else {
        float attack_samples = attack_param * (SAMPLE_RATE / 60.0f);
        env->attack_inc = 1.0f / attack_samples;
    }

    /* Decay: param 0 = instant, 1-15 = progressively slower
     * At param=0, instant. At param=15, ~1 second. */
    if (decay_param <= 0) {
        env->decay_dec = 1.0f; /* instant */
    } else {
        float decay_samples = decay_param * (SAMPLE_RATE / 15.0f);
        env->decay_dec = 1.0f / decay_samples;
    }

    /* Sustain: 0 = no sustain (AD envelope), 15 = full level */
    env->sustain_level = (float)sustain_param / 15.0f;

    /* Release: param 0 = instant, 1-15 = progressively slower
     * At param=0, instant. At param=15, ~1 second. */
    if (release_param <= 0) {
        env->release_dec = 1.0f; /* instant */
    } else {
        float release_samples = release_param * (SAMPLE_RATE / 15.0f);
        env->release_dec = 1.0f / release_samples;
    }
}

static void env_gate_on(voice_envelope_t *env) {
    env->stage = ENV_ATTACK;
    /* Don't reset level - allows retriggering */
}

static void env_gate_off(voice_envelope_t *env) {
    if (env->stage != ENV_IDLE) {
        env->stage = ENV_RELEASE;
    }
}

/* Advance envelope by one sample, return level 0.0-1.0 */
static float env_process(voice_envelope_t *env) {
    switch (env->stage) {
        case ENV_ATTACK:
            env->level += env->attack_inc;
            if (env->level >= 1.0f) {
                env->level = 1.0f;
                env->stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY:
            env->level -= env->decay_dec;
            if (env->level <= env->sustain_level) {
                env->level = env->sustain_level;
                if (env->sustain_level > 0.0f) {
                    env->stage = ENV_SUSTAIN;
                } else {
                    env->stage = ENV_IDLE; /* AD mode: no sustain */
                }
            }
            break;
        case ENV_SUSTAIN:
            /* Hold at sustain level until gate off */
            break;
        case ENV_RELEASE:
            env->level -= env->release_dec;
            if (env->level <= 0.0f) {
                env->level = 0.0f;
                env->stage = ENV_IDLE;
            }
            break;
        case ENV_IDLE:
        default:
            env->level = 0.0f;
            break;
    }
    return env->level;
}

/* =====================================================================
 * APU initialization helpers
 * ===================================================================== */

static void init_nes_apu(chiptune_instance_t *inst) {
    inst->nes_blip.clock_rate(NES_CPU_CLOCK);
    inst->nes_blip.set_sample_rate(SAMPLE_RATE);
    inst->nes_blip.clear();
    inst->nes_apu.set_output(&inst->nes_blip);
    inst->nes_apu.reset(false, 0);
    /* Enable all channels */
    inst->nes_apu.write_register(0, 0x4015, 0x0F);
}

static void init_gb_apu(chiptune_instance_t *inst) {
    if (inst->gb_apu) {
        gb_apu_wrapper_destroy(inst->gb_apu);
    }
    inst->gb_apu = gb_apu_wrapper_create(SAMPLE_RATE);
    /* Master enable, volume, and routing are set by the wrapper */
}

/* =====================================================================
 * Preset application
 * ===================================================================== */

static void apply_preset(chiptune_instance_t *inst, int idx) {
    if (idx < 0 || idx >= NUM_PRESETS) return;

    const chiptune_preset_t *p = &g_factory_presets[idx];

    inst->chip = p->chip;
    inst->params[P_DUTY] = (float)p->duty;
    inst->params[P_ENV_ATTACK] = (float)p->env_attack;
    inst->params[P_ENV_DECAY] = (float)p->env_decay;
    inst->params[P_ENV_SUSTAIN] = (float)p->env_sustain;
    inst->params[P_ENV_RELEASE] = (float)p->env_release;
    inst->params[P_SWEEP] = (float)p->sweep;
    inst->params[P_VIBRATO_DEPTH] = (float)p->vibrato_depth;
    inst->params[P_VIBRATO_RATE] = (float)p->vibrato_rate;
    inst->params[P_NOISE_MODE] = (float)p->noise_mode;
    inst->params[P_WAVETABLE] = (float)p->wavetable_idx;
    inst->params[P_CHANNEL_MASK] = (float)p->channel_mask;
    inst->params[P_DETUNE] = (float)p->detune;
    inst->params[P_VOLUME] = (float)p->volume;
    inst->params[P_OCTAVE_TRANSPOSE] = 0.0f;
    inst->params[P_ALLOC_MODE] = (float)p->alloc_mode;
    inst->params[P_PITCH_ENV_DEPTH] = (float)p->pitch_env_depth;
    inst->params[P_PITCH_ENV_SPEED] = (float)p->pitch_env_speed;

    inst->current_preset = idx;
    snprintf(inst->preset_name, sizeof(inst->preset_name), "%s", p->name);
}

/* =====================================================================
 * Voice allocation
 * ===================================================================== */

static void kill_all_voices(chiptune_instance_t *inst) {
    for (int i = 0; i < MAX_VOICES; i++) {
        inst->voices[i].active = 0;
        env_init(&inst->voices[i].env);
    }
}

/* Determine which APU channel to assign for a new voice */
static int pick_channel(chiptune_instance_t *inst, int note) {
    int mask = (int)inst->params[P_CHANNEL_MASK];
    int alloc = (int)inst->params[P_ALLOC_MODE];

    if (alloc == ALLOC_LOCKED) {
        /* Find first available channel in mask not currently in use */
        for (int ch = 0; ch < 4; ch++) {
            if (!(mask & (1 << ch))) continue;
            int in_use = 0;
            for (int v = 0; v < MAX_VOICES; v++) {
                if (inst->voices[v].active && inst->voices[v].channel_idx == ch) {
                    in_use = 1;
                    break;
                }
            }
            if (!in_use) return ch;
        }
        /* All channels in mask are in use, steal from oldest */
        int oldest_voice = -1;
        int oldest_age = 0x7FFFFFFF;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (inst->voices[v].active && inst->voices[v].age < oldest_age) {
                int ch = inst->voices[v].channel_idx;
                if (mask & (1 << ch)) {
                    oldest_age = inst->voices[v].age;
                    oldest_voice = v;
                }
            }
        }
        if (oldest_voice >= 0) return inst->voices[oldest_voice].channel_idx;
        /* Fallback */
        for (int ch = 0; ch < 4; ch++) {
            if (mask & (1 << ch)) return ch;
        }
        return 0;
    }

    if (alloc == ALLOC_LEAD) {
        /* Monophonic: always use first channel in mask */
        for (int ch = 0; ch < 4; ch++) {
            if (mask & (1 << ch)) return ch;
        }
        return 0;
    }

    /* AUTO mode */
    /* Noise channel for very high notes */
    if (note > 96 && (mask & 0x08)) {
        return 3; /* noise */
    }

    /* Prefer pulse channels for melody */
    for (int ch = 0; ch < 2; ch++) {
        if (!(mask & (1 << ch))) continue;
        int in_use = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (inst->voices[v].active && inst->voices[v].channel_idx == ch) {
                in_use = 1;
                break;
            }
        }
        if (!in_use) return ch;
    }

    /* Try triangle/wave */
    if (mask & 0x04) {
        int in_use = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (inst->voices[v].active && inst->voices[v].channel_idx == 2) {
                in_use = 1;
                break;
            }
        }
        if (!in_use) return 2;
    }

    /* Try noise */
    if (mask & 0x08) {
        int in_use = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (inst->voices[v].active && inst->voices[v].channel_idx == 3) {
                in_use = 1;
                break;
            }
        }
        if (!in_use) return 3;
    }

    /* All busy - steal oldest on a pulse channel */
    int oldest_voice = -1;
    int oldest_age = 0x7FFFFFFF;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (inst->voices[v].active && inst->voices[v].age < oldest_age) {
            oldest_age = inst->voices[v].age;
            oldest_voice = v;
        }
    }
    if (oldest_voice >= 0) return inst->voices[oldest_voice].channel_idx;

    return 0;
}

static int allocate_voice(chiptune_instance_t *inst) {
    /* Find inactive voice */
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!inst->voices[i].active) return i;
    }
    /* Steal oldest */
    int oldest = 0;
    int oldest_age = inst->voices[0].age;
    for (int i = 1; i < MAX_VOICES; i++) {
        if (inst->voices[i].age < oldest_age) {
            oldest_age = inst->voices[i].age;
            oldest = i;
        }
    }
    return oldest;
}

static int find_voice_for_note(chiptune_instance_t *inst, int note) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (inst->voices[i].active && inst->voices[i].note == note) {
            return i;
        }
    }
    return -1;
}

/* =====================================================================
 * NES APU register writing
 * ===================================================================== */

static void nes_write_pulse(chiptune_instance_t *inst, int chan_idx, int time,
                            int duty, int vol, float freq, int do_trigger) {
    /* chan_idx: 0 = pulse1 ($4000-$4003), 1 = pulse2 ($4004-$4007) */
    uint16_t base = (chan_idx == 0) ? 0x4000 : 0x4004;

    int period = nes_pulse_period(freq);
    /* $4000/$4004: duty | length counter halt | constant volume | volume */
    uint8_t reg0 = (uint8_t)(((duty & 0x03) << 6) | 0x30 | (vol & 0x0F));

    inst->nes_apu.write_register(time, base + 0, reg0);
    /* $4002/$4006: period low (safe to write every block) */
    inst->nes_apu.write_register(time + 1, base + 2, (uint8_t)(period & 0xFF));
    if (do_trigger) {
        /* $4001/$4005: sweep disabled */
        inst->nes_apu.write_register(time + 2, base + 1, 0x00);
        /* $4003/$4007: length counter load | period high
         * This resets the phase sequencer - only do it on note-on */
        inst->nes_apu.write_register(time + 3, base + 3,
            (uint8_t)(0xF8 | ((period >> 8) & 0x07)));
    }
}

static void nes_write_triangle(chiptune_instance_t *inst, int time, int gate, float freq, int do_trigger) {
    int period = nes_triangle_period(freq);
    /* $4008: linear counter (0x7F = max length, bit 7 = control) */
    uint8_t reg8 = gate ? 0xFF : 0x80;

    inst->nes_apu.write_register(time, 0x4008, reg8);
    /* $400A: period low (safe to write every block) */
    inst->nes_apu.write_register(time + 1, 0x400A, (uint8_t)(period & 0xFF));
    if (do_trigger) {
        /* $400B: length counter load | period high (resets linear counter) */
        inst->nes_apu.write_register(time + 2, 0x400B,
            (uint8_t)(0xF8 | ((period >> 8) & 0x07)));
    }
}

static void nes_write_noise(chiptune_instance_t *inst, int time, int vol, int note, int short_mode, int do_trigger) {
    int period_idx = nes_noise_period_from_note(note);
    /* $400C: length halt | constant volume | volume */
    uint8_t regC = (uint8_t)(0x30 | (vol & 0x0F));
    /* $400E: mode | period */
    uint8_t regE = (uint8_t)((short_mode ? 0x80 : 0x00) | (period_idx & 0x0F));

    inst->nes_apu.write_register(time, 0x400C, regC);
    inst->nes_apu.write_register(time + 1, 0x400E, regE);
    if (do_trigger) {
        /* $400F: length counter load */
        inst->nes_apu.write_register(time + 2, 0x400F, 0xF8);
    }
}

static void nes_silence_channel(chiptune_instance_t *inst, int chan_idx, int time) {
    switch (chan_idx) {
        case 0:
            inst->nes_apu.write_register(time, 0x4000, 0x30); /* vol=0, constant */
            break;
        case 1:
            inst->nes_apu.write_register(time, 0x4004, 0x30);
            break;
        case 2:
            inst->nes_apu.write_register(time, 0x4008, 0x80); /* halt, counter=0 */
            break;
        case 3:
            inst->nes_apu.write_register(time, 0x400C, 0x30);
            break;
    }
}

/* =====================================================================
 * GB APU register writing
 * ===================================================================== */

static void gb_load_wavetable(chiptune_instance_t *inst, int wave_idx, unsigned time) {
    if (wave_idx < 0 || wave_idx >= NUM_WAVETABLES) wave_idx = 0;

    /* Disable wave channel before writing wave RAM.
     * Use the same time value for all writes — blargg's emulator processes
     * them in call order regardless, and incrementing time would advance
     * last_time, causing asserts if render_block starts at time 0. */
    gb_apu_wrapper_write(inst->gb_apu, 0xFF1A, 0x00, time);
    /* Write 16 bytes of wave RAM ($FF30-$FF3F) */
    for (int i = 0; i < 16; i++) {
        gb_apu_wrapper_write(inst->gb_apu, 0xFF30 + i, g_wavetables[wave_idx][i], time);
    }
    /* Re-enable wave channel */
    gb_apu_wrapper_write(inst->gb_apu, 0xFF1A, 0x80, time);
}

static void gb_write_square1(chiptune_instance_t *inst, unsigned time,
                             int duty, int vol, float freq, int sweep, int do_trigger) {
    int freq_reg = gb_square_freq_reg(freq);
    if (do_trigger) {
        /* Full register setup + trigger on note-on */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF12, (uint8_t)(((vol & 0x0F) << 4) | 0x00), time);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF13, (uint8_t)(freq_reg & 0xFF), time + 1);
        uint8_t sweep_reg = 0x00;
        if (sweep > 0) {
            sweep_reg = (uint8_t)(((sweep & 0x07) << 4) | 0x02);
        }
        gb_apu_wrapper_write(inst->gb_apu, 0xFF10, sweep_reg, time + 2);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF11, (uint8_t)(((duty & 0x03) << 6) | 0x3F), time + 3);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF14, (uint8_t)(0x80 | ((freq_reg >> 8) & 0x07)), time + 4);
    } else {
        /* Just update frequency (for vibrato/pitch bend) — no trigger */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF13, (uint8_t)(freq_reg & 0xFF), time);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF14, (uint8_t)((freq_reg >> 8) & 0x07), time + 1);
    }
}

static void gb_write_square2(chiptune_instance_t *inst, unsigned time,
                             int duty, int vol, float freq, int do_trigger) {
    int freq_reg = gb_square_freq_reg(freq);
    if (do_trigger) {
        /* Full register setup + trigger on note-on */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF17, (uint8_t)(((vol & 0x0F) << 4) | 0x00), time);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF18, (uint8_t)(freq_reg & 0xFF), time + 1);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF16, (uint8_t)(((duty & 0x03) << 6) | 0x3F), time + 2);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF19, (uint8_t)(0x80 | ((freq_reg >> 8) & 0x07)), time + 3);
    } else {
        /* Just update frequency — no trigger */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF18, (uint8_t)(freq_reg & 0xFF), time);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF19, (uint8_t)((freq_reg >> 8) & 0x07), time + 1);
    }
}

static void gb_write_wave(chiptune_instance_t *inst, unsigned time, int vol, float freq, int do_trigger) {
    int freq_reg = gb_wave_freq_reg(freq);
    /* GB wave volume: 0=mute, 1=100%, 2=50%, 3=25% */
    int wave_vol;
    if (vol >= 12) wave_vol = 1;       /* 100% */
    else if (vol >= 8) wave_vol = 2;   /* 50% */
    else if (vol >= 4) wave_vol = 3;   /* 25% */
    else wave_vol = 0;                 /* mute */

    /* $FF1C: volume select (safe every block) */
    gb_apu_wrapper_write(inst->gb_apu, 0xFF1C, (uint8_t)((wave_vol & 0x03) << 5), time);
    /* $FF1D: freq low (safe every block) */
    gb_apu_wrapper_write(inst->gb_apu, 0xFF1D, (uint8_t)(freq_reg & 0xFF), time + 1);
    if (do_trigger) {
        /* $FF1A: DAC enable */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF1A, 0x80, time + 2);
        /* $FF1E: trigger | freq high */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF1E, (uint8_t)(0x80 | ((freq_reg >> 8) & 0x07)), time + 3);
    } else {
        /* Just update freq high without trigger */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF1E, (uint8_t)((freq_reg >> 8) & 0x07), time + 2);
    }
}

static void gb_write_noise(chiptune_instance_t *inst, unsigned time, int vol, int note, int short_mode, int do_trigger) {
    uint8_t poly_reg;
    gb_noise_params_from_note(note, short_mode, &poly_reg);

    if (do_trigger) {
        /* Full register setup + trigger on note-on */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF21, (uint8_t)(((vol & 0x0F) << 4) | 0x00), time);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF22, poly_reg, time + 1);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF20, 0x3F, time + 2);
        gb_apu_wrapper_write(inst->gb_apu, 0xFF23, 0x80, time + 3);
    } else {
        /* Just update noise params without trigger */
        gb_apu_wrapper_write(inst->gb_apu, 0xFF22, poly_reg, time);
    }
}

static void gb_silence_channel(chiptune_instance_t *inst, int chan_idx, unsigned time) {
    switch (chan_idx) {
        case 0: /* square 1 */
            gb_apu_wrapper_write(inst->gb_apu, 0xFF12, 0x00, time); /* vol=0 */
            gb_apu_wrapper_write(inst->gb_apu, 0xFF14, 0x80, time + 1); /* retrigger with 0 vol */
            break;
        case 1: /* square 2 */
            gb_apu_wrapper_write(inst->gb_apu, 0xFF17, 0x00, time);
            gb_apu_wrapper_write(inst->gb_apu, 0xFF19, 0x80, time + 1);
            break;
        case 2: /* wave */
            gb_apu_wrapper_write(inst->gb_apu, 0xFF1C, 0x00, time); /* vol=0 (mute) */
            break;
        case 3: /* noise */
            gb_apu_wrapper_write(inst->gb_apu, 0xFF21, 0x00, time);
            gb_apu_wrapper_write(inst->gb_apu, 0xFF23, 0x80, time + 1);
            break;
    }
}

/* =====================================================================
 * Plugin API v2 implementation
 * ===================================================================== */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    /* Must use new (not calloc) because Nes_Apu and Blip_Buffer are C++ objects
     * with constructors that must run. calloc skips constructors → SIGSEGV. */
    chiptune_instance_t *inst = new (std::nothrow) chiptune_instance_t();
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);

    /* Init NES APU */
    init_nes_apu(inst);

    /* Init GB APU */
    inst->gb_apu = NULL;
    init_gb_apu(inst);

    /* Init voices */
    for (int i = 0; i < MAX_VOICES; i++) {
        inst->voices[i].active = 0;
        env_init(&inst->voices[i].env);
    }
    inst->voice_age_counter = 0;
    inst->lfo_phase = 0.0f;
    inst->pitch_bend_semitones = 0.0f;

    /* Load default preset */
    apply_preset(inst, 0);

    plugin_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    chiptune_instance_t *inst = (chiptune_instance_t*)instance;
    if (!inst) return;
    if (inst->gb_apu) {
        gb_apu_wrapper_destroy(inst->gb_apu);
        inst->gb_apu = NULL;
    }
    delete inst;
    plugin_log("Instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    chiptune_instance_t *inst = (chiptune_instance_t*)instance;
    if (!inst || len < 2) return;
    (void)source;

    uint8_t status = msg[0] & 0xF0;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    int octave = (int)inst->params[P_OCTAVE_TRANSPOSE];
    int alloc_mode = (int)inst->params[P_ALLOC_MODE];

    switch (status) {
        case 0x90: { /* Note On */
            if (data2 == 0) {
                /* Velocity 0 = Note Off — release all voices for this note */
                int note = (int)data1 + octave * 12;
                if (note < 0) note = 0;
                if (note > 127) note = 127;
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (inst->voices[i].active && inst->voices[i].note == note) {
                        env_gate_off(&inst->voices[i].env);
                        inst->voices[i].active = 0;
                    }
                }
                break;
            }
            int note = (int)data1 + octave * 12;
            if (note < 0) note = 0;
            if (note > 127) note = 127;

            /* In LEAD mode, kill existing voices first */
            if (alloc_mode == ALLOC_LEAD) {
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (inst->voices[i].active) {
                        inst->voices[i].active = 0;
                        env_init(&inst->voices[i].env);
                    }
                }
            }

            int chan = pick_channel(inst, note);
            int vi = allocate_voice(inst);
            voice_t *v = &inst->voices[vi];

            /* If stealing, mark old voice dead */
            if (v->active) {
                v->active = 0;
            }

            v->active = 1;
            v->note = note;
            v->velocity = data2;
            v->channel_idx = chan;
            v->triggered = 0;  /* Will trigger on first render block */
            v->pitch_env = inst->params[P_PITCH_ENV_DEPTH]; /* Start high, decay to 0 */
            v->age = ++inst->voice_age_counter;

            /* Determine channel type */
            if (chan == 3) {
                v->channel_type = CHAN_NOISE;
            } else if (chan == 2) {
                v->channel_type = (inst->chip == CHIP_NES) ? CHAN_TRIANGLE : CHAN_WAVE;
            } else {
                v->channel_type = chan; /* CHAN_PULSE1 or CHAN_PULSE2 */
            }

            /* Configure envelope */
            int attack = (int)inst->params[P_ENV_ATTACK];
            int decay = (int)inst->params[P_ENV_DECAY];
            int sustain = (int)inst->params[P_ENV_SUSTAIN];
            int release = (int)inst->params[P_ENV_RELEASE];
            env_init(&v->env);
            env_configure(&v->env, attack, decay, sustain, release);
            env_gate_on(&v->env);

            /* Auto-unison: if detune > 0 and both pulse channels available,
             * auto-double the note to the other pulse channel for thick unison */
            float detune_val = inst->params[P_DETUNE];
            int ch_mask = (int)inst->params[P_CHANNEL_MASK];
            if (detune_val > 0.0f && (ch_mask & 0x03) == 0x03 && chan < 2) {
                int chan2 = (chan == 0) ? 1 : 0;
                int vi2 = -1;
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (i != vi && !inst->voices[i].active) {
                        vi2 = i;
                        break;
                    }
                }
                if (vi2 >= 0) {
                    voice_t *v2 = &inst->voices[vi2];
                    v2->active = 1;
                    v2->note = note;
                    v2->velocity = data2;
                    v2->channel_idx = chan2;
                    v2->channel_type = chan2;
                    v2->triggered = 0;
                    v2->pitch_env = inst->params[P_PITCH_ENV_DEPTH];
                    v2->age = ++inst->voice_age_counter;
                    env_init(&v2->env);
                    env_configure(&v2->env, attack, decay, sustain, release);
                    env_gate_on(&v2->env);
                }
            }
            break;
        }

        case 0x80: { /* Note Off */
            int note = (int)data1 + octave * 12;
            if (note < 0) note = 0;
            if (note > 127) note = 127;

            /* Release ALL voices matching this note (handles unison doubles) */
            for (int i = 0; i < MAX_VOICES; i++) {
                if (inst->voices[i].active && inst->voices[i].note == note) {
                    env_gate_off(&inst->voices[i].env);
                    inst->voices[i].active = 0;
                }
            }
            break;
        }

        case 0xB0: { /* CC */
            if (data1 == 1) {
                /* Mod wheel -> vibrato depth */
                inst->params[P_VIBRATO_DEPTH] = (float)(int)(data2 * 12.0f / 127.0f);
            }
            if (data1 == 123 || data1 == 120) {
                /* All notes off / All sound off */
                kill_all_voices(inst);
            }
            break;
        }

        case 0xE0: { /* Pitch bend */
            int bend = ((data2 << 7) | data1) - 8192;
            inst->pitch_bend_semitones = (bend / 8192.0f) * 2.0f; /* +/- 2 semitones */
            break;
        }
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    chiptune_instance_t *inst = (chiptune_instance_t*)instance;
    if (!inst || !key || !val) return;

    /* State restore */
    if (strcmp(key, "state") == 0) {
        float fval;
        /* Restore preset first */
        if (json_get_number(val, "preset", &fval) == 0) {
            int idx = (int)fval;
            if (idx >= 0 && idx < NUM_PRESETS) {
                kill_all_voices(inst);
                apply_preset(inst, idx);
            }
        }
        /* Then override with saved params */
        if (json_get_number(val, "chip", &fval) == 0) {
            inst->chip = (uint8_t)(int)fval;
        }
        for (int i = 0; i < (int)PARAM_DEF_COUNT(g_param_defs); i++) {
            if (json_get_number(val, g_param_defs[i].key, &fval) == 0) {
                if (fval < g_param_defs[i].min_val) fval = g_param_defs[i].min_val;
                if (fval > g_param_defs[i].max_val) fval = g_param_defs[i].max_val;
                inst->params[g_param_defs[i].index] = fval;
            }
        }
        /* Reinit APUs after state restore */
        init_nes_apu(inst);
        init_gb_apu(inst);
        if (inst->chip == CHIP_GB) {
            gb_load_wavetable(inst, (int)inst->params[P_WAVETABLE], 0);
        }
        return;
    }

    /* Preset selection */
    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < NUM_PRESETS && idx != inst->current_preset) {
            kill_all_voices(inst);
            apply_preset(inst, idx);
            /* Reinit APUs on preset change */
            init_nes_apu(inst);
            init_gb_apu(inst);
            if (inst->chip == CHIP_GB) {
                gb_load_wavetable(inst, (int)inst->params[P_WAVETABLE], 0);
            }
        }
        return;
    }

    /* Chip selection */
    if (strcmp(key, "chip") == 0) {
        if (strcmp(val, "NES") == 0 || strcmp(val, "0") == 0) {
            inst->chip = CHIP_NES;
        } else if (strcmp(val, "GB") == 0 || strcmp(val, "1") == 0) {
            inst->chip = CHIP_GB;
            gb_load_wavetable(inst, (int)inst->params[P_WAVETABLE], 0);
        }
        kill_all_voices(inst);
        return;
    }

    /* Alloc mode */
    if (strcmp(key, "alloc_mode") == 0) {
        if (strcmp(val, "Auto") == 0 || strcmp(val, "0") == 0) {
            inst->params[P_ALLOC_MODE] = ALLOC_AUTO;
        } else if (strcmp(val, "Lead") == 0 || strcmp(val, "1") == 0) {
            inst->params[P_ALLOC_MODE] = ALLOC_LEAD;
        } else if (strcmp(val, "Locked") == 0 || strcmp(val, "2") == 0) {
            inst->params[P_ALLOC_MODE] = ALLOC_LOCKED;
        }
        return;
    }

    /* All notes off */
    if (strcmp(key, "all_notes_off") == 0) {
        kill_all_voices(inst);
        return;
    }

    /* Wavetable change: reload wave RAM */
    if (strcmp(key, "wavetable") == 0) {
        int idx = atoi(val);
        if (idx < 0) idx = 0;
        if (idx >= NUM_WAVETABLES) idx = NUM_WAVETABLES - 1;
        inst->params[P_WAVETABLE] = (float)idx;
        if (inst->chip == CHIP_GB) {
            gb_load_wavetable(inst, idx, 0);
        }
        return;
    }

    /* Generic param_helper set */
    if (param_helper_set(g_param_defs, PARAM_DEF_COUNT(g_param_defs),
                         inst->params, key, val) == 0) {
        return;
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    chiptune_instance_t *inst = (chiptune_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Chiptune");
    }
    if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    }
    if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_PRESETS);
    }
    if (strcmp(key, "preset_name") == 0) {
        return snprintf(buf, buf_len, "%s", inst->preset_name);
    }
    if (strcmp(key, "chip") == 0) {
        return snprintf(buf, buf_len, "%s", inst->chip == CHIP_NES ? "NES" : "GB");
    }
    if (strcmp(key, "alloc_mode") == 0) {
        int mode = (int)inst->params[P_ALLOC_MODE];
        const char *names[] = {"Auto", "Lead", "Locked"};
        if (mode < 0) mode = 0;
        if (mode > 2) mode = 2;
        return snprintf(buf, buf_len, "%s", names[mode]);
    }
    if (strcmp(key, "noise_mode") == 0) {
        int mode = (int)inst->params[P_NOISE_MODE];
        return snprintf(buf, buf_len, "%s", mode ? "Short" : "Long");
    }

    /* param_helper params */
    int result = param_helper_get(g_param_defs, PARAM_DEF_COUNT(g_param_defs),
                                  inst->params, key, buf, buf_len);
    if (result >= 0) return result;

    /* UI hierarchy */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy =
            "{\"modes\":null,\"levels\":{"
                "\"root\":{"
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":\"main\","
                    "\"knobs\":[\"env_attack\",\"env_decay\",\"env_sustain\",\"env_release\","
                               "\"duty\",\"vibrato_depth\",\"vibrato_rate\",\"volume\"],"
                    "\"params\":[]"
                "},"
                "\"main\":{"
                    "\"label\":\"Parameters\","
                    "\"children\":null,"
                    "\"knobs\":[\"env_attack\",\"env_decay\",\"env_sustain\",\"env_release\","
                               "\"duty\",\"vibrato_depth\",\"vibrato_rate\",\"volume\"],"
                    "\"params\":["
                        "{\"key\":\"chip\",\"label\":\"Chip\"},"
                        "{\"key\":\"duty\",\"label\":\"Duty Cycle\"},"
                        "{\"key\":\"env_attack\",\"label\":\"Attack\"},"
                        "{\"key\":\"env_decay\",\"label\":\"Decay\"},"
                        "{\"key\":\"env_sustain\",\"label\":\"Sustain\"},"
                        "{\"key\":\"env_release\",\"label\":\"Release\"},"
                        "{\"key\":\"sweep\",\"label\":\"Sweep\"},"
                        "{\"key\":\"vibrato_depth\",\"label\":\"Vibrato Depth\"},"
                        "{\"key\":\"vibrato_rate\",\"label\":\"Vibrato Rate\"},"
                        "{\"key\":\"pitch_env_depth\",\"label\":\"PEnv Depth\"},"
                        "{\"key\":\"pitch_env_speed\",\"label\":\"PEnv Speed\"},"
                        "{\"key\":\"alloc_mode\",\"label\":\"Voice Mode\"},"
                        "{\"key\":\"noise_mode\",\"label\":\"Noise Mode\"},"
                        "{\"key\":\"wavetable\",\"label\":\"Wavetable (GB)\"},"
                        "{\"key\":\"volume\",\"label\":\"Volume\"},"
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"}"
                    "]"
                "}"
            "}}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    /* Chain params metadata */
    if (strcmp(key, "chain_params") == 0) {
        int offset = 0;
        offset += snprintf(buf + offset, buf_len - offset,
            "["
            "{\"key\":\"chip\",\"name\":\"Chip\",\"type\":\"enum\",\"options\":[\"NES\",\"GB\"]},"
            "{\"key\":\"alloc_mode\",\"name\":\"Voice Mode\",\"type\":\"enum\",\"options\":[\"Auto\",\"Lead\",\"Locked\"]},"
            "{\"key\":\"noise_mode\",\"name\":\"Noise Mode\",\"type\":\"enum\",\"options\":[\"Long\",\"Short\"]},"
            "{\"key\":\"duty\",\"name\":\"Duty Cycle\",\"type\":\"int\",\"min\":0,\"max\":3,\"step\":1},"
            "{\"key\":\"env_attack\",\"name\":\"Attack\",\"type\":\"int\",\"min\":0,\"max\":15,\"step\":1},"
            "{\"key\":\"env_decay\",\"name\":\"Decay\",\"type\":\"int\",\"min\":0,\"max\":15,\"step\":1},"
            "{\"key\":\"env_sustain\",\"name\":\"Sustain\",\"type\":\"int\",\"min\":0,\"max\":15,\"step\":1},"
            "{\"key\":\"env_release\",\"name\":\"Release\",\"type\":\"int\",\"min\":0,\"max\":15,\"step\":1},"
            "{\"key\":\"sweep\",\"name\":\"Sweep\",\"type\":\"int\",\"min\":0,\"max\":7,\"step\":1},"
            "{\"key\":\"vibrato_depth\",\"name\":\"Vibrato Depth\",\"type\":\"int\",\"min\":0,\"max\":12,\"step\":1},"
            "{\"key\":\"vibrato_rate\",\"name\":\"Vibrato Rate\",\"type\":\"int\",\"min\":0,\"max\":10,\"step\":1},"
            "{\"key\":\"wavetable\",\"name\":\"Wavetable (GB)\",\"type\":\"int\",\"min\":0,\"max\":7,\"step\":1},"
            "{\"key\":\"channel_mask\",\"name\":\"Channel Mask\",\"type\":\"int\",\"min\":0,\"max\":15,\"step\":1},"
            "{\"key\":\"detune\",\"name\":\"Detune\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
            "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"int\",\"min\":0,\"max\":15,\"step\":1},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3,\"step\":1},"
            "{\"key\":\"pitch_env_depth\",\"name\":\"PEnv Depth\",\"type\":\"int\",\"min\":0,\"max\":24,\"step\":1},"
            "{\"key\":\"pitch_env_speed\",\"name\":\"PEnv Speed\",\"type\":\"int\",\"min\":0,\"max\":15,\"step\":1}"
            "]");
        if (offset >= buf_len) return -1;
        return offset;
    }

    /* State serialization */
    if (strcmp(key, "state") == 0) {
        int offset = 0;
        offset += snprintf(buf + offset, buf_len - offset,
            "{\"preset\":%d,\"chip\":%d", inst->current_preset, inst->chip);
        for (int i = 0; i < (int)PARAM_DEF_COUNT(g_param_defs); i++) {
            float val = inst->params[g_param_defs[i].index];
            offset += snprintf(buf + offset, buf_len - offset,
                ",\"%s\":%d", g_param_defs[i].key, (int)val);
        }
        offset += snprintf(buf + offset, buf_len - offset, "}");
        if (offset >= buf_len) return -1;
        return offset;
    }

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    (void)instance;
    (void)buf;
    (void)buf_len;
    return 0;
}

/* =====================================================================
 * Render block
 * ===================================================================== */

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    chiptune_instance_t *inst = (chiptune_instance_t*)instance;
    if (!inst) {
        memset(out_interleaved_lr, 0, frames * 4);
        return;
    }

    memset(out_interleaved_lr, 0, frames * 4);

    int duty = (int)inst->params[P_DUTY];
    int noise_mode = (int)inst->params[P_NOISE_MODE];
    int sweep = (int)inst->params[P_SWEEP];
    float vib_depth = inst->params[P_VIBRATO_DEPTH];
    float vib_rate = inst->params[P_VIBRATO_RATE];
    int preset_vol = (int)inst->params[P_VOLUME];
    float detune_cents = inst->params[P_DETUNE];
    int wavetable_idx = (int)inst->params[P_WAVETABLE];

    /* Check if any voices are active */
    int any_active = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (inst->voices[i].active) { any_active = 1; break; }
    }

    if (inst->chip == CHIP_NES) {
        /* ---- NES rendering ---- */
        int nes_time = 0;

        /* Re-enable channels each frame */
        inst->nes_apu.write_register(nes_time++, 0x4015, 0x0F);

        for (int vi = 0; vi < MAX_VOICES; vi++) {
            voice_t *v = &inst->voices[vi];
            if (!v->active) continue;

            /* Block-rate envelope: sample level at start of block for APU volume.
             * This gives chiptune-authentic staircase envelope behavior (~2.9ms steps). */
            float avg_level = v->env.level;

            /* Advance envelope state through the block (per-sample values unused) */
            for (int s = 0; s < frames; s++) {
                env_process(&v->env);
            }

            /* If envelope finished, mark voice inactive */
            if (v->env.stage == ENV_IDLE) {
                v->active = 0;
                /* Silence this channel */
                nes_silence_channel(inst, v->channel_idx, nes_time);
                nes_time += 2;
                continue;
            }

            /* Compute vibrato */
            float vib_mult = 1.0f;
            if (vib_depth > 0.0f && vib_rate > 0.0f) {
                float lfo_val = sinf(inst->lfo_phase * 2.0f * 3.14159265f);
                vib_mult = powf(2.0f, lfo_val * vib_depth / 1200.0f);
            }

            /* Base frequency */
            float base_freq = midi_to_freq(v->note);
            /* Apply pitch bend */
            base_freq *= powf(2.0f, inst->pitch_bend_semitones / 12.0f);
            /* Apply pitch envelope (e.g., kick drum pitch drop) */
            if (v->pitch_env > 0.01f) {
                base_freq *= powf(2.0f, v->pitch_env / 12.0f);
                float penv_speed = inst->params[P_PITCH_ENV_SPEED];
                if (penv_speed > 0.0f) {
                    float decay_per_sample = v->pitch_env / (penv_speed * (SAMPLE_RATE / 60.0f));
                    v->pitch_env -= decay_per_sample * frames;
                    if (v->pitch_env < 0.0f) v->pitch_env = 0.0f;
                }
            }
            /* Apply vibrato */
            float freq = base_freq * vib_mult;

            /* Apply detune for second pulse channel in duo mode */
            if (detune_cents > 0.0f && v->channel_idx == 1) {
                freq *= powf(2.0f, detune_cents / 1200.0f);
            }

            /* Compute APU volume from envelope */
            int apu_vol = (int)(avg_level * (float)preset_vol / 15.0f * 15.0f + 0.5f);
            if (apu_vol > 15) apu_vol = 15;
            if (apu_vol < 0) apu_vol = 0;

            /* Scale by velocity */
            apu_vol = (apu_vol * v->velocity) / 127;
            if (apu_vol > 15) apu_vol = 15;

            /* Write to appropriate APU channel */
            int do_trigger = !v->triggered;
            switch (v->channel_type) {
                case CHAN_PULSE1:
                    nes_write_pulse(inst, 0, nes_time, duty, apu_vol, freq, do_trigger);
                    nes_time += 4;
                    break;
                case CHAN_PULSE2:
                    nes_write_pulse(inst, 1, nes_time, duty, apu_vol, freq, do_trigger);
                    nes_time += 4;
                    break;
                case CHAN_TRIANGLE:
                    /* Triangle has no volume control, just gate */
                    nes_write_triangle(inst, nes_time, (apu_vol > 0) ? 1 : 0, freq, do_trigger);
                    nes_time += 3;
                    break;
                case CHAN_NOISE:
                    nes_write_noise(inst, nes_time, apu_vol, v->note, noise_mode, do_trigger);
                    nes_time += 3;
                    break;
            }
            v->triggered = 1;
        }

        /* Silence inactive channels */
        for (int ch = 0; ch < 4; ch++) {
            int in_use = 0;
            for (int vi = 0; vi < MAX_VOICES; vi++) {
                if (inst->voices[vi].active && inst->voices[vi].channel_idx == ch) {
                    in_use = 1;
                    break;
                }
            }
            if (!in_use) {
                nes_silence_channel(inst, ch, nes_time);
                nes_time += 2;
            }
        }

        /* Advance LFO */
        if (vib_rate > 0.0f) {
            inst->lfo_phase += vib_rate * (float)frames / (float)SAMPLE_RATE;
            while (inst->lfo_phase >= 1.0f) inst->lfo_phase -= 1.0f;
        }

        /* Run NES APU for the frame */
        int total_cycles = NES_CYCLES_PER_BLOCK;
        inst->nes_apu.end_frame(total_cycles);
        inst->nes_blip.end_frame(total_cycles);

        /* Read mono samples */
        int avail = inst->nes_blip.samples_avail();
        int to_read = (avail < frames) ? avail : frames;
        if (to_read > 0) {
            memset(inst->nes_mono_buf, 0, sizeof(inst->nes_mono_buf));
            inst->nes_blip.read_samples(inst->nes_mono_buf, to_read);

            /* Convert mono to stereo. NES APU output peaks ~5000; 6x scales to ~30000
             * for good headroom within int16 range. */
            for (int s = 0; s < to_read; s++) {
                int32_t sample = (int32_t)inst->nes_mono_buf[s] * 6;
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                out_interleaved_lr[s * 2] = (int16_t)sample;
                out_interleaved_lr[s * 2 + 1] = (int16_t)sample;
            }
        }

    } else {
        /* ---- GB rendering ---- */
        unsigned gb_time = 0;

        /* Compute combined envelope level for output scaling.
         * GB channels trigger once at full volume; we scale the final output
         * by the software envelope instead of re-triggering every block. */
        float gb_env_level = 0.0f;
        int gb_active_count = 0;

        for (int vi = 0; vi < MAX_VOICES; vi++) {
            voice_t *v = &inst->voices[vi];
            if (!v->active) continue;

            /* Sample envelope level at start of block */
            float avg_level = v->env.level;

            /* Advance envelope through block */
            for (int s = 0; s < frames; s++) {
                env_process(&v->env);
            }

            /* If envelope finished, mark voice inactive */
            if (v->env.stage == ENV_IDLE) {
                v->active = 0;
                gb_silence_channel(inst, v->channel_idx, gb_time);
                gb_time += 4;
                continue;
            }

            /* Track envelope for output scaling */
            gb_env_level += avg_level;
            gb_active_count++;

            /* Compute vibrato */
            float vib_mult = 1.0f;
            if (vib_depth > 0.0f && vib_rate > 0.0f) {
                float lfo_val = sinf(inst->lfo_phase * 2.0f * 3.14159265f);
                vib_mult = powf(2.0f, lfo_val * vib_depth / 1200.0f);
            }

            /* Base frequency */
            float base_freq = midi_to_freq(v->note);
            base_freq *= powf(2.0f, inst->pitch_bend_semitones / 12.0f);
            /* Apply pitch envelope */
            if (v->pitch_env > 0.01f) {
                base_freq *= powf(2.0f, v->pitch_env / 12.0f);
                float penv_speed = inst->params[P_PITCH_ENV_SPEED];
                if (penv_speed > 0.0f) {
                    float decay_per_sample = v->pitch_env / (penv_speed * (SAMPLE_RATE / 60.0f));
                    v->pitch_env -= decay_per_sample * frames;
                    if (v->pitch_env < 0.0f) v->pitch_env = 0.0f;
                }
            }
            float freq = base_freq * vib_mult;

            /* Detune for second square channel */
            if (detune_cents > 0.0f && v->channel_idx == 1) {
                freq *= powf(2.0f, detune_cents / 1200.0f);
            }

            /* Volume: on trigger, set to preset volume. On subsequent blocks,
             * volume stays at whatever the APU has (we scale output instead). */
            int do_trigger = !v->triggered;
            int gb_vol = (int)((float)preset_vol * (float)v->velocity / 127.0f + 0.5f);
            if (gb_vol > 15) gb_vol = 15;
            if (gb_vol < 1) gb_vol = 1;  /* Keep DAC enabled (0 disables DAC) */

            switch (v->channel_idx) {
                case 0:
                    gb_write_square1(inst, gb_time, duty, gb_vol, freq, sweep, do_trigger);
                    gb_time += do_trigger ? 5 : 2;
                    break;
                case 1:
                    gb_write_square2(inst, gb_time, duty, gb_vol, freq, do_trigger);
                    gb_time += do_trigger ? 4 : 2;
                    break;
                case 2: /* wave */
                    gb_write_wave(inst, gb_time, gb_vol, freq, do_trigger);
                    gb_time += 4;
                    break;
                case 3: /* noise */
                    gb_write_noise(inst, gb_time, gb_vol, v->note, noise_mode, do_trigger);
                    gb_time += do_trigger ? 4 : 1;
                    break;
            }
            v->triggered = 1;
        }

        /* Silence inactive channels */
        for (int ch = 0; ch < 4; ch++) {
            int in_use = 0;
            for (int vi = 0; vi < MAX_VOICES; vi++) {
                if (inst->voices[vi].active && inst->voices[vi].channel_idx == ch) {
                    in_use = 1;
                    break;
                }
            }
            if (!in_use) {
                gb_silence_channel(inst, ch, gb_time);
                gb_time += 4;
            }
        }

        /* Advance LFO */
        if (vib_rate > 0.0f) {
            inst->lfo_phase += vib_rate * (float)frames / (float)SAMPLE_RATE;
            while (inst->lfo_phase >= 1.0f) inst->lfo_phase -= 1.0f;
        }

        /* Run GB APU for this block — blargg handles frame sequencer internally */
        long total_cycles = GB_CYCLES_PER_BLOCK;
        gb_apu_wrapper_end_frame(inst->gb_apu, total_cycles);

        /* Compute output envelope scale. For mono voices, this is the
         * voice's envelope level. For poly, average across active voices. */
        float env_scale = 1.0f;
        if (gb_active_count > 0) {
            env_scale = gb_env_level / (float)gb_active_count;
        }

        /* Read stereo samples */
        int avail = gb_apu_wrapper_samples_avail(inst->gb_apu);
        /* avail is count of shorts (stereo pairs * 2) */
        int stereo_shorts = frames * 2;
        if (avail < stereo_shorts) stereo_shorts = avail;
        if (stereo_shorts > 0) {
            memset(inst->gb_stereo_buf, 0, sizeof(inst->gb_stereo_buf));
            int read_count = gb_apu_wrapper_read_samples(inst->gb_apu, inst->gb_stereo_buf, stereo_shorts);

            /* Copy to output with scaling. Boost GB to match NES loudness.
             * Apply software envelope scaling (since we don't re-trigger for volume). */
            int sample_pairs = read_count / 2;
            for (int s = 0; s < sample_pairs && s < frames; s++) {
                int32_t left = (int32_t)((float)inst->gb_stereo_buf[s * 2] * 6.0f * env_scale);
                int32_t right = (int32_t)((float)inst->gb_stereo_buf[s * 2 + 1] * 6.0f * env_scale);
                if (left > 32767) left = 32767;
                if (left < -32768) left = -32768;
                if (right > 32767) right = 32767;
                if (right < -32768) right = -32768;
                out_interleaved_lr[s * 2] = (int16_t)left;
                out_interleaved_lr[s * 2 + 1] = (int16_t)right;
            }
        }
    }
}

/* =====================================================================
 * Plugin API v2 table and entry point
 * ===================================================================== */

static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;

    plugin_log("Plugin API v2 initialized");
    return &g_plugin_api_v2;
}

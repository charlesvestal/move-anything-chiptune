/* C wrapper for blargg's Gb_Apu (from Gb_Snd_Emu)
 * Keeps Blip_Buffer in a separate compilation unit to avoid conflicts
 * with the NES Nes_Snd_Emu Blip_Buffer. */

#ifndef GB_APU_WRAPPER_H
#define GB_APU_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gb_apu_wrapper gb_apu_wrapper_t;

/* Create a new GB APU instance at the given sample rate */
gb_apu_wrapper_t* gb_apu_wrapper_create(int sample_rate);

/* Destroy an instance */
void gb_apu_wrapper_destroy(gb_apu_wrapper_t *w);

/* Reset the APU */
void gb_apu_wrapper_reset(gb_apu_wrapper_t *w);

/* Write to a register (addr: 0xFF10-0xFF3F, time: cycle offset within frame) */
void gb_apu_wrapper_write(gb_apu_wrapper_t *w, unsigned addr, int data, long time);

/* End the current frame (cycles = total GB CPU cycles in this frame) */
void gb_apu_wrapper_end_frame(gb_apu_wrapper_t *w, long cycles);

/* Number of samples available to read (stereo: count of individual shorts) */
int gb_apu_wrapper_samples_avail(gb_apu_wrapper_t *w);

/* Read stereo samples (interleaved L/R int16). Returns number of shorts read. */
int gb_apu_wrapper_read_samples(gb_apu_wrapper_t *w, int16_t *out, int count);

#ifdef __cplusplus
}
#endif

#endif /* GB_APU_WRAPPER_H */

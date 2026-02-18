/* C wrapper for blargg's Gb_Apu
 * Compiled as a separate translation unit to avoid Blip_Buffer conflicts
 * with the NES Nes_Snd_Emu library.
 * All GB sources are compiled with -fvisibility=hidden; only
 * the extern "C" wrapper functions below are exported. */

#include "gb_apu_wrapper.h"
#include "Gb_Apu.h"
#include "Multi_Buffer.h"
#include <new>

#define GB_CPU_CLOCK 4194304
#define GB_EXPORT __attribute__((visibility("default")))

struct gb_apu_wrapper {
    Gb_Apu apu;
    Stereo_Buffer buf;
};

extern "C" {

GB_EXPORT gb_apu_wrapper_t* gb_apu_wrapper_create(int sample_rate) {
    gb_apu_wrapper_t *w = new (std::nothrow) gb_apu_wrapper_t;
    if (!w) return NULL;

    w->buf.clock_rate(GB_CPU_CLOCK);
    if (w->buf.set_sample_rate(sample_rate)) {
        delete w;
        return NULL;
    }

    w->apu.output(w->buf.center(), w->buf.left(), w->buf.right());
    w->apu.reset();

    /* Enable master sound */
    w->apu.write_register(0, 0xFF26, 0x80);
    /* Max master volume */
    w->apu.write_register(0, 0xFF24, 0x77);
    /* All channels to both speakers */
    w->apu.write_register(0, 0xFF25, 0xFF);

    return w;
}

GB_EXPORT void gb_apu_wrapper_destroy(gb_apu_wrapper_t *w) {
    if (w) delete w;
}

GB_EXPORT void gb_apu_wrapper_reset(gb_apu_wrapper_t *w) {
    if (!w) return;
    w->apu.reset();
    w->buf.clear();

    /* Re-enable after reset */
    w->apu.write_register(0, 0xFF26, 0x80);
    w->apu.write_register(0, 0xFF24, 0x77);
    w->apu.write_register(0, 0xFF25, 0xFF);
}

GB_EXPORT void gb_apu_wrapper_write(gb_apu_wrapper_t *w, unsigned addr, int data, long time) {
    if (!w) return;
    w->apu.write_register((gb_time_t)time, (gb_addr_t)addr, data);
}

GB_EXPORT void gb_apu_wrapper_end_frame(gb_apu_wrapper_t *w, long cycles) {
    if (!w) return;
    bool stereo = w->apu.end_frame((gb_time_t)cycles);
    w->buf.end_frame((blip_time_t)cycles, stereo);
}

GB_EXPORT int gb_apu_wrapper_samples_avail(gb_apu_wrapper_t *w) {
    if (!w) return 0;
    return (int)w->buf.samples_avail();
}

GB_EXPORT int gb_apu_wrapper_read_samples(gb_apu_wrapper_t *w, int16_t *out, int count) {
    if (!w) return 0;
    /* blip_sample_t may be short or int depending on platform;
     * cast through the library's type for safety */
    return (int)w->buf.read_samples((blip_sample_t*)out, count);
}

} /* extern "C" */

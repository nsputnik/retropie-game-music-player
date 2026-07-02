/* alsa_out.h - minimal blocking ALSA S16 stereo output for the jukebox engines.
 *
 * The vgmjuke/gmejuke engines route audio through libvgm's AudioStream driver.
 * The libopenmpt/libsidplayfp/FluidSynth engines instead render PCM buffers
 * themselves, so they use this tiny self-contained ALSA writer (libasound only)
 * rather than pulling in libvgm's audio layer. Interleaved 16-bit stereo.
 */
#ifndef JUKEBOX_ALSA_OUT_H
#define JUKEBOX_ALSA_OUT_H

#include <alsa/asoundlib.h>

struct AlsaOut {
    snd_pcm_t* pcm;
    AlsaOut() : pcm(0) {}

    /* Open "default" PCM, S16_LE, interleaved, `channels` at `rate` Hz,
     * ~200ms latency. Returns 0 on success. */
    int open(unsigned rate, unsigned channels) {
        if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            pcm = 0;
            return 1;
        }
        if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED,
                               channels, rate, 1 /*soft resample*/,
                               200000 /*latency us*/) < 0) {
            snd_pcm_close(pcm);
            pcm = 0;
            return 1;
        }
        chans = channels;
        return 0;
    }

    /* Write `frames` interleaved S16 frames, recovering from xruns.
     * Returns 0 on success. */
    int write(const short* buf, snd_pcm_uframes_t frames) {
        while (frames > 0) {
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, frames);
            if (w < 0) {
                w = snd_pcm_recover(pcm, (int)w, 1);
                if (w < 0)
                    return 1;
                continue;
            }
            buf += (size_t)w * chans;
            frames -= (snd_pcm_uframes_t)w;
        }
        return 0;
    }

    void drain() { if (pcm) snd_pcm_drain(pcm); }
    void close() { if (pcm) { snd_pcm_close(pcm); pcm = 0; } }

    unsigned chans;
};

#endif /* JUKEBOX_ALSA_OUT_H */

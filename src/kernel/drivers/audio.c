/* Audio front-end: picks a playback backend once (HDA first, then AC'97)
 * and forwards the drivers/audio.h API to it, so callers never care which
 * controller the machine has. */
#include "drivers/audio.h"
#include "drivers/ac97.h"
#include "drivers/hda.h"
#include "lib/string.h"

extern void klog(const char* s);

enum { AUDIO_BACKEND_NONE = 0, AUDIO_BACKEND_HDA, AUDIO_BACKEND_AC97 };

static int backend;
static char last_err[44] = "no audio controller";

const char* audio_last_error(void) { return last_err; }

int audio_init(void) {
    if (backend != AUDIO_BACKEND_NONE) return 0;   /* already probed */

    if (hda_init() == 0) {
        backend = AUDIO_BACKEND_HDA;
        last_err[0] = '\0';
        klog("[audio] backend: HDA ");
        klog(hda_codec_name());
        klog("\n");
        return 0;
    }
    /* keep the HDA reason: on an HDA-only board this is the whole story */
    strncpy(last_err, hda_last_error(), sizeof(last_err) - 1);
    last_err[sizeof(last_err) - 1] = '\0';

    if (ac97_init() == 0) {
        backend = AUDIO_BACKEND_AC97;
        last_err[0] = '\0';
        klog("[audio] backend: AC97\n");
        return 0;
    }
    klog("[audio] no supported audio controller: ");
    klog(last_err);
    klog("\n");
    return -1;
}

int audio_open(uint32_t sample_rate) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  return hda_open(sample_rate);
        case AUDIO_BACKEND_AC97: return ac97_open(sample_rate);
        default: return -1;
    }
}

void audio_close(void) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  hda_close();  break;
        case AUDIO_BACKEND_AC97: ac97_close(); break;
        default: break;
    }
}

int audio_write(const int16_t* pcm, int frames) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  return hda_write(pcm, frames);
        case AUDIO_BACKEND_AC97: return ac97_write(pcm, frames);
        default: return 0;
    }
}

void audio_play(void) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  hda_play();  break;
        case AUDIO_BACKEND_AC97: ac97_play(); break;
        default: break;
    }
}

void audio_pause(void) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  hda_pause();  break;
        case AUDIO_BACKEND_AC97: ac97_pause(); break;
        default: break;
    }
}

int audio_playing(void) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  return hda_playing();
        case AUDIO_BACKEND_AC97: return ac97_playing();
        default: return 0;
    }
}

int audio_queued(void) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  return hda_queued();
        case AUDIO_BACKEND_AC97: return ac97_queued();
        default: return 0;
    }
}

uint64_t audio_written_total(void) {
    switch (backend) {
        case AUDIO_BACKEND_HDA:  return hda_written_total();
        case AUDIO_BACKEND_AC97: return ac97_written_total();
        default: return 0;
    }
}

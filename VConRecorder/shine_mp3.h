#ifndef SHINE_MP3_H
#define SHINE_MP3_H

// Thin wrapper around the libshine fixed-point MP3 encoder.
// Provides init / encode-frame / flush helpers for the vCon recorder.

#include "config.h"

#if ENABLE_MP3

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "layer3.h"   // libshine public API — C linkage
#ifdef __cplusplus
}
#endif

// Initialise a mono MP3 encoder for the given sample rate and bitrate.
// Returns NULL on failure (invalid params or out of memory).
inline shine_t mp3_encoder_init(int sample_rate, int bitrate_kbps) {
    if (shine_check_config(sample_rate, bitrate_kbps) < 0)
        return nullptr;

    shine_config_t cfg;
    shine_set_config_mpeg_defaults(&cfg.mpeg);
    cfg.mpeg.bitr = bitrate_kbps;
    cfg.mpeg.mode = MONO;
    cfg.wave.channels = PCM_MONO;
    cfg.wave.samplerate = sample_rate;

    return shine_initialise(&cfg);
}

// Encode one frame of PCM samples.  `samples` must point to exactly
// shine_samples_per_pass(enc) int16_t values (mono).
// Returns pointer to encoded MP3 data (owned by libshine — valid until next
// call) and sets *out_bytes to the number of bytes written.
// Returns NULL with *out_bytes == 0 if no data was produced.
inline const uint8_t* mp3_encode_frame(shine_t enc, int16_t* samples,
                                       int* out_bytes) {
    // shine_encode_buffer expects int16_t** (array of channel pointers)
    int16_t* ch[1] = { samples };
    unsigned char* data = shine_encode_buffer(enc, ch, out_bytes);
    return (const uint8_t*)data;
}

// Flush the encoder's internal buffer.  Call once after the last frame.
// Returns pointer to remaining MP3 data, sets *out_bytes.
inline const uint8_t* mp3_encode_flush(shine_t enc, int* out_bytes) {
    unsigned char* data = shine_flush(enc, out_bytes);
    return (const uint8_t*)data;
}

#endif // ENABLE_MP3
#endif // SHINE_MP3_H

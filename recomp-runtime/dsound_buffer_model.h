#ifndef DOA2U_RECOMP_DSOUND_BUFFER_MODEL_H
#define DOA2U_RECOMP_DSOUND_BUFFER_MODEL_H

#include <stdint.h>

enum {
    RECOMP_DSOUND_OK = 0x00000000u,
    RECOMP_DSOUND_POINTER_ERROR = 0x80004003u,
    RECOMP_DSOUND_OUT_OF_MEMORY = 0x8007000eu,
    RECOMP_DSOUND_INVALID_PARAM = 0x80070057u,
    RECOMP_DSOUND_PLAY_LOOPING = 1u,
    RECOMP_DSOUND_PLAY_FROMSTART = 2u,
};

typedef struct RecompDsoundBufferModel {
    uint32_t size_bytes;
    uint32_t sample_rate;
    uint32_t original_sample_rate;
    uint32_t block_align;
    uint32_t cursor_bytes;
    uint32_t loop_start_bytes;
    uint32_t loop_end_bytes;
    uint32_t frame_remainder;
    uint32_t play_flags;
    uint32_t playing;
    uint64_t last_ms;
} RecompDsoundBufferModel;

uint32_t recomp_dsound_buffer_configure(
    RecompDsoundBufferModel *model, uint32_t size_bytes,
    uint32_t sample_rate, uint32_t block_align, uint64_t now_ms);
uint32_t recomp_dsound_buffer_cursor(
    RecompDsoundBufferModel *model, uint64_t now_ms);
/* Consume elapsed PCM on a separate output clock; wrap at loop_end_bytes to
   loop_start_bytes when looping. A span can cover multiple loop iterations. */
/* Spans recomp_dsound_buffer_consume discarded after a stall longer than
   RECOMP_DSOUND_STALL_MS; diagnostics only. */
extern uint32_t recomp_dsound_stall_drops;

uint32_t recomp_dsound_buffer_consume(
    RecompDsoundBufferModel *model, uint64_t now_ms, uint32_t *offset_bytes);
uint32_t recomp_dsound_buffer_play(
    RecompDsoundBufferModel *model, uint32_t flags, uint64_t now_ms);
void recomp_dsound_buffer_stop(
    RecompDsoundBufferModel *model, uint64_t now_ms);
uint32_t recomp_dsound_buffer_set_position(
    RecompDsoundBufferModel *model, uint32_t position_bytes, uint64_t now_ms);
uint32_t recomp_dsound_buffer_set_frequency(
    RecompDsoundBufferModel *model, uint32_t sample_rate, uint64_t now_ms);

#endif

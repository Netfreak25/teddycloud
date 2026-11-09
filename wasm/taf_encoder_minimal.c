/**
 * Bounded browser encoder. A synchronous seekable sink writes to OPFS; encoded
 * output never accumulates in WASM. Native Opus uses one fresh context per file.
 */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef WASM_BUILD
#include <emscripten.h>
EM_JS(int, taf_encoder_sink_write, (uint32_t offset, const uint8_t *data, size_t length), {
    try {
        const write = Module["tafWrite"];
        return typeof write === "function" ? write(offset, data, length) : -1;
    } catch (_) { return -1; }
});
#else
#define EMSCRIPTEN_KEEPALIVE
extern int taf_encoder_sink_write(uint32_t offset, const uint8_t *data, size_t length);
#endif
#include "encoder_limits.h"
#include "ogg/ogg.h"
#include "opus.h"
#include "proto/toniebox.pb.taf-header.pb-c.h"
#include "sha1_standalone.h"

#define ENCODER_SAMPLE_RATE 48000
#define ENCODER_CHANNELS 2
#define ENCODER_FRAME_SAMPLES 2880
#define ENCODER_SHORTEST_FRAME 120
#define ENCODER_DEFAULT_KBPS 96
#define ENCODER_PACKET_MARGIN 64
#define ENCODER_OGG_HEADER_BYTES 27
#define ENCODER_COMMENT_BYTES 436
#define ENCODER_INITIAL_CHAPTERS 16U

enum {
    ENCODER_OK, ENCODER_MEMORY_ERROR, ENCODER_SINK_ERROR,
    ENCODER_HEADER_ERROR, ENCODER_CODEC_ERROR, ENCODER_STATE_ERROR,
    ENCODER_SIZE_ERROR
};

typedef struct taf_encoder_t {
    OpusEncoder *enc;
    ogg_stream_state os;
    bool ogg_initialized, native_opus, finalized;
    int error;
    TonieboxAudioFileHeader taf;
    size_t chapter_capacity;
    Sha1Context sha1;
    int16_t pcm[ENCODER_FRAME_SAMPLES * ENCODER_CHANNELS];
    int pcm_used;
    uint8_t pending_packet[ENCODER_TAF_HEADER_SIZE];
    int pending_length, pending_capacity;
    uint64_t pending_granule, input_samples, encoded_samples, chapter_padding;
    uint64_t chapter_input_start, packet_count;
    uint32_t output_size;
    int lookahead;
} taf_encoder_t;

void taf_encoder_free(taf_encoder_t *ctx);

static int encoder_fail(taf_encoder_t *ctx, int error)
{
    if (ctx != NULL && ctx->error == ENCODER_OK)
        ctx->error = error;
    return -1;
}

static bool encoder_ready(const taf_encoder_t *ctx)
{
    return ctx != NULL && ctx->error == ENCODER_OK && !ctx->finalized;
}

static int encoder_write_at(taf_encoder_t *ctx, uint32_t offset, const void *data, size_t length)
{
    uint32_t maximum = ctx->native_opus ? ENCODER_MAX_FILE_BYTES : ENCODER_MAX_TAF_UPLOAD_BYTES;
    if (length > maximum || offset > maximum - length)
        return encoder_fail(ctx, ENCODER_SIZE_ERROR);
    if (taf_encoder_sink_write(offset, data, length) != (int)length)
        return encoder_fail(ctx, ENCODER_SINK_ERROR);
    return 0;
}

static int encoder_append(taf_encoder_t *ctx, const void *data, size_t length)
{
    if (encoder_write_at(ctx, ctx->output_size, data, length) != 0)
        return -1;
    if (!ctx->native_opus)
        sha1Update(&ctx->sha1, data, length);
    ctx->output_size += (uint32_t)length;
    return 0;
}

static int encoder_pages(taf_encoder_t *ctx, bool flush)
{
    ogg_page page;
    while (flush ? ogg_stream_flush(&ctx->os, &page) : ogg_stream_pageout(&ctx->os, &page)) {
        if (encoder_append(ctx, page.header, (size_t)page.header_len) != 0 ||
            encoder_append(ctx, page.body, (size_t)page.body_len) != 0)
            return -1;
    }
    return 0;
}

/** Reserve the worst-case final bookkeeping sizes, not their current values. */
static bool encoder_header_fits(const taf_encoder_t *ctx)
{
    uint8_t digest[SHA1_DIGEST_SIZE] = {0};
    TonieboxAudioFileHeader header = ctx->taf;
    header.sha1_hash.data = digest;
    header.sha1_hash.len = sizeof(digest);
    header.num_bytes = ENCODER_MAX_TAF_UPLOAD_BYTES - ENCODER_TAF_HEADER_SIZE;
    header.has_ogg_granule_position = header.has_ogg_packet_count = true;
    header.has_taf_block_num = header.has_pageno = true;
    header.ogg_granule_position = header.ogg_packet_count = UINT64_MAX;
    header.taf_block_num = header.pageno = UINT64_MAX;
    header._fill.len = 0;
    header._fill.data = NULL;
    return toniebox_audio_file_header__get_packed_size(&header) <= ENCODER_TAF_PROTO_BYTES;
}

static int encoder_add_chapter(taf_encoder_t *ctx)
{
    if (ctx->taf.n_track_page_nums >= ENCODER_TAF_MAX_CHAPTERS)
        return encoder_fail(ctx, ENCODER_HEADER_ERROR);
    if (ctx->taf.n_track_page_nums == ctx->chapter_capacity) {
        size_t capacity = ctx->chapter_capacity ? ctx->chapter_capacity * 2 : ENCODER_INITIAL_CHAPTERS;
        if (capacity > ENCODER_TAF_MAX_CHAPTERS)
            capacity = ENCODER_TAF_MAX_CHAPTERS;
        uint32_t *chapters = realloc(ctx->taf.track_page_nums, capacity * sizeof(*chapters));
        if (chapters == NULL)
            return encoder_fail(ctx, ENCODER_MEMORY_ERROR);
        ctx->taf.track_page_nums = chapters;
        ctx->chapter_capacity = capacity;
    }
    ctx->taf.track_page_nums[ctx->taf.n_track_page_nums++] =
        (ctx->output_size - ENCODER_TAF_HEADER_SIZE) / ENCODER_TAF_HEADER_SIZE;
    if (!encoder_header_fits(ctx))
        return encoder_fail(ctx, ENCODER_HEADER_ERROR);
    return 0;
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++)
        dst[i] = (uint8_t)(value >> (i * 8));
}

static int encoder_headers(taf_encoder_t *ctx)
{
    uint8_t head[] = {'O','p','u','s','H','e','a','d',1,ENCODER_CHANNELS,
                      0,0,0x80,0xbb,0,0,0,0,0};
    head[10] = (uint8_t)ctx->lookahead;
    head[11] = (uint8_t)(ctx->lookahead >> 8);
    uint8_t tags[ENCODER_COMMENT_BYTES] = {0};
    const char vendor[] = "teddyCloud WASM";
    const char comment[] = "encoder=teddyCloud WASM TAF Encoder";
    memcpy(tags, "OpusTags", 8);
    size_t pos = 8;
    put_u32_le(tags + pos, sizeof(vendor) - 1);
    pos += 4;
    memcpy(tags + pos, vendor, sizeof(vendor) - 1);
    pos += sizeof(vendor) - 1;
    put_u32_le(tags + pos, 1);
    pos += 4;
    put_u32_le(tags + pos, sizeof(comment) - 1);
    pos += 4;
    memcpy(tags + pos, comment, sizeof(comment) - 1);
    ogg_packet packet = {0};
    packet.packet = head;
    packet.bytes = sizeof(head);
    packet.b_o_s = 1;
    packet.packetno = (ogg_int64_t)ctx->packet_count++;
    if (ogg_stream_packetin(&ctx->os, &packet) != 0 || encoder_pages(ctx, true) != 0)
        return encoder_fail(ctx, ENCODER_CODEC_ERROR);
    packet.packet = tags;
    packet.bytes = sizeof(tags);
    packet.b_o_s = 0;
    packet.packetno = (ogg_int64_t)ctx->packet_count++;
    if (ogg_stream_packetin(&ctx->os, &packet) != 0 || encoder_pages(ctx, true) != 0)
        return encoder_fail(ctx, ENCODER_CODEC_ERROR);
    return 0;
}

static taf_encoder_t *encoder_create(uint32_t audio_id, int bitrate_kbps, bool native)
{
    taf_encoder_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return NULL;
    ctx->native_opus = native;
    toniebox_audio_file_header__init(&ctx->taf);
    ctx->taf.audio_id = audio_id;
    sha1Init(&ctx->sha1);
    int error = OPUS_OK;
    ctx->enc = opus_encoder_create(ENCODER_SAMPLE_RATE, ENCODER_CHANNELS, OPUS_APPLICATION_AUDIO, &error);
    if (ctx->enc == NULL || error != OPUS_OK)
        goto failed;
    if (bitrate_kbps <= 0)
        bitrate_kbps = ENCODER_DEFAULT_KBPS;
    if (bitrate_kbps > INT_MAX / 1000 ||
        opus_encoder_ctl(ctx->enc, OPUS_SET_BITRATE(bitrate_kbps * 1000)) != OPUS_OK ||
        opus_encoder_ctl(ctx->enc, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(ctx->enc, OPUS_SET_COMPLEXITY(10)) != OPUS_OK ||
        opus_encoder_ctl(ctx->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)) != OPUS_OK ||
        opus_encoder_ctl(ctx->enc, OPUS_SET_LSB_DEPTH(16)) != OPUS_OK ||
        opus_encoder_ctl(ctx->enc, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_ARG)) != OPUS_OK ||
        opus_encoder_ctl(ctx->enc, OPUS_GET_LOOKAHEAD(&ctx->lookahead)) != OPUS_OK ||
        ctx->lookahead < 0 || ctx->lookahead > UINT16_MAX)
        goto failed;
    if (ogg_stream_init(&ctx->os, (int)audio_id) != 0)
        goto failed;
    ctx->ogg_initialized = true;
    if (!native) {
        uint8_t header[ENCODER_TAF_HEADER_SIZE] = {0};
        if (encoder_write_at(ctx, 0, header, sizeof(header)) != 0)
            goto failed;
        ctx->output_size = sizeof(header);
        if (encoder_add_chapter(ctx) != 0)
            goto failed;
    }
    if (encoder_headers(ctx) != 0)
        goto failed;
    return ctx;
failed:
    taf_encoder_free(ctx);
    return NULL;
}

EMSCRIPTEN_KEEPALIVE
taf_encoder_t *taf_encoder_create(uint32_t audio_id, int bitrate_kbps)
{
    return encoder_create(audio_id, bitrate_kbps, false);
}

/** One complete independently decodable Ogg/Opus chapter. */
EMSCRIPTEN_KEEPALIVE
taf_encoder_t *opus_encoder_create_file(uint32_t audio_id, int bitrate_kbps)
{
    return encoder_create(audio_id, bitrate_kbps, true);
}

/* One pending packet allows chapter padding/EOS to be decided before writing. */
static int encoder_submit_packet(taf_encoder_t *ctx, bool chapter_end, bool eos, uint64_t granule)
{
    if (ctx->pending_length == 0)
        return 0;
    int length = ctx->pending_length;
    if (!ctx->native_opus && !eos &&
        (chapter_end || ctx->pending_capacity - length < ENCODER_PACKET_MARGIN)) {
        if (opus_packet_pad(ctx->pending_packet, length, ctx->pending_capacity) != OPUS_OK)
            return encoder_fail(ctx, ENCODER_CODEC_ERROR);
        length = ctx->pending_capacity;
    }
    ogg_packet packet = {0};
    packet.packet = ctx->pending_packet;
    packet.bytes = length;
    packet.e_o_s = eos;
    packet.granulepos = (ogg_int64_t)granule;
    packet.packetno = (ogg_int64_t)ctx->packet_count++;
    if (ogg_stream_packetin(&ctx->os, &packet) != 0)
        return encoder_fail(ctx, ENCODER_CODEC_ERROR);
    ctx->pending_length = 0;
    return encoder_pages(ctx, chapter_end || eos);
}

static int encoder_packet_capacity(taf_encoder_t *ctx, bool eos)
{
    if (ctx->native_opus)
        return sizeof(ctx->pending_packet);
    int page_used = (ctx->output_size % ENCODER_TAF_HEADER_SIZE) + ENCODER_OGG_HEADER_BYTES +
                    ctx->os.lacing_fill - ctx->os.lacing_returned +
                    ctx->os.body_fill - ctx->os.body_returned;
    int remaining = ENCODER_TAF_HEADER_SIZE - page_used;
    int capacity = (remaining / 256) * 255 + remaining % 256 - 1;
    int reconstructed = capacity / 255 + 1 + capacity;
    if (remaining != reconstructed && capacity > ENCODER_PACKET_MARGIN)
        capacity -= ENCODER_PACKET_MARGIN;
    /* A complete TAF must not look like an unfinished block-aligned stream. */
    if (eos)
        capacity -= ENCODER_PACKET_MARGIN;
    return capacity;
}

static int encoder_frame(taf_encoder_t *ctx, int frames, bool eos, uint64_t final_granule)
{
    if (encoder_submit_packet(ctx, false, false, ctx->pending_granule) != 0)
        return -1;
    int capacity = encoder_packet_capacity(ctx, eos);
    if (capacity < ENCODER_PACKET_MARGIN - 1) {
        if (encoder_pages(ctx, true) != 0)
            return -1;
        capacity = encoder_packet_capacity(ctx, eos);
    }
    if (capacity < ENCODER_PACKET_MARGIN - 1 || capacity > (int)sizeof(ctx->pending_packet))
        return encoder_fail(ctx, ENCODER_CODEC_ERROR);
    int length = opus_encode(ctx->enc, ctx->pcm, frames, ctx->pending_packet, capacity);
    if (length <= 0)
        return encoder_fail(ctx, ENCODER_CODEC_ERROR);
    ctx->encoded_samples += (uint64_t)frames;
    ctx->pending_granule = eos ? final_granule : ctx->encoded_samples;
    ctx->pending_length = length;
    ctx->pending_capacity = capacity;
    ctx->pcm_used = 0;
    return eos ? encoder_submit_packet(ctx, false, true, final_granule) : 0;
}

EMSCRIPTEN_KEEPALIVE
int taf_encoder_encode(taf_encoder_t *ctx, const int16_t *samples, int num_samples)
{
    if (!encoder_ready(ctx) || samples == NULL || num_samples < 0)
        return encoder_fail(ctx, ENCODER_STATE_ERROR);
    int offset = 0;
    while (offset < num_samples) {
        int count = ENCODER_FRAME_SAMPLES - ctx->pcm_used;
        if (count > num_samples - offset)
            count = num_samples - offset;
        memcpy(ctx->pcm + ctx->pcm_used * ENCODER_CHANNELS,
               samples + offset * ENCODER_CHANNELS,
               (size_t)count * ENCODER_CHANNELS * sizeof(*samples));
        ctx->pcm_used += count;
        ctx->input_samples += (uint64_t)count;
        offset += count;
        if (ctx->pcm_used == ENCODER_FRAME_SAMPLES &&
            encoder_frame(ctx, ENCODER_FRAME_SAMPLES, false, 0) != 0)
            return -1;
    }
    return 0;
}

static int encoder_tail_frames(int samples)
{
    const int sizes[] = {120, 240, 480, 960, 1920, 2880};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (samples <= sizes[i])
            return sizes[i];
    return ENCODER_FRAME_SAMPLES;
}

EMSCRIPTEN_KEEPALIVE
int taf_encoder_new_chapter(taf_encoder_t *ctx)
{
    if (!encoder_ready(ctx) || ctx->native_opus || ctx->input_samples == ctx->chapter_input_start)
        return encoder_fail(ctx, ENCODER_STATE_ERROR);
    if (ctx->pcm_used > 0) {
        int frames = encoder_tail_frames(ctx->pcm_used);
        ctx->chapter_padding += (uint64_t)(frames - ctx->pcm_used);
        memset(ctx->pcm + ctx->pcm_used * ENCODER_CHANNELS, 0,
               (size_t)(frames - ctx->pcm_used) * ENCODER_CHANNELS * sizeof(int16_t));
        if (encoder_frame(ctx, frames, false, 0) != 0)
            return -1;
    }
    if (encoder_submit_packet(ctx, true, false, ctx->pending_granule) != 0)
        return -1;
    /* A lacing-size discontinuity can leave a small slot. A 2.5 ms silent frame
     * fills it; original PCM is never discarded. Chapter padding stays <60ms. */
    if (ctx->output_size % ENCODER_TAF_HEADER_SIZE != 0) {
        memset(ctx->pcm, 0, ENCODER_SHORTEST_FRAME * ENCODER_CHANNELS * sizeof(int16_t));
        ctx->chapter_padding += ENCODER_SHORTEST_FRAME;
        if (encoder_frame(ctx, ENCODER_SHORTEST_FRAME, false, 0) != 0 ||
            encoder_submit_packet(ctx, true, false, ctx->pending_granule) != 0)
            return -1;
    }
    if (ctx->output_size % ENCODER_TAF_HEADER_SIZE != 0)
        return encoder_fail(ctx, ENCODER_CODEC_ERROR);
    ctx->chapter_input_start = ctx->input_samples;
    return encoder_add_chapter(ctx);
}

static int encoder_taf_header(taf_encoder_t *ctx, uint64_t final_granule)
{
    uint8_t digest[SHA1_DIGEST_SIZE];
    uint8_t padding[ENCODER_TAF_PROTO_BYTES] = {0};
    uint8_t output[ENCODER_TAF_HEADER_SIZE] = {0};
    sha1Final(&ctx->sha1, digest);
    TonieboxAudioFileHeader header = ctx->taf;
    header.sha1_hash.data = digest;
    header.sha1_hash.len = sizeof(digest);
    header.num_bytes = ctx->output_size - ENCODER_TAF_HEADER_SIZE;
    header.has_ogg_granule_position = header.has_ogg_packet_count = true;
    header.has_taf_block_num = header.has_pageno = true;
    header.ogg_granule_position = final_granule;
    header.ogg_packet_count = ctx->packet_count;
    header.taf_block_num = header.num_bytes / ENCODER_TAF_HEADER_SIZE;
    header.pageno = ctx->os.pageno;
    header._fill.data = padding;
    header._fill.len = 0;
    size_t base = toniebox_audio_file_header__get_packed_size(&header);
    if (base > ENCODER_TAF_PROTO_BYTES)
        return encoder_fail(ctx, ENCODER_HEADER_ERROR);
    header._fill.len = ENCODER_TAF_PROTO_BYTES - base;
    size_t packed = toniebox_audio_file_header__get_packed_size(&header);
    while (packed > ENCODER_TAF_PROTO_BYTES && header._fill.len > 0) {
        header._fill.len--;
        packed = toniebox_audio_file_header__get_packed_size(&header);
    }
    if (packed > ENCODER_TAF_PROTO_BYTES)
        return encoder_fail(ctx, ENCODER_HEADER_ERROR);
    output[0] = (uint8_t)(packed >> 24);
    output[1] = (uint8_t)(packed >> 16);
    output[2] = (uint8_t)(packed >> 8);
    output[3] = (uint8_t)packed;
    if (toniebox_audio_file_header__pack(&header, output + 4) != packed)
        return encoder_fail(ctx, ENCODER_HEADER_ERROR);
    return encoder_write_at(ctx, 0, output, sizeof(output));
}

EMSCRIPTEN_KEEPALIVE
int taf_encoder_finalize(taf_encoder_t *ctx)
{
    if (!encoder_ready(ctx) || ctx->input_samples == ctx->chapter_input_start)
        return encoder_fail(ctx, ENCODER_STATE_ERROR);
    uint64_t final_granule = ctx->input_samples + ctx->chapter_padding + (uint64_t)ctx->lookahead;
    while (ctx->encoded_samples < final_granule) {
        uint64_t remaining = final_granule - ctx->encoded_samples;
        int frames = encoder_tail_frames(remaining > ENCODER_FRAME_SAMPLES
                                             ? ENCODER_FRAME_SAMPLES : (int)remaining);
        bool eos = ctx->encoded_samples + (uint64_t)frames >= final_granule;
        if (ctx->pcm_used > frames)
            return encoder_fail(ctx, ENCODER_STATE_ERROR);
        memset(ctx->pcm + ctx->pcm_used * ENCODER_CHANNELS, 0,
               (size_t)(frames - ctx->pcm_used) * ENCODER_CHANNELS * sizeof(int16_t));
        if (encoder_frame(ctx, frames, eos, final_granule) != 0)
            return -1;
    }
    if (!ctx->native_opus && encoder_taf_header(ctx, final_granule) != 0)
        return -1;
    ctx->finalized = true;
    return 0;
}

/** ABI compatibility only: this implementation deliberately has no RAM output. */
EMSCRIPTEN_KEEPALIVE
uint8_t *taf_encoder_get_buffer(taf_encoder_t *ctx)
{
    (void)ctx;
    return NULL;
}

EMSCRIPTEN_KEEPALIVE
uint32_t taf_encoder_get_size(taf_encoder_t *ctx)
{
    return ctx ? ctx->output_size : 0;
}

EMSCRIPTEN_KEEPALIVE
int taf_encoder_get_error(taf_encoder_t *ctx)
{
    return ctx ? ctx->error : ENCODER_STATE_ERROR;
}

EMSCRIPTEN_KEEPALIVE
void taf_encoder_free(taf_encoder_t *ctx)
{
    if (ctx == NULL)
        return;
    if (ctx->enc != NULL)
        opus_encoder_destroy(ctx->enc);
    if (ctx->ogg_initialized)
        ogg_stream_clear(&ctx->os);
    free(ctx->taf.track_page_nums);
    free(ctx);
}

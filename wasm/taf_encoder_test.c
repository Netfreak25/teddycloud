/** Focused streaming encoder checks, using real Opus/Ogg and a seekable sink. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "encoder_limits.h"
#include "ogg/ogg.h"
#include "opus.h"
#include "proto/toniebox.pb.taf-header.pb-c.h"
#include "sha1_standalone.h"

typedef struct taf_encoder_t taf_encoder_t;
taf_encoder_t *taf_encoder_create(uint32_t audio_id, int bitrate);
taf_encoder_t *opus_encoder_create_file(uint32_t audio_id, int bitrate);
int taf_encoder_encode(taf_encoder_t *, const int16_t *, int);
int taf_encoder_new_chapter(taf_encoder_t *);
int taf_encoder_finalize(taf_encoder_t *);
uint32_t taf_encoder_get_size(taf_encoder_t *);
int taf_encoder_get_error(taf_encoder_t *);
void taf_encoder_free(taf_encoder_t *);

static FILE *sink;
static int fail_write;
static size_t writes;
int taf_encoder_sink_write(uint32_t offset, const uint8_t *data, size_t length)
{
    writes++;
    if (fail_write || fseek(sink, offset, SEEK_SET) != 0)
        return -1;
    return (int)fwrite(data, 1, length, sink);
}

static void begin_sink(void)
{
    sink = tmpfile();
    assert(sink != NULL);
    fail_write = 0;
    writes = 0;
}

static uint8_t *read_sink(size_t size)
{
    uint8_t *bytes = malloc(size);
    assert(bytes && fflush(sink) == 0 && fseek(sink, 0, SEEK_SET) == 0);
    assert(fread(bytes, 1, size, sink) == size);
    return bytes;
}

static void pcm_tone(int16_t *pcm, int frames)
{
    for (int i = 0; i < frames; i++)
        pcm[2*i] = pcm[2*i+1] = (int16_t)(12000.0 * sin((double)i * 0.13));
}

static void native_case(int frames)
{
    begin_sink();
    taf_encoder_t *ctx = opus_encoder_create_file(123, 96);
    assert(ctx);
    int16_t *pcm = malloc((size_t)frames * 2 * sizeof(*pcm));
    assert(pcm);
    pcm_tone(pcm, frames);
    /* Exercise boundaries that are unrelated to Opus frame boundaries. */
    for (int at = 0; at < frames;) {
        int count = frames - at > 137 ? 137 : frames - at;
        assert(taf_encoder_encode(ctx, pcm + at * 2, count) == 0);
        at += count;
    }
    assert(taf_encoder_finalize(ctx) == 0);
    size_t size = taf_encoder_get_size(ctx);
    uint8_t *bytes = read_sink(size);
    ogg_sync_state sync;
    ogg_stream_state stream;
    ogg_sync_init(&sync);
    memcpy(ogg_sync_buffer(&sync, (long)size), bytes, size);
    assert(ogg_sync_wrote(&sync, (long)size) == 0);
    ogg_page page;
    int page_count = 0, packet_count = 0, preskip = 0, eos = 0;
    ogg_int64_t last_granule = -1;
    int error;
    OpusDecoder *decoder = opus_decoder_create(48000, 2, &error);
    assert(decoder && error == OPUS_OK);
    int16_t decoded[5760 * 2];
    uint64_t decoded_frames = 0;
    while (ogg_sync_pageout(&sync, &page) == 1) {
        if (page_count++ == 0)
            assert(ogg_stream_init(&stream, ogg_page_serialno(&page)) == 0);
        assert(ogg_stream_pagein(&stream, &page) == 0);
        if (ogg_page_eos(&page)) {
            eos++;
            last_granule = ogg_page_granulepos(&page);
        }
        ogg_packet packet;
        while (ogg_stream_packetout(&stream, &packet) == 1) {
            if (packet_count == 0) {
                assert(packet.bytes == 19 && memcmp(packet.packet, "OpusHead", 8) == 0);
                preskip = packet.packet[10] | packet.packet[11] << 8;
            } else if (packet_count == 1) {
                assert(memcmp(packet.packet, "OpusTags", 8) == 0);
            } else {
                int count = opus_decode(decoder, packet.packet, (opus_int32)packet.bytes,
                                        decoded, 5760, 0);
                assert(count > 0);
                decoded_frames += (uint64_t)count;
            }
            packet_count++;
        }
    }
    assert(eos == 1 && packet_count >= 3);
    assert(last_granule == frames + preskip);
    assert(decoded_frames >= (uint64_t)last_granule);
    ogg_stream_clear(&stream);
    ogg_sync_clear(&sync);
    opus_decoder_destroy(decoder);
    free(bytes);
    free(pcm);
    taf_encoder_free(ctx);
    fclose(sink);
}

static void taf_chapters_case(void)
{
    enum { CHAPTERS = 123, MAX_FRAMES = 3001 };
    int16_t pcm[MAX_FRAMES * 2];
    pcm_tone(pcm, MAX_FRAMES);
    begin_sink();
    taf_encoder_t *ctx = taf_encoder_create(456, 96);
    assert(ctx);
    uint64_t input = 0;
    for (int i = 0; i < CHAPTERS; i++) {
        if (i)
            assert(taf_encoder_new_chapter(ctx) == 0);
        int frames = i < 3 ? MAX_FRAMES : 1 + (i * 137) % MAX_FRAMES;
        int chunks = i < 3 ? 20 : 1;
        for (int chunk = 0; chunk < chunks; chunk++) {
            assert(taf_encoder_encode(ctx, pcm, frames) == 0);
            input += (uint64_t)frames;
        }
    }
    assert(taf_encoder_finalize(ctx) == 0);
    size_t size = taf_encoder_get_size(ctx);
    assert(size > ENCODER_TAF_HEADER_SIZE && size % ENCODER_TAF_HEADER_SIZE != 0);
    uint8_t *bytes = read_sink(size);
    uint32_t header_size = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
                           (uint32_t)bytes[2] << 8 | bytes[3];
    assert(header_size <= ENCODER_TAF_PROTO_BYTES);
    TonieboxAudioFileHeader *header =
        toniebox_audio_file_header__unpack(NULL, header_size, bytes + 4);
    assert(header && header->n_track_page_nums == CHAPTERS);
    assert(header->num_bytes == size - ENCODER_TAF_HEADER_SIZE);
    uint8_t digest[SHA1_DIGEST_SIZE];
    Sha1Context sha;
    sha1Init(&sha);
    sha1Update(&sha, bytes + ENCODER_TAF_HEADER_SIZE, size - ENCODER_TAF_HEADER_SIZE);
    sha1Final(&sha, digest);
    assert(header->sha1_hash.len == sizeof(digest) &&
           memcmp(header->sha1_hash.data, digest, sizeof(digest)) == 0);
    for (size_t i = 0; i < CHAPTERS; i++) {
        size_t offset = ENCODER_TAF_HEADER_SIZE +
                        (size_t)header->track_page_nums[i] * ENCODER_TAF_HEADER_SIZE;
        assert(offset + 4 <= size && memcmp(bytes + offset, "OggS", 4) == 0);
        if (i)
            assert(header->track_page_nums[i] > header->track_page_nums[i-1]);
    }
    assert(header->ogg_granule_position >= input);
    assert(header->ogg_granule_position - input < CHAPTERS * 2880U);
    toniebox_audio_file_header__free_unpacked(header, NULL);
    free(bytes);
    taf_encoder_free(ctx);
    fclose(sink);
}

static void failure_cases(void)
{
    int16_t pcm[240] = {0};
    begin_sink();
    taf_encoder_t *ctx = taf_encoder_create(789, 96);
    assert(ctx && taf_encoder_encode(ctx, pcm, 120) == 0);
    fail_write = 1;
    assert(taf_encoder_finalize(ctx) != 0 && taf_encoder_get_error(ctx) == 2);
    size_t count = writes;
    assert(taf_encoder_encode(ctx, pcm, 120) != 0);
    assert(taf_encoder_finalize(ctx) != 0 && writes == count);
    taf_encoder_free(ctx);
    fclose(sink);

    begin_sink();
    ctx = taf_encoder_create(790, 96);
    assert(ctx);
    size_t chapters = 0;
    while (chapters < ENCODER_TAF_MAX_CHAPTERS) {
        assert(taf_encoder_encode(ctx, pcm, 120) == 0);
        chapters++;
        if (taf_encoder_new_chapter(ctx) != 0)
            break;
    }
    assert(chapters > 100 && chapters < ENCODER_TAF_MAX_CHAPTERS);
    assert(taf_encoder_get_error(ctx) == 3 && taf_encoder_finalize(ctx) != 0);
    taf_encoder_free(ctx);
    fclose(sink);
}

int main(void)
{
    const int lengths[] = {1,119,120,121,959,960,961,2879,2880,2881,47999};
    for (size_t i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++)
        native_case(lengths[i]);
    taf_chapters_case();
    failure_cases();
    puts("PASS: native Opus tail/pre-skip/EOS; 123 aligned TAF chapters/hash; sink/header failure propagation");
    return 0;
}

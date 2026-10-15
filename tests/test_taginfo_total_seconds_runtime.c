/* The Python contract runner appends the unmodified production helper. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "contentJson.h"
#include "toniefile.h"
#include "cJSON.h"

static void api_add_taf_total_seconds(cJSON *json_entry,
                                      const tonie_info_t *taf_info);

static const uint32_t test_payload_size = 8192;
static const uint32_t test_total_seconds = 183;
static uint32_t file_size;
static error_t stat_error;
static unsigned stat_calls;
static const char *expected_path;
static unsigned cases;

error_t fsGetFileSize(const char_t *path, uint32_t *size)
{
    assert(path != NULL && expected_path != NULL);
    assert(strcmp(path, expected_path) == 0);
    stat_calls++;
    *size = file_size;
    return stat_error;
}

static tonie_info_t complete_taf(TonieboxAudioFileHeader *header,
                                uint32_t *positions)
{
    memset(header, 0, sizeof(*header));
    header->has_ogg_granule_position = TRUE;
    header->ogg_granule_position = (uint64_t)test_total_seconds * OPUS_SAMPLING_RATE;
    header->num_bytes = test_payload_size;
    header->n_track_page_nums = 3;
    positions[0] = 0;
    positions[1] = 60;
    positions[2] = 120;

    tonie_info_t info = {0};
    info.exists = TRUE;
    info.valid = TRUE;
    info.contentPath = "/fixture/selected-cache.taf";
    info.tafHeader = header;
    info.json._source_type = CT_SOURCE_NONE;
    info.additional.track_positions.count = header->n_track_page_nums;
    info.additional.track_positions.pos = positions;
    info.additional.track_positions.total_seconds = test_total_seconds;
    file_size = TONIEFILE_FRAME_SIZE + test_payload_size;
    stat_error = NO_ERROR;
    return info;
}

/* All existing fields, including starts and per-chapter durations, survive
 * unchanged. In rejected cases the entire response remains identical. */
static void check_total(const tonie_info_t *info, bool_t present,
                        unsigned expected_stat_calls)
{
    cJSON *entry = cJSON_Parse(
        "{\"ruid\":\"fixture\",\"valid\":true,\"exists\":true,"
        "\"source\":\"\",\"audioUrl\":\"/content/download/fixture\","
        "\"cacheState\":{\"tafComplete\":true,\"v3Complete\":false},"
        "\"trackSeconds\":[0,60,120],"
        "\"playlist\":{\"kind\":\"direct_taf\",\"chapterCount\":3,"
        "\"tracks\":[\"A\",\"B\",\"C\"],\"durations\":[60,60,63]}}");
    assert(entry != NULL);
    cJSON *before = cJSON_Duplicate(entry, TRUE);
    assert(before != NULL);
    expected_path = info != NULL ? info->contentPath : NULL;
    stat_calls = 0;
    api_add_taf_total_seconds(entry, info);
    assert(stat_calls == expected_stat_calls);
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(entry, "totalSeconds");
    if (present)
    {
        assert(cJSON_IsNumber(total));
        assert(total->valuedouble == info->additional.track_positions.total_seconds);
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "totalSeconds");
    }
    else
    {
        assert(total == NULL);
    }
    assert(cJSON_Compare(entry, before, TRUE));
    cJSON_Delete(before);
    cJSON_Delete(entry);
    cases++;
}

int main(void)
{
    TonieboxAudioFileHeader header;
    uint32_t positions[3];
    tonie_info_t info = complete_taf(&header, positions);
    check_total(&info, TRUE, 1); /* Complete original cache, no source. */
    info.json.source = "";
    check_total(&info, TRUE, 1);
    info.json._source_type = CT_SOURCE_TAF;
    info.json.source = "lib://assigned.taf";
    info.contentPath = "/fixture/resolved-assigned.taf";
    check_total(&info, TRUE, 1); /* Stat the resolved file, not source URI. */
    header.n_track_page_nums = 1;
    info.additional.track_positions.count = 1;
    check_total(&info, TRUE, 1); /* One start at zero still has a total. */
    info = complete_taf(&header, positions);
    positions[1] = positions[0];
    check_total(&info, TRUE, 1); /* Rounded starts may be equal. */

    const ct_source_t excluded[] = {
        CT_SOURCE_TAF_INCOMPLETE, CT_SOURCE_TAP_STREAM, CT_SOURCE_TAP_CACHED,
        CT_SOURCE_STREAM, CT_SOURCE_NATIVE_COLLECTION,
        CT_SOURCE_TONIEPLAY_COLLECTION, (ct_source_t)255,
    };
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++)
    {
        info = complete_taf(&header, positions);
        info.json._source_type = excluded[i];
        check_total(&info, FALSE, 0);
    }
    info = complete_taf(&header, positions);
    info.json.source = "lib://unresolved-or-invalid-source";
    check_total(&info, FALSE, 0);

    check_total(NULL, FALSE, 0);
    info = complete_taf(&header, positions);
    info.valid = FALSE;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    info.exists = FALSE;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    info.tafHeader = NULL;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    info.contentPath = NULL;
    check_total(&info, FALSE, 0);

    info = complete_taf(&header, positions);
    header.has_ogg_granule_position = FALSE;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    info.additional.track_positions.total_seconds = 0;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    header.num_bytes = 0;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    info.additional.track_positions.pos = NULL;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    info.additional.track_positions.count = 0;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    header.n_track_page_nums = 0;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    header.n_track_page_nums++;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    positions[2] = test_total_seconds + 1;
    check_total(&info, FALSE, 0);
    info = complete_taf(&header, positions);
    positions[1] = positions[2] + 1;
    check_total(&info, FALSE, 0);

    info = complete_taf(&header, positions);
    stat_error = ERROR_FILE_NOT_FOUND;
    check_total(&info, FALSE, 1);
    info = complete_taf(&header, positions);
    file_size--;
    check_total(&info, FALSE, 1); /* Valid header, incomplete payload. */
    info = complete_taf(&header, positions);
    file_size = TONIEFILE_FRAME_SIZE - 1;
    check_total(&info, FALSE, 1); /* No unsigned subtraction underflow. */
    info = complete_taf(&header, positions);
    file_size = TONIEFILE_FRAME_SIZE;
    check_total(&info, FALSE, 1);
    info = complete_taf(&header, positions);
    header.num_bytes = UINT64_MAX;
    check_total(&info, FALSE, 1); /* No header-size addition overflow. */
    info = complete_taf(&header, positions);
    file_size++; /* Trailing padding is permitted. */
    check_total(&info, TRUE, 1);

    printf("PASS tagInfo totalSeconds: %u cases; existing JSON unchanged\n", cases);
    return 0;
}

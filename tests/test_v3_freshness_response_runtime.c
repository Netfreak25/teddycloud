/* The runner inserts the unchanged production functions. Transport and the
 * independently exercised notification ledger are the only mocked boundaries. */
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handler.h"
#include "handler_cloud.h"
#include "mqtt_server.h"
#include "cJSON.h"
#undef TRACE_WARNING
#define TRACE_WARNING(...) ((void)0)

struct mqtt_freshness_snapshot { unsigned generation; };
static unsigned generation, delivered_generation;
static unsigned prepare_count, finish_count, headers, bodies, flushes;
static unsigned fail_phase, change_phase;
static bool_t claim;
static char written_body[4096];

void *osAllocMem(size_t size) { return malloc(size); }
void osFreeMem(void *pointer) { free(pointer); }
error_t mqtt_server_freshness_prepare(mqtt_freshness_snapshot_t *snapshot,
                                     const uint64_t *uids, size_t count)
{
    assert(snapshot != NULL);
    assert(count == 0 || uids != NULL);
    prepare_count++;
    claim = TRUE;
    return NO_ERROR;
}
void mqtt_server_freshness_finish(mqtt_freshness_snapshot_t *snapshot, bool_t sent)
{
    assert(snapshot != NULL);
    finish_count++;
    assert(claim);
    if (sent && snapshot->generation == generation)
        delivered_generation = generation;
    claim = FALSE;
    free(snapshot);
}
void httpPrepareHeader(HttpConnection *connection, const void *type, size_t length)
{
    assert(claim);
    assert(strcmp(type, "application/json; charset=utf-8") == 0);
    connection->response.contentLength = length;
}
static error_t transport(unsigned phase)
{
    assert(claim);
    if (change_phase == phase)
        generation++;
    return phase == fail_phase ? ERROR_WRITE_FAILED : NO_ERROR;
}
error_t httpWriteHeader(HttpConnection *connection)
{
    (void)connection;
    headers++;
    return transport(1);
}
error_t httpWriteStream(HttpConnection *connection, const void *data, size_t length)
{
    assert(length == connection->response.contentLength);
    assert(length < sizeof(written_body));
    memcpy(written_body, data, length);
    written_body[length] = '\0';
    bodies++;
    return transport(2);
}
error_t httpFlushStream(HttpConnection *connection)
{
    (void)connection;
    flushes++;
    return transport(3);
}

/* RESPONSE_FUNCTIONS */

static HttpConnection connection;
static TonieFreshnessCheckResponse response;
static uint64_t uids[4];
static uint64_t cloud_uids[2];
static cbr_ctx_t context;
static HttpClientContext cloud;

static void reset(void)
{
    assert(!claim);
    free(context.buffer);
    memset(&context, 0, sizeof(context));
    memset(&cloud, 0, sizeof(cloud));
    memset(&response, 0, sizeof(response));
    generation = 1;
    delivered_generation = 0;
    prepare_count = finish_count = headers = bodies = flushes = 0;
    fail_phase = change_phase = 0;
    written_body[0] = '\0';
    uids[0] = UINT64_C(0xE00403501CF29481);
    cloud_uids[0] = UINT64_C(0xE00403501CF29482);
    response.tonie_marked = uids;
    response.n_tonie_marked = 1;
    context.connection = &connection;
    context.customData = &response;
    context.customDataLen = 4;
    context.freshnessCloudUids = cloud_uids;
    context.freshnessCloudUidCount = 1;
    context.freshnessSnapshot = malloc(sizeof(*context.freshnessSnapshot));
    context.freshnessSnapshot->generation = generation;
    cloud.statusCode = 200;
}

static void test_body_and_eof_complete_once(void)
{
    reset();
    const char *body = "{\"items\":[\"8294F21C500304E0\",\"8294F21C500304E0\","
                       "\"8394F21C500304E0\",\"8294F21C500304E0extra\"]}";
    cloud.bodyLen = strlen(body);
    receiveFreshnessCloudResponseV3(&context, &cloud, body, 7, NO_ERROR);
    assert(headers == 0);
    receiveFreshnessCloudResponseV3(&context, &cloud, body + 7, strlen(body) - 7, NO_ERROR);
    assert(context.status == PROX_STATUS_DONE);
    assert(headers == 1 && bodies == 1 && flushes == 1);
    assert(prepare_count == 1 && finish_count == 1 && delivered_generation == 1);
    assert(response.n_tonie_marked == 2);
    assert(strcmp(written_body, "{\"items\":[\"8194F21C500304E0\",\"8294F21C500304E0\"]}") == 0);
    receiveFreshnessCloudResponseV3(&context, &cloud, NULL, 0, ERROR_END_OF_STREAM);
    assert(finishFreshnessResponseV3(&context) == NO_ERROR);
    assert(headers == 1 && finish_count == 1 && flushes == 1);
}

static void test_write_errors_are_not_acknowledged_or_retried(void)
{
    for (unsigned phase = 1; phase <= 3; phase++)
    {
        reset();
        fail_phase = phase;
        assert(finishFreshnessResponseV3(&context) == ERROR_WRITE_FAILED);
        assert(context.status == PROX_STATUS_DONE && !claim);
        assert(delivered_generation == 0 && finish_count == 1);
        assert(headers == 1 && bodies == (phase >= 2) && flushes == (phase >= 3));
        fail_phase = 0;
        assert(finishFreshnessResponseV3(&context) == ERROR_WRITE_FAILED);
        assert(headers == 1 && finish_count == 1);
    }
}

static void test_cloud_failure_preserves_local_answer(void)
{
    reset();
    /* Connect failure: no callback, so the request owner completes locally. */
    assert(finishFreshnessResponseV3(&context) == NO_ERROR);
    assert(response.n_tonie_marked == 1 && delivered_generation == 1);
    reset();
    cloud.bodyLen = 100;
    const char *partial = "{\"items\":[";
    receiveFreshnessCloudResponseV3(&context, &cloud, partial, strlen(partial), NO_ERROR);
    receiveFreshnessCloudResponseV3(&context, &cloud, NULL, 0, ERROR_TIMEOUT);
    assert(response.n_tonie_marked == 1 && finish_count == 1 && delivered_generation == 1);
    reset();
    cloud.chunkedEncoding = TRUE;
    const char *invalid = "not json";
    receiveFreshnessCloudResponseV3(&context, &cloud, invalid, strlen(invalid), NO_ERROR);
    assert(headers == 0);
    receiveFreshnessCloudResponseV3(&context, &cloud, NULL, 0, ERROR_END_OF_STREAM);
    assert(response.n_tonie_marked == 1 && delivered_generation == 1);
}

static void test_unknown_length_and_invalid_trailing_data(void)
{
    const char *body = "{\"items\":[\"8294F21C500304E0\"]}";
    reset();
    cloud.bodyLen = UINT_MAX;
    receiveFreshnessCloudResponseV3(&context, &cloud, body, strlen(body), NO_ERROR);
    assert(headers == 0);
    receiveFreshnessCloudResponseV3(&context, &cloud, NULL, 0, ERROR_END_OF_STREAM);
    assert(response.n_tonie_marked == 2 && delivered_generation == 1);
    reset();
    const char *invalid = "{\"items\":[\"8294F21C500304E0\"]}garbage";
    cloud.bodyLen = strlen(invalid);
    receiveFreshnessCloudResponseV3(&context, &cloud, invalid, strlen(invalid), NO_ERROR);
    assert(response.n_tonie_marked == 1 && delivered_generation == 1);
}

static void test_completion_uses_captured_generation(void)
{
    for (unsigned phase = 1; phase <= 3; phase++)
    {
        reset();
        change_phase = phase;
        assert(finishFreshnessResponseV3(&context) == NO_ERROR);
        assert(generation == 2 && delivered_generation == 0 && finish_count == 1);
    }
}

int main(void)
{
    test_body_and_eof_complete_once();
    test_write_errors_are_not_acknowledged_or_retried();
    test_cloud_failure_preserves_local_answer();
    test_unknown_length_and_invalid_trailing_data();
    test_completion_uses_captured_generation();
    free(context.buffer);
    puts("V3 freshness response runtime: PASS (completion, transport failures, fallback, captured generation)");
    return 0;
}

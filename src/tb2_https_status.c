#include "tb2_https_status.h"

#include <time.h>

#include "cJSON.h"
#include "handler.h"
#include "os_port.h"
#include "settings.h"

typedef struct
{
    char state[24];
    char error_code[64];
    char last_endpoint[256];
    uint32_t active_requests;
    uint32_t last_http_status;
    time_t last_attempt;
    time_t last_success;
} tb2_v3_status_t;

typedef struct
{
    char state[24];
    char error_code[64];
    uint32_t active_sessions;
    uint64_t bytes_box_to_upstream;
    uint64_t bytes_upstream_to_box;
    time_t last_attempt;
    time_t last_success;
} tb2_tunnel_status_t;

typedef struct
{
    bool_t initialized;
    OsMutex mutex;
    tb2_v3_status_t v3;
    tb2_tunnel_status_t tunnel;
} tb2_https_status_t;

typedef struct
{
    uint32_t v3;
    uint32_t transparent;
    uint32_t disabled;
} tb2_mode_counts_t;

static tb2_https_status_t tb2_status;

static void tb2_status_copy(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0)
    {
        return;
    }
    osStrncpy(destination, source != NULL ? source : "", destination_size - 1);
    destination[destination_size - 1] = '\0';
}

static void tb2_count_mode(const settings_t *settings, tb2_mode_counts_t *counts)
{
    if (settings->cloud.tb2_enabled)
    {
        counts->transparent++;
    }
    else if (settings->cloud.tb2_v3_enabled)
    {
        counts->v3++;
    }
    else
    {
        counts->disabled++;
    }
}

static tb2_mode_counts_t tb2_get_mode_counts(void)
{
    tb2_mode_counts_t counts = {0};
    bool_t found_tb2_overlay = FALSE;

    for (uint8_t id = 1; id < MAX_OVERLAYS; id++)
    {
        settings_t *settings = get_settings_id(id);
        if (!settings->internal.config_used ||
            settings->toniebox.boxGeneration != GENERATION_TB2)
        {
            continue;
        }
        found_tb2_overlay = TRUE;
        tb2_count_mode(settings, &counts);
    }

    if (!found_tb2_overlay)
    {
        tb2_count_mode(get_settings(), &counts);
    }
    return counts;
}

static const char *tb2_get_mode(const tb2_mode_counts_t *counts)
{
    if (counts->v3 > 0 && counts->transparent > 0)
    {
        return "mixed";
    }
    if (counts->transparent > 0)
    {
        return "transparent";
    }
    if (counts->v3 > 0)
    {
        return "v3";
    }
    return "disabled";
}

static const char *tb2_get_aggregate_state(const tb2_mode_counts_t *counts)
{
    if (counts->v3 == 0 && counts->transparent == 0)
    {
        return "disabled";
    }
    if ((counts->transparent > 0 && osStrcmp(tb2_status.tunnel.state, "error") == 0) ||
        (counts->v3 > 0 && osStrcmp(tb2_status.v3.state, "error") == 0))
    {
        return "error";
    }
    if (counts->transparent > 0 && tb2_status.tunnel.active_sessions > 0)
    {
        return osStrcmp(tb2_status.tunnel.state, "connecting") == 0 ? "connecting" : "tunneling";
    }
    if (counts->v3 > 0 && tb2_status.v3.active_requests > 0)
    {
        return "request_active";
    }
    if ((counts->transparent > 0 && osStrcmp(tb2_status.tunnel.state, "success") == 0) ||
        (counts->v3 > 0 && osStrcmp(tb2_status.v3.state, "success") == 0))
    {
        return "success";
    }
    return "ready";
}

error_t tb2_https_status_init(void)
{
    osMemset(&tb2_status, 0, sizeof(tb2_status));
    if (!osCreateMutex(&tb2_status.mutex))
    {
        return ERROR_OUT_OF_RESOURCES;
    }
    tb2_status.initialized = TRUE;
    tb2_status_copy(tb2_status.v3.state, sizeof(tb2_status.v3.state), "ready");
    tb2_status_copy(tb2_status.tunnel.state, sizeof(tb2_status.tunnel.state), "ready");
    return NO_ERROR;
}

void tb2_https_status_deinit(void)
{
    if (tb2_status.initialized)
    {
        osDeleteMutex(&tb2_status.mutex);
        tb2_status.initialized = FALSE;
    }
}

void tb2_https_status_v3_start(const char *endpoint)
{
    if (!tb2_status.initialized)
    {
        return;
    }
    osAcquireMutex(&tb2_status.mutex);
    tb2_status.v3.active_requests++;
    tb2_status.v3.last_attempt = time(NULL);
    tb2_status.v3.last_http_status = 0;
    tb2_status.v3.error_code[0] = '\0';
    tb2_status_copy(tb2_status.v3.last_endpoint, sizeof(tb2_status.v3.last_endpoint), endpoint);
    tb2_status_copy(tb2_status.v3.state, sizeof(tb2_status.v3.state), "request_active");
    osReleaseMutex(&tb2_status.mutex);
}

void tb2_https_status_v3_finish(error_t error, uint32_t http_status)
{
    if (!tb2_status.initialized)
    {
        return;
    }
    osAcquireMutex(&tb2_status.mutex);
    if (tb2_status.v3.active_requests > 0)
    {
        tb2_status.v3.active_requests--;
    }
    tb2_status.v3.last_http_status = http_status;
    if (error == NO_ERROR)
    {
        tb2_status.v3.last_success = time(NULL);
        tb2_status.v3.error_code[0] = '\0';
        tb2_status_copy(tb2_status.v3.state, sizeof(tb2_status.v3.state),
                        tb2_status.v3.active_requests > 0 ? "request_active" : "success");
    }
    else
    {
        tb2_status_copy(tb2_status.v3.error_code, sizeof(tb2_status.v3.error_code),
                        error2text(error));
        tb2_status_copy(tb2_status.v3.state, sizeof(tb2_status.v3.state),
                        tb2_status.v3.active_requests > 0 ? "request_active" : "error");
    }
    osReleaseMutex(&tb2_status.mutex);
}

void tb2_https_status_tunnel_start(void)
{
    if (!tb2_status.initialized)
    {
        return;
    }
    osAcquireMutex(&tb2_status.mutex);
    tb2_status.tunnel.active_sessions++;
    tb2_status.tunnel.last_attempt = time(NULL);
    tb2_status.tunnel.error_code[0] = '\0';
    tb2_status_copy(tb2_status.tunnel.state, sizeof(tb2_status.tunnel.state), "connecting");
    osReleaseMutex(&tb2_status.mutex);
}

void tb2_https_status_tunnel_set_state(const char *state)
{
    if (!tb2_status.initialized)
    {
        return;
    }
    osAcquireMutex(&tb2_status.mutex);
    tb2_status_copy(tb2_status.tunnel.state, sizeof(tb2_status.tunnel.state), state);
    osReleaseMutex(&tb2_status.mutex);
}

void tb2_https_status_tunnel_add_bytes(bool_t box_to_upstream, size_t length)
{
    if (!tb2_status.initialized)
    {
        return;
    }
    osAcquireMutex(&tb2_status.mutex);
    if (box_to_upstream)
    {
        tb2_status.tunnel.bytes_box_to_upstream += length;
    }
    else
    {
        tb2_status.tunnel.bytes_upstream_to_box += length;
    }
    osReleaseMutex(&tb2_status.mutex);
}

void tb2_https_status_tunnel_finish(bool_t success, const char *error_code)
{
    if (!tb2_status.initialized)
    {
        return;
    }
    osAcquireMutex(&tb2_status.mutex);
    if (tb2_status.tunnel.active_sessions > 0)
    {
        tb2_status.tunnel.active_sessions--;
    }
    if (success)
    {
        tb2_status.tunnel.last_success = time(NULL);
        tb2_status.tunnel.error_code[0] = '\0';
        tb2_status_copy(tb2_status.tunnel.state, sizeof(tb2_status.tunnel.state),
                        tb2_status.tunnel.active_sessions > 0 ? "tunneling" : "success");
    }
    else
    {
        tb2_status_copy(tb2_status.tunnel.error_code, sizeof(tb2_status.tunnel.error_code),
                        error_code);
        tb2_status_copy(tb2_status.tunnel.state, sizeof(tb2_status.tunnel.state),
                        tb2_status.tunnel.active_sessions > 0 ? "tunneling" : "error");
    }
    osReleaseMutex(&tb2_status.mutex);
}

error_t tb2_https_status_write(HttpConnection *connection)
{
    settings_t *settings = get_settings();
    tb2_mode_counts_t counts = tb2_get_mode_counts();
    cJSON *json = cJSON_CreateObject();
    cJSON *mode_counts = cJSON_CreateObject();
    cJSON *v3 = cJSON_CreateObject();
    cJSON *transparent = cJSON_CreateObject();
    if (json == NULL || mode_counts == NULL || v3 == NULL || transparent == NULL)
    {
        cJSON_Delete(json);
        cJSON_Delete(mode_counts);
        cJSON_Delete(v3);
        cJSON_Delete(transparent);
        return ERROR_OUT_OF_MEMORY;
    }

    osAcquireMutex(&tb2_status.mutex);
    const char *aggregate_state = tb2_get_aggregate_state(&counts);
    const char *aggregate_error = "";
    if (counts.transparent > 0 && tb2_status.tunnel.error_code[0] != '\0')
    {
        aggregate_error = tb2_status.tunnel.error_code;
    }
    else if (counts.v3 > 0)
    {
        aggregate_error = tb2_status.v3.error_code;
    }

    cJSON_AddBoolToObject(json, "enabled", counts.v3 > 0 || counts.transparent > 0);
    cJSON_AddBoolToObject(json, "passthrough_enabled", counts.transparent > 0);
    cJSON_AddStringToObject(json, "mode", tb2_get_mode(&counts));
    cJSON_AddStringToObject(json, "state", aggregate_state);
    cJSON_AddStringToObject(json, "hostname", settings->cloud.remote_hostname_tb2);
    cJSON_AddNumberToObject(json, "port", settings->cloud.remote_port_tb2);
    cJSON_AddNumberToObject(json, "bytes_box_to_upstream",
                           (double)tb2_status.tunnel.bytes_box_to_upstream);
    cJSON_AddNumberToObject(json, "bytes_upstream_to_box",
                           (double)tb2_status.tunnel.bytes_upstream_to_box);
    cJSON_AddNumberToObject(json, "last_attempt",
                           (double)(tb2_status.v3.last_attempt > tb2_status.tunnel.last_attempt
                                        ? tb2_status.v3.last_attempt
                                        : tb2_status.tunnel.last_attempt));
    cJSON_AddNumberToObject(json, "last_success",
                           (double)(tb2_status.v3.last_success > tb2_status.tunnel.last_success
                                        ? tb2_status.v3.last_success
                                        : tb2_status.tunnel.last_success));
    cJSON_AddStringToObject(json, "error_code", aggregate_error);

    cJSON_AddNumberToObject(mode_counts, "v3", counts.v3);
    cJSON_AddNumberToObject(mode_counts, "transparent", counts.transparent);
    cJSON_AddNumberToObject(mode_counts, "disabled", counts.disabled);
    cJSON_AddItemToObject(json, "mode_counts", mode_counts);

    cJSON_AddStringToObject(v3, "state", tb2_status.v3.state);
    cJSON_AddNumberToObject(v3, "active_requests", tb2_status.v3.active_requests);
    cJSON_AddNumberToObject(v3, "last_attempt", (double)tb2_status.v3.last_attempt);
    cJSON_AddNumberToObject(v3, "last_success", (double)tb2_status.v3.last_success);
    cJSON_AddNumberToObject(v3, "last_http_status", tb2_status.v3.last_http_status);
    cJSON_AddStringToObject(v3, "last_endpoint", tb2_status.v3.last_endpoint);
    cJSON_AddStringToObject(v3, "error_code", tb2_status.v3.error_code);
    cJSON_AddItemToObject(json, "v3", v3);

    cJSON_AddStringToObject(transparent, "state", tb2_status.tunnel.state);
    cJSON_AddNumberToObject(transparent, "active_sessions", tb2_status.tunnel.active_sessions);
    cJSON_AddNumberToObject(transparent, "bytes_box_to_upstream",
                           (double)tb2_status.tunnel.bytes_box_to_upstream);
    cJSON_AddNumberToObject(transparent, "bytes_upstream_to_box",
                           (double)tb2_status.tunnel.bytes_upstream_to_box);
    cJSON_AddNumberToObject(transparent, "last_attempt", (double)tb2_status.tunnel.last_attempt);
    cJSON_AddNumberToObject(transparent, "last_success", (double)tb2_status.tunnel.last_success);
    cJSON_AddStringToObject(transparent, "error_code", tb2_status.tunnel.error_code);
    cJSON_AddItemToObject(json, "transparent", transparent);
    osReleaseMutex(&tb2_status.mutex);

    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL)
    {
        return ERROR_OUT_OF_MEMORY;
    }

    httpPrepareHeader(connection, "application/json; charset=utf-8", osStrlen(body));
    return httpWriteResponse(connection, body, connection->response.contentLength, true);
}

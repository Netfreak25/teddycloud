#include <stdint.h>
#include <string.h>

#include "core/net.h"
#include "core/socket.h"
#include "core/tcp.h"
#include "core/tcp_misc.h"
#include "debug.h"
#include "http/http_client.h"
#include "http/http_client_transport.h"
#include "os_port.h"
#include "platform.h"
#include "rand.h"
#include "tb2_mqtt_upstream.h"
#include "tls.h"
#include "tls_adapter.h"

#define UPSTREAM_READ_BYTES 16384U
#define UPSTREAM_POLL_MS 20U
#define UPSTREAM_IDLE_WAIT_MS 1000U
#define UPSTREAM_RETRY_MAX_MS 30000U
#define UPSTREAM_INITIAL_RETRY_MS 1000U
#define UPSTREAM_CONFIG_MAX_BYTES (64U * 1024U)
/* At most 32 TX completions, 32 RX chunks, CONNECTED, DISCONNECT and DOWN.
 * The worker drains no further attempt until these events have been polled. */
#define UPSTREAM_EVENT_CAPACITY (2U * TB2_MQTT_UPSTREAM_QUEUE_MESSAGES + 3U)

typedef struct
{
    uint8_t *data;
    size_t length;
    uint64_t token;
    uint64_t epoch;
    uint64_t owner_serial;
} upstream_tx_t;

struct tb2_mqtt_upstream_worker
{
    bool_t initialized;
    bool_t in_use;
    bool_t enabled;
    bool_t busy;
    bool_t connected;
    bool_t retry_now;
    bool_t shutdown_requested;
    bool_t session_confirmed;
    OsMutex mutex;
    OsEvent wake;
    tb2_mqtt_upstream_config_t *config;
    uint64_t owner_serial;
    uint64_t revision;
    uint64_t epoch;
    error_t abort_reason;
    upstream_tx_t tx[TB2_MQTT_UPSTREAM_QUEUE_MESSAGES];
    size_t tx_head;
    size_t tx_count;
    size_t tx_outstanding;
    size_t tx_bytes;
    tb2_mqtt_upstream_event_t events[UPSTREAM_EVENT_CAPACITY];
    size_t event_head;
    size_t event_count;
    size_t rx_count;
    size_t rx_bytes;
};

static tb2_mqtt_upstream_worker_t workers[TB2_MQTT_UPSTREAM_SLOTS];

static void upstream_config_free(tb2_mqtt_upstream_config_t *config)
{
    if (config == NULL)
        return;
    osFreeMem((void *)config->hostname);
    osFreeMem((void *)config->ca);
    osFreeMem((void *)config->certificate);
    if (config->private_key != NULL)
    {
        /* This is only our owned snapshot, never a live settings string. */
        volatile char *key = (volatile char *)config->private_key;
        size_t length = osStrlen(config->private_key);
        while (length > 0)
            key[--length] = 0;
        osFreeMem((void *)config->private_key);
    }
    osFreeMem(config);
}

static char *upstream_string_copy(const char *value)
{
    size_t length = osStrlen(value) + 1;
    char *copy = osAllocMem(length);
    if (copy != NULL)
        osMemcpy(copy, value, length);
    return copy;
}

static error_t upstream_config_copy(const tb2_mqtt_upstream_config_t *source,
                                    tb2_mqtt_upstream_config_t **output)
{
    *output = NULL;
    if (source == NULL || source->hostname == NULL || source->ca == NULL ||
        source->certificate == NULL || source->private_key == NULL ||
        source->hostname[0] == 0 || source->ca[0] == 0 ||
        source->certificate[0] == 0 || source->private_key[0] == 0 ||
        source->port == 0 || source->timeout_ms == 0)
        return ERROR_INVALID_PARAMETER;
    const char *strings[] = {source->hostname, source->ca,
                            source->certificate, source->private_key};
    size_t total = 0;
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++)
    {
        size_t length = osStrlen(strings[i]);
        if (length >= UPSTREAM_CONFIG_MAX_BYTES - total)
            return ERROR_INVALID_LENGTH;
        total += length + 1;
    }
    tb2_mqtt_upstream_config_t *copy = osAllocMem(sizeof(*copy));
    if (copy == NULL)
        return ERROR_OUT_OF_MEMORY;
    osMemset(copy, 0, sizeof(*copy));
    copy->hostname = upstream_string_copy(source->hostname);
    copy->ca = upstream_string_copy(source->ca);
    copy->certificate = upstream_string_copy(source->certificate);
    copy->private_key = upstream_string_copy(source->private_key);
    copy->port = source->port;
    copy->timeout_ms = source->timeout_ms;
    if (copy->hostname == NULL || copy->ca == NULL ||
        copy->certificate == NULL || copy->private_key == NULL)
    {
        upstream_config_free(copy);
        return ERROR_OUT_OF_MEMORY;
    }
    *output = copy;
    return NO_ERROR;
}

static bool_t upstream_config_equal(const tb2_mqtt_upstream_config_t *left,
                                    const tb2_mqtt_upstream_config_t *right)
{
    return left != NULL && right != NULL && left->port == right->port &&
           left->timeout_ms == right->timeout_ms &&
           osStrcmp(left->hostname, right->hostname) == 0 &&
           osStrcmp(left->ca, right->ca) == 0 &&
           osStrcmp(left->certificate, right->certificate) == 0 &&
           osStrcmp(left->private_key, right->private_key) == 0;
}

static bool_t upstream_wanted_locked(tb2_mqtt_upstream_worker_t *worker,
                                    uint64_t revision)
{
    /* Planned shutdown must not interrupt a partially written TLS/MQTT packet.
     * Its fixed-slot worker can finish that write even after owner release. */
    return (worker->in_use && worker->enabled && worker->revision == revision) ||
           (worker->busy && worker->shutdown_requested);
}

static bool_t upstream_wanted(tb2_mqtt_upstream_worker_t *worker, uint64_t revision)
{
    osAcquireMutex(&worker->mutex);
    bool_t wanted = upstream_wanted_locked(worker, revision);
    osReleaseMutex(&worker->mutex);
    return wanted;
}

static void upstream_event_push_locked(tb2_mqtt_upstream_worker_t *worker,
                                       const tb2_mqtt_upstream_event_t *event)
{
    /* Capacity is reserved by the TX/RX counters; no completion is allocated
     * after accepting a packet, including the failure path. */
    size_t position = (worker->event_head + worker->event_count) % UPSTREAM_EVENT_CAPACITY;
    worker->events[position] = *event;
    worker->event_count++;
}

static void upstream_tx_complete_locked(tb2_mqtt_upstream_worker_t *worker,
                                        upstream_tx_t *tx, error_t error)
{
    osFreeMem(tx->data);
    worker->tx_bytes -= tx->length;
    if (worker->in_use)
    {
        tb2_mqtt_upstream_event_t event = {
            .type = error ? TB2_MQTT_UPSTREAM_TX_FAILED : TB2_MQTT_UPSTREAM_TX_COMPLETE,
            .owner_serial = tx->owner_serial, .epoch = tx->epoch,
            .token = tx->token, .error = error, .length = tx->length,
        };
        upstream_event_push_locked(worker, &event);
    }
    else
    {
        worker->tx_outstanding--;
    }
    osMemset(tx, 0, sizeof(*tx));
}

static error_t upstream_tls_init(HttpClientContext *context, TlsContext *tls)
{
    const tb2_mqtt_upstream_config_t *config = context->sourceCtx;
    error_t error = tlsSetPrng(tls, rand_get_algo(), rand_get_context());
    if (!error)
        error = tlsSetTrustedCaList(tls, config->ca, osStrlen(config->ca));
    if (!error)
        error = tlsAddCertificate(tls, config->certificate, osStrlen(config->certificate),
                                  config->private_key, osStrlen(config->private_key));
    if (!error)
        error = tlsSetServerName(tls, config->hostname);
    if (!error)
        tls_context_key_log_init(tls);
    return error;
}

static error_t upstream_connect(tb2_mqtt_upstream_worker_t *worker,
                                uint64_t revision, HttpClientContext *client,
                                const tb2_mqtt_upstream_config_t *config)
{
    client->serverName = config->hostname;
    client->sourceCtx = (void *)config;
    error_t error = httpClientSetTimeout(client, config->timeout_ms);
    if (!error)
        error = httpClientRegisterTlsInitCallback(client, upstream_tls_init);
    if (error)
        return error;
    void *resolver = resolve_host(config->hostname);
    if (resolver == NULL)
        return ERROR_ADDRESS_NOT_FOUND;
    error = ERROR_ADDRESS_NOT_FOUND;
    for (int position = 0; upstream_wanted(worker, revision); position++)
    {
        IpAddr address;
        if (!resolve_get_ip(resolver, position, &address))
            break;
        error = httpClientConnect(client, &address, config->port);
        if (!error)
            break;
    }
    resolve_free(resolver);
    if (!upstream_wanted(worker, revision))
        return ERROR_ABORTED;
    if (!error)
    {
        const TlsContext *tls = client->tlsContext;
        (void)tls; /* TRACE_LEVEL_OFF builds still use the same transport. */
        TRACE_DEBUG("TB2 MQTT upstream stage=client_auth certificate_request=%s response=%s resumed=%s\r\n",
                    tls->clientCertRequested ? "received" : "not_received",
                    !tls->clientCertRequested ? "not_sent" :
                        tls->cert != NULL ? "certificate_sent" : "empty_certificate",
                    tls->resume ? "true" : "false");
        error = socketSetTimeout(client->socket, TB2_MQTT_UPSTREAM_IO_TIMEOUT_MS);
    }
    return error;
}

static error_t upstream_write(tb2_mqtt_upstream_worker_t *worker, uint64_t revision,
                              TlsContext *tls, const upstream_tx_t *tx)
{
    size_t offset = 0;
    systime_t started = osGetSystemTime();
    while (offset < tx->length)
    {
        if (!upstream_wanted(worker, revision))
            return ERROR_ABORTED;
        size_t written = 0;
        error_t error = tlsWrite(tls, tx->data + offset, tx->length - offset, &written, 0);
        if (error)
            return error;
        if (written == 0)
            return ERROR_WRITE_FAILED;
        offset += written;
        if (offset < tx->length &&
            (systime_t)(osGetSystemTime() - started) >= TB2_MQTT_UPSTREAM_IO_TIMEOUT_MS)
            return ERROR_TIMEOUT;
    }
    return NO_ERROR;
}

static error_t upstream_receive(tb2_mqtt_upstream_worker_t *worker, uint64_t revision,
                                uint64_t owner_serial, uint64_t epoch,
                                HttpClientContext *client)
{
    if (!tlsIsRxReady(client->tlsContext) &&
        !(tcpWaitForEvents(client->socket, SOCKET_EVENT_RX_READY, 0) & SOCKET_EVENT_RX_READY))
        return NO_ERROR;
    uint8_t buffer[UPSTREAM_READ_BYTES];
    size_t received = 0;
    error_t error = tlsRead(client->tlsContext, buffer, sizeof(buffer), &received, 0);
    if (received > 0)
    {
        uint8_t *copy = osAllocMem(received);
        if (copy == NULL)
            return ERROR_OUT_OF_MEMORY;
        osMemcpy(copy, buffer, received);
        osAcquireMutex(&worker->mutex);
        if (!upstream_wanted_locked(worker, revision))
            error = ERROR_ABORTED;
        else if (worker->rx_count >= TB2_MQTT_UPSTREAM_QUEUE_MESSAGES ||
                 received > TB2_MQTT_UPSTREAM_QUEUE_BYTES - worker->rx_bytes)
            error = ERROR_OUT_OF_RESOURCES;
        else
        {
            tb2_mqtt_upstream_event_t event = {
                .type = TB2_MQTT_UPSTREAM_RX, .owner_serial = owner_serial,
                .epoch = epoch, .data = copy, .length = received,
            };
            upstream_event_push_locked(worker, &event);
            worker->rx_count++;
            worker->rx_bytes += received;
            copy = NULL;
        }
        osReleaseMutex(&worker->mutex);
        osFreeMem(copy);
    }
    if (error == ERROR_WOULD_BLOCK || error == ERROR_TIMEOUT)
        return NO_ERROR;
    return error ? error : received == 0 ? ERROR_END_OF_STREAM : NO_ERROR;
}

static void upstream_worker_task(void *context)
{
    tb2_mqtt_upstream_worker_t *worker = context;
    systime_t retry_at = 0;
    systime_t retry_delay = UPSTREAM_INITIAL_RETRY_MS;
    uint64_t previous_owner = 0;
    for (;;)
    {
        osAcquireMutex(&worker->mutex);
        if (worker->retry_now)
        {
            retry_at = osGetSystemTime();
            worker->retry_now = FALSE;
        }
        bool_t ready = worker->in_use && worker->enabled && worker->config != NULL &&
                       worker->event_count == 0 &&
                       (int32_t)(osGetSystemTime() - retry_at) >= 0;
        if (!ready)
        {
            systime_t wait = UPSTREAM_IDLE_WAIT_MS;
            systime_t now = osGetSystemTime();
            if (worker->in_use && worker->enabled &&
                (int32_t)(retry_at - now) > 0)
                wait = MIN(wait, (systime_t)(retry_at - now));
            osReleaseMutex(&worker->mutex);
            osWaitForEvent(&worker->wake, wait);
            continue;
        }
        worker->busy = TRUE;
        worker->session_confirmed = FALSE;
        uint64_t revision = worker->revision;
        uint64_t owner_serial = worker->owner_serial;
        if (owner_serial != previous_owner)
        {
            retry_delay = UPSTREAM_INITIAL_RETRY_MS;
            previous_owner = owner_serial;
        }
        uint64_t epoch = ++worker->epoch;
        if (epoch == 0)
            epoch = ++worker->epoch;
        tb2_mqtt_upstream_config_t *config = NULL;
        error_t error = upstream_config_copy(worker->config, &config);
        osReleaseMutex(&worker->mutex);

        HttpClientContext client;
        bool_t initialized = FALSE;
        bool_t was_connected = FALSE;
        bool_t mqtt_started = FALSE;
        if (!error)
        {
            error = httpClientInit(&client);
            initialized = !error;
        }
        if (!error)
            error = upstream_connect(worker, revision, &client, config);
        if (!error)
        {
            osAcquireMutex(&worker->mutex);
            if (!upstream_wanted_locked(worker, revision))
                error = ERROR_ABORTED;
            else
            {
                worker->connected = TRUE;
                was_connected = TRUE;
                tb2_mqtt_upstream_event_t event = {
                    .type = TB2_MQTT_UPSTREAM_CONNECTED,
                    .owner_serial = owner_serial, .epoch = epoch,
                };
                upstream_event_push_locked(worker, &event);
            }
            osReleaseMutex(&worker->mutex);
        }
        while (!error && upstream_wanted(worker, revision))
        {
            upstream_tx_t tx = {0};
            osAcquireMutex(&worker->mutex);
            if (worker->shutdown_requested)
            {
                osReleaseMutex(&worker->mutex);
                break;
            }
            if (worker->tx_count > 0 && upstream_wanted_locked(worker, revision))
            {
                tx = worker->tx[worker->tx_head];
                worker->tx_head = (worker->tx_head + 1) % TB2_MQTT_UPSTREAM_QUEUE_MESSAGES;
                worker->tx_count--;
            }
            osReleaseMutex(&worker->mutex);
            if (tx.data != NULL)
            {
                error = upstream_write(worker, revision, client.tlsContext, &tx);
                if (!error && tx.length > 0 && tx.data[0] == 0x10)
                    mqtt_started = TRUE;
                osAcquireMutex(&worker->mutex);
                upstream_tx_complete_locked(worker, &tx, error);
                osReleaseMutex(&worker->mutex);
            }
            if (!error)
                error = upstream_receive(worker, revision, owner_serial, epoch, &client);
            if (!error)
            {
                osAcquireMutex(&worker->mutex);
                bool_t more_tx = worker->tx_count > 0;
                osReleaseMutex(&worker->mutex);
                if (!more_tx)
                    osWaitForEvent(&worker->wake, UPSTREAM_POLL_MS);
            }
        }

        osAcquireMutex(&worker->mutex);
        worker->connected = FALSE;
        bool_t graceful = worker->shutdown_requested && was_connected && !error;
        if (worker->shutdown_requested || !upstream_wanted_locked(worker, revision))
            error = worker->abort_reason != NO_ERROR ? worker->abort_reason : ERROR_ABORTED;
        while (worker->tx_count > 0)
        {
            upstream_tx_t tx = worker->tx[worker->tx_head];
            worker->tx_head = (worker->tx_head + 1) % TB2_MQTT_UPSTREAM_QUEUE_MESSAGES;
            worker->tx_count--;
            upstream_tx_complete_locked(worker, &tx, error ? error : ERROR_ABORTED);
        }
        osReleaseMutex(&worker->mutex);
        if (graceful && mqtt_started)
        {
            uint8_t disconnect[] = {0xe0, 0};
            upstream_tx_t final = {.data = disconnect, .length = sizeof(disconnect)};
            error_t disconnect_error = upstream_write(worker, revision, client.tlsContext, &final);
            osAcquireMutex(&worker->mutex);
            if (worker->in_use)
            {
                tb2_mqtt_upstream_event_t event = {
                    .type = TB2_MQTT_UPSTREAM_DISCONNECT, .owner_serial = owner_serial,
                    .epoch = epoch, .error = disconnect_error, .length = sizeof(disconnect),
                };
                upstream_event_push_locked(worker, &event);
            }
            osReleaseMutex(&worker->mutex);
            if (disconnect_error) error = disconnect_error;
        }
        if (initialized)
            httpClientDeinit(&client);
        upstream_config_free(config);

        osAcquireMutex(&worker->mutex);
        if (worker->in_use)
        {
            tb2_mqtt_upstream_event_t event = {
                .type = TB2_MQTT_UPSTREAM_DOWN, .owner_serial = owner_serial,
                .epoch = epoch, .error = error,
            };
            upstream_event_push_locked(worker, &event);
        }
        worker->shutdown_requested = FALSE;
        worker->busy = FALSE;
        bool_t session_confirmed = worker->session_confirmed;
        osReleaseMutex(&worker->mutex);
        TRACE_DEBUG("TB2 MQTT upstream transport epoch=%llu disconnected error=%d\r\n",
                    (unsigned long long)epoch, (int)error);
        if (session_confirmed)
            retry_delay = UPSTREAM_INITIAL_RETRY_MS;
        retry_at = osGetSystemTime() + retry_delay;
        retry_delay = MIN(retry_delay * 2U, UPSTREAM_RETRY_MAX_MS);
    }
}

error_t tb2_mqtt_upstream_acquire(const tb2_mqtt_upstream_config_t *config,
                                uint64_t owner_serial, tb2_mqtt_upstream_worker_t **output)
{
    if (output == NULL)
        return ERROR_INVALID_PARAMETER;
    *output = NULL;
    tb2_mqtt_upstream_config_t *copy = NULL;
    error_t error = upstream_config_copy(config, &copy);
    if (error)
        return error;
    for (size_t i = 0; i < TB2_MQTT_UPSTREAM_SLOTS; i++)
    {
        tb2_mqtt_upstream_worker_t *worker = &workers[i];
        if (!worker->initialized)
        {
            if (!osCreateMutex(&worker->mutex))
                break;
            if (!osCreateEvent(&worker->wake))
            {
                osDeleteMutex(&worker->mutex);
                break;
            }
            OsTaskParameters parameters = OS_TASK_DEFAULT_PARAMS;
            parameters.stackSize = 32U * 1024U;
            if (osCreateTask("tb2-mqtt-upstream", upstream_worker_task, worker, &parameters) ==
                (OsTaskId)OS_INVALID_TASK_ID)
            {
                osDeleteEvent(&worker->wake);
                osDeleteMutex(&worker->mutex);
                break;
            }
            worker->initialized = TRUE;
        }
        osAcquireMutex(&worker->mutex);
        if (worker->in_use || worker->busy)
        {
            osReleaseMutex(&worker->mutex);
            continue;
        }
        worker->config = copy;
        worker->in_use = TRUE;
        worker->enabled = TRUE;
        worker->retry_now = TRUE;
        worker->owner_serial = owner_serial;
        worker->revision++;
        worker->abort_reason = ERROR_ABORTED;
        worker->shutdown_requested = FALSE;
        osReleaseMutex(&worker->mutex);
        osSetEvent(&worker->wake);
        *output = worker;
        return NO_ERROR;
    }
    upstream_config_free(copy);
    return ERROR_OUT_OF_RESOURCES;
}

error_t tb2_mqtt_upstream_reconfigure(tb2_mqtt_upstream_worker_t *worker,
                                    const tb2_mqtt_upstream_config_t *config,
                                    uint64_t owner_serial)
{
    if (worker == NULL)
        return ERROR_INVALID_PARAMETER;
    tb2_mqtt_upstream_config_t *copy = NULL;
    error_t error = upstream_config_copy(config, &copy);
    if (error)
        return error;
    osAcquireMutex(&worker->mutex);
    if (!worker->in_use)
        error = ERROR_INVALID_STATUS;
    else if (!worker->enabled || worker->owner_serial != owner_serial ||
             !upstream_config_equal(worker->config, copy))
    {
        if (worker->busy && worker->connected)
            worker->shutdown_requested = TRUE;
        upstream_config_free(worker->config);
        worker->config = copy;
        copy = NULL;
        worker->owner_serial = owner_serial;
        worker->enabled = TRUE;
        worker->retry_now = TRUE;
        worker->connected = FALSE;
        worker->revision++;
        worker->abort_reason = ERROR_ABORTED;
    }
    osReleaseMutex(&worker->mutex);
    upstream_config_free(copy);
    osSetEvent(&worker->wake);
    return error;
}

void tb2_mqtt_upstream_disable(tb2_mqtt_upstream_worker_t *worker)
{
    if (worker == NULL)
        return;
    osAcquireMutex(&worker->mutex);
    worker->enabled = FALSE;
    worker->connected = FALSE;
    worker->shutdown_requested = FALSE;
    worker->revision++;
    worker->abort_reason = ERROR_ABORTED;
    osReleaseMutex(&worker->mutex);
    osSetEvent(&worker->wake);
}

void tb2_mqtt_upstream_reconnect(tb2_mqtt_upstream_worker_t *worker, error_t reason)
{
    if (worker == NULL)
        return;
    osAcquireMutex(&worker->mutex);
    worker->connected = FALSE;
    worker->shutdown_requested = FALSE;
    worker->revision++;
    worker->abort_reason = reason ? reason : ERROR_ABORTED;
    osReleaseMutex(&worker->mutex);
    osSetEvent(&worker->wake);
}

void tb2_mqtt_upstream_shutdown(tb2_mqtt_upstream_worker_t *worker,
                               bool_t reconnect, error_t reason)
{
    if (worker == NULL)
        return;
    osAcquireMutex(&worker->mutex);
    if (worker->in_use)
    {
        if (worker->busy && worker->connected)
            worker->shutdown_requested = TRUE;
        worker->connected = FALSE;
        worker->enabled = reconnect;
        worker->revision++;
        worker->abort_reason = reason ? reason : ERROR_ABORTED;
    }
    osReleaseMutex(&worker->mutex);
    osSetEvent(&worker->wake);
}

void tb2_mqtt_upstream_confirm_session(tb2_mqtt_upstream_worker_t *worker, uint64_t epoch)
{
    if (worker == NULL || epoch == 0)
        return;
    osAcquireMutex(&worker->mutex);
    if (worker->in_use && worker->connected && worker->epoch == epoch)
        worker->session_confirmed = TRUE;
    osReleaseMutex(&worker->mutex);
}

void tb2_mqtt_upstream_release(tb2_mqtt_upstream_worker_t *worker)
{
    if (worker == NULL)
        return;
    osAcquireMutex(&worker->mutex);
    worker->in_use = FALSE;
    worker->enabled = FALSE;
    worker->connected = FALSE;
    worker->revision++;
    worker->abort_reason = ERROR_ABORTED;
    upstream_config_free(worker->config);
    worker->config = NULL;
    while (worker->event_count > 0)
    {
        tb2_mqtt_upstream_event_t *event = &worker->events[worker->event_head];
        if (event->type == TB2_MQTT_UPSTREAM_TX_COMPLETE || event->type == TB2_MQTT_UPSTREAM_TX_FAILED)
            worker->tx_outstanding--;
        osFreeMem(event->data);
        worker->event_head = (worker->event_head + 1) % UPSTREAM_EVENT_CAPACITY;
        worker->event_count--;
    }
    worker->rx_count = 0;
    worker->rx_bytes = 0;
    osReleaseMutex(&worker->mutex);
    osSetEvent(&worker->wake);
}

error_t tb2_mqtt_upstream_send(tb2_mqtt_upstream_worker_t *worker,
                             uint64_t epoch, uint64_t token,
                             uint8_t *owned_packet, size_t length)
{
    if (worker == NULL || owned_packet == NULL || length == 0)
        return ERROR_INVALID_PARAMETER;
    osAcquireMutex(&worker->mutex);
    error_t error = NO_ERROR;
    if (!worker->in_use || !worker->enabled || !worker->connected || worker->epoch != epoch)
        error = ERROR_NOT_CONNECTED;
    else if (worker->tx_outstanding >= TB2_MQTT_UPSTREAM_QUEUE_MESSAGES ||
             length > TB2_MQTT_UPSTREAM_QUEUE_BYTES - worker->tx_bytes)
    {
        error = ERROR_OUT_OF_RESOURCES;
        worker->connected = FALSE;
        worker->revision++;
        worker->abort_reason = error;
    }
    else
    {
        size_t position = (worker->tx_head + worker->tx_count) % TB2_MQTT_UPSTREAM_QUEUE_MESSAGES;
        worker->tx[position] = (upstream_tx_t){
            .data = owned_packet, .length = length, .token = token,
            .epoch = epoch, .owner_serial = worker->owner_serial,
        };
        worker->tx_count++;
        worker->tx_outstanding++;
        worker->tx_bytes += length;
    }
    osReleaseMutex(&worker->mutex);
    osSetEvent(&worker->wake);
    return error;
}

bool_t tb2_mqtt_upstream_poll(tb2_mqtt_upstream_worker_t *worker,
                            tb2_mqtt_upstream_event_t *event)
{
    if (worker == NULL || event == NULL)
        return FALSE;
    osMemset(event, 0, sizeof(*event));
    osAcquireMutex(&worker->mutex);
    bool_t found = worker->event_count > 0;
    if (found)
    {
        *event = worker->events[worker->event_head];
        worker->event_head = (worker->event_head + 1) % UPSTREAM_EVENT_CAPACITY;
        worker->event_count--;
        if (event->type == TB2_MQTT_UPSTREAM_RX)
        {
            worker->rx_count--;
            worker->rx_bytes -= event->length;
        }
        else if (event->type == TB2_MQTT_UPSTREAM_TX_COMPLETE || event->type == TB2_MQTT_UPSTREAM_TX_FAILED)
            worker->tx_outstanding--;
    }
    osReleaseMutex(&worker->mutex);
    if (found)
        osSetEvent(&worker->wake);
    return found;
}

void tb2_mqtt_upstream_event_free(tb2_mqtt_upstream_event_t *event)
{
    if (event != NULL)
    {
        osFreeMem(event->data);
        osMemset(event, 0, sizeof(*event));
    }
}

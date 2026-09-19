/* Actual worker and POSIX mutex/event/task implementation; no live sockets. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#define TRACE_LEVEL 0
#include "../src/tb2_mqtt_upstream.c"

static atomic_bool dns_block, dns_entered, write_block, write_entered;
static atomic_bool partial_failure;
static atomic_uint writes, write_fail_at, rx_chunks, disconnect_writes;

void *resolve_host(const char *host)
{
    assert(strcmp(host, "broker") == 0 || strcmp(host, "changed") == 0);
    atomic_store(&dns_entered, true);
    while (atomic_load(&dns_block)) osDelayTask(1);
    return (void *)1;
}
bool resolve_get_ip(void *resolver, int position, IpAddr *address)
{ (void)resolver; memset(address, 0, sizeof(*address)); return position == 0; }
void resolve_free(void *resolver) { (void)resolver; }
const PrngAlgo *rand_get_algo(void) { return NULL; }
void *rand_get_context(void) { return NULL; }
void tls_context_key_log_init(TlsContext *tls) { (void)tls; }
error_t tlsSetPrng(TlsContext *tls, const PrngAlgo *algo, void *context)
{ (void)tls; (void)algo; (void)context; return NO_ERROR; }
error_t tlsSetTrustedCaList(TlsContext *tls, const char *ca, size_t length)
{ (void)tls; assert(length == 2 && !strcmp(ca, "CA")); return NO_ERROR; }
error_t tlsAddCertificate(TlsContext *tls, const char *cert, size_t cert_len,
                          const char *key, size_t key_len)
{
    (void)tls;
    assert(cert_len == 4 && !strcmp(cert, "CERT") && key_len == 3 && !strcmp(key, "KEY"));
    return NO_ERROR;
}
error_t tlsSetServerName(TlsContext *tls, const char *name)
{ (void)tls; assert(name[0]); return NO_ERROR; }
error_t httpClientInit(HttpClientContext *client)
{ memset(client, 0, sizeof(*client)); return NO_ERROR; }
error_t httpClientSetTimeout(HttpClientContext *client, systime_t timeout)
{ (void)client; assert(timeout == 500); return NO_ERROR; }
error_t httpClientRegisterTlsInitCallback(HttpClientContext *client, HttpClientTlsInitCallback callback)
{ client->tlsInitCallback = callback; return NO_ERROR; }
error_t httpClientConnect(HttpClientContext *client, const IpAddr *address, uint16_t port)
{
    (void)address; assert(port == 8883);
    client->tlsContext = calloc(1, sizeof(TlsContext));
    client->socket = calloc(1, sizeof(Socket));
    assert(client->tlsContext && client->socket);
    return client->tlsInitCallback(client, client->tlsContext);
}
void httpClientDeinit(HttpClientContext *client)
{ free(client->tlsContext); free(client->socket); }
error_t socketSetTimeout(Socket *socket, systime_t timeout)
{ assert(socket && timeout == 500); return NO_ERROR; }
uint_t tcpWaitForEvents(Socket *socket, uint_t mask, systime_t timeout)
{ (void)socket; (void)mask; assert(timeout == 0); return 0; }
bool_t tlsIsRxReady(TlsContext *tls) { (void)tls; return atomic_load(&rx_chunks) != 0; }
error_t tlsRead(TlsContext *tls, void *data, size_t size, size_t *received, uint_t flags)
{
    (void)tls; (void)flags; assert(atomic_load(&rx_chunks));
    atomic_fetch_sub(&rx_chunks, 1); memset(data, 0x55, size); *received = size;
    return NO_ERROR;
}
error_t tlsWrite(TlsContext *tls, const void *data, size_t length, size_t *written, uint_t flags)
{
    (void)tls; (void)data; (void)flags;
    atomic_store(&write_entered, true);
    while (atomic_load(&write_block)) osDelayTask(1);
    unsigned number = atomic_fetch_add(&writes, 1) + 1;
    *written = 0;
    if (number == atomic_load(&write_fail_at))
    {
        if (atomic_load(&partial_failure)) { *written = length / 2; return ERROR_TIMEOUT; }
        return ERROR_WRITE_FAILED;
    }
    if (length == 2 && ((const uint8_t *)data)[0] == 0xe0 && ((const uint8_t *)data)[1] == 0)
        atomic_fetch_add(&disconnect_writes, 1);
    *written = length; return NO_ERROR;
}

static tb2_mqtt_upstream_config_t config = {
    .hostname = "broker", .port = 8883, .timeout_ms = 500,
    .ca = "CA", .certificate = "CERT", .private_key = "KEY",
};
static void wait_flag(atomic_bool *flag)
{
    systime_t start = osGetSystemTime();
    while (!atomic_load(flag)) { assert((systime_t)(osGetSystemTime() - start) < 2000); osDelayTask(1); }
}
static bool_t busy(tb2_mqtt_upstream_worker_t *worker)
{
    osAcquireMutex(&worker->mutex); bool_t result = worker->busy;
    osReleaseMutex(&worker->mutex); return result;
}
static void wait_idle(tb2_mqtt_upstream_worker_t *worker)
{
    systime_t start = osGetSystemTime();
    while (busy(worker)) { assert((systime_t)(osGetSystemTime() - start) < 2500); osDelayTask(1); }
}
static tb2_mqtt_upstream_event_t event_wait(tb2_mqtt_upstream_worker_t *worker)
{
    tb2_mqtt_upstream_event_t event;
    systime_t start = osGetSystemTime();
    while (!tb2_mqtt_upstream_poll(worker, &event))
    { assert((systime_t)(osGetSystemTime() - start) < 4000); osDelayTask(1); }
    return event;
}
static uint64_t connected(tb2_mqtt_upstream_worker_t *worker, uint64_t serial)
{
    tb2_mqtt_upstream_event_t event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_CONNECTED && event.owner_serial == serial);
    return event.epoch;
}
static uint8_t *packet(size_t length)
{ uint8_t *data = malloc(length); assert(data); memset(data, 0x30, length); return data; }

static void test_blocked_dns_and_owner_release(void)
{
    atomic_store(&dns_block, true); atomic_store(&dns_entered, false);
    char hostname[] = "broker", key[] = "KEY";
    tb2_mqtt_upstream_config_t source = config;
    source.hostname = hostname; source.private_key = key;
    tb2_mqtt_upstream_worker_t *worker;
    assert(!tb2_mqtt_upstream_acquire(&source, 1, &worker));
    strcpy(hostname, "broken"); strcpy(key, "BAD");
    wait_flag(&dns_entered);
    systime_t start = osGetSystemTime();
    tb2_mqtt_upstream_event_t event;
    assert(!tb2_mqtt_upstream_poll(worker, &event));
    uint8_t *data = packet(1);
    assert(tb2_mqtt_upstream_send(worker, 1, 1, data, 1) == ERROR_NOT_CONNECTED); free(data);
    tb2_mqtt_upstream_disable(worker); tb2_mqtt_upstream_release(worker);
    assert((systime_t)(osGetSystemTime() - start) < 200);
    tb2_mqtt_upstream_worker_t *next;
    assert(!tb2_mqtt_upstream_acquire(&config, 2, &next) && next != worker);
    tb2_mqtt_upstream_release(next);
    atomic_store(&dns_block, false); wait_idle(worker); wait_idle(next);
    assert(!tb2_mqtt_upstream_poll(worker, &event));
}

static void test_tx_order_reconnect_and_reconfigure(void)
{
    tb2_mqtt_upstream_worker_t *worker;
    assert(!tb2_mqtt_upstream_acquire(&config, 3, &worker));
    uint64_t epoch = connected(worker, 3);
    uint64_t revision = worker->revision;
    assert(!tb2_mqtt_upstream_reconfigure(worker, &config, 3));
    assert(worker->revision == revision);
    atomic_store(&write_block, true); atomic_store(&write_entered, false);
    atomic_store(&writes, 0); atomic_store(&write_fail_at, 2);
    for (uint64_t token = 1; token <= 3; token++)
        assert(!tb2_mqtt_upstream_send(worker, epoch, token, packet(4), 4));
    wait_flag(&write_entered); atomic_store(&write_block, false);
    for (uint64_t token = 1; token <= 3; token++)
    {
        tb2_mqtt_upstream_event_t event = event_wait(worker);
        assert(event.token == token && event.owner_serial == 3 && event.epoch == epoch);
        assert(event.type == (token == 1 ? TB2_MQTT_UPSTREAM_TX_COMPLETE : TB2_MQTT_UPSTREAM_TX_FAILED));
    }
    tb2_mqtt_upstream_event_t down = event_wait(worker);
    assert(down.type == TB2_MQTT_UPSTREAM_DOWN && down.error == ERROR_WRITE_FAILED);
    systime_t failed_at = osGetSystemTime();
    uint64_t next_epoch = connected(worker, 3);
    assert(next_epoch > epoch && (systime_t)(osGetSystemTime() - failed_at) >= 900);
    uint8_t *old = packet(1);
    assert(tb2_mqtt_upstream_send(worker, epoch, 9, old, 1) == ERROR_NOT_CONNECTED); free(old);
    /* A TLS-only connection does not reset backoff. A stale READY notification
     * cannot mark the new transport successful either. */
    tb2_mqtt_upstream_confirm_session(worker, epoch);
    assert(!worker->session_confirmed);
    tb2_mqtt_upstream_reconnect(worker, ERROR_FAILURE);
    down = event_wait(worker); assert(down.type == TB2_MQTT_UPSTREAM_DOWN);
    failed_at = osGetSystemTime();
    epoch = connected(worker, 3);
    assert(epoch > next_epoch && (systime_t)(osGetSystemTime() - failed_at) >= 1900);
    assert(!worker->session_confirmed);
    tb2_mqtt_upstream_confirm_session(worker, epoch);
    assert(worker->session_confirmed);
    tb2_mqtt_upstream_reconnect(worker, ERROR_FAILURE);
    down = event_wait(worker); assert(down.type == TB2_MQTT_UPSTREAM_DOWN);
    failed_at = osGetSystemTime();
    next_epoch = connected(worker, 3);
    systime_t retry_elapsed = osGetSystemTime() - failed_at;
    assert(next_epoch > epoch && retry_elapsed >= 900 && retry_elapsed < 1800);
    assert(!worker->session_confirmed);
    tb2_mqtt_upstream_config_t changed = config; changed.hostname = "changed";
    assert(!tb2_mqtt_upstream_reconfigure(worker, &changed, 4));
    down = event_wait(worker); assert(down.type == TB2_MQTT_UPSTREAM_DOWN && down.owner_serial == 3);
    assert(connected(worker, 4) > next_epoch);
    tb2_mqtt_upstream_disable(worker); down = event_wait(worker);
    assert(down.type == TB2_MQTT_UPSTREAM_DOWN && down.owner_serial == 4);
    tb2_mqtt_upstream_release(worker); wait_idle(worker);
}

static void test_tx_and_rx_limits(void)
{
    tb2_mqtt_upstream_worker_t *worker;
    assert(!tb2_mqtt_upstream_acquire(&config, 5, &worker));
    uint64_t epoch = connected(worker, 5);
    atomic_store(&write_block, true); atomic_store(&write_entered, false);
    atomic_store(&write_fail_at, 0);
    assert(!tb2_mqtt_upstream_send(worker, epoch, 1, packet(1), 1)); wait_flag(&write_entered);
    for (uint64_t i = 2; i <= 32; i++) assert(!tb2_mqtt_upstream_send(worker, epoch, i, packet(1), 1));
    uint8_t *extra = packet(1);
    assert(tb2_mqtt_upstream_send(worker, epoch, 33, extra, 1) == ERROR_OUT_OF_RESOURCES); free(extra);
    atomic_store(&write_block, false);
    for (unsigned i = 0; i < 32; i++)
    {
        tb2_mqtt_upstream_event_t event = event_wait(worker);
        assert(event.type == TB2_MQTT_UPSTREAM_TX_FAILED || event.type == TB2_MQTT_UPSTREAM_TX_COMPLETE);
    }
    tb2_mqtt_upstream_event_t event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_DOWN && event.error == ERROR_OUT_OF_RESOURCES);
    assert(!tb2_mqtt_upstream_reconfigure(worker, &config, 6));
    epoch = connected(worker, 6);
    atomic_store(&write_block, true); atomic_store(&write_entered, false);
    assert(!tb2_mqtt_upstream_send(worker, epoch, 1, packet(TB2_MQTT_UPSTREAM_QUEUE_BYTES), TB2_MQTT_UPSTREAM_QUEUE_BYTES));
    wait_flag(&write_entered); extra = packet(1);
    assert(tb2_mqtt_upstream_send(worker, epoch, 2, extra, 1) == ERROR_OUT_OF_RESOURCES); free(extra);
    atomic_store(&write_block, false); event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_TX_COMPLETE || event.type == TB2_MQTT_UPSTREAM_TX_FAILED);
    event = event_wait(worker); assert(event.type == TB2_MQTT_UPSTREAM_DOWN);
    assert(!tb2_mqtt_upstream_reconfigure(worker, &config, 7)); connected(worker, 7);
    atomic_store(&rx_chunks, 33); wait_idle(worker);
    for (unsigned i = 0; i < 32; i++)
    {
        event = event_wait(worker); assert(event.type == TB2_MQTT_UPSTREAM_RX && event.length == UPSTREAM_READ_BYTES);
        tb2_mqtt_upstream_event_free(&event);
    }
    event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_DOWN && event.error == ERROR_OUT_OF_RESOURCES);
    tb2_mqtt_upstream_release(worker); wait_idle(worker);
}

static uint64_t begin_mqtt(tb2_mqtt_upstream_worker_t **worker, uint64_t owner)
{
    atomic_store(&write_fail_at, 0); atomic_store(&write_block, false);
    assert(!tb2_mqtt_upstream_acquire(&config, owner, worker));
    uint64_t epoch = connected(*worker, owner);
    uint8_t *connect = packet(2); connect[0] = 0x10; connect[1] = 0;
    assert(!tb2_mqtt_upstream_send(*worker, epoch, 1, connect, 2));
    tb2_mqtt_upstream_event_t event = event_wait(*worker);
    assert(event.type == TB2_MQTT_UPSTREAM_TX_COMPLETE && event.token == 1);
    return epoch;
}

static void test_graceful_shutdown_and_detached_close(void)
{
    tb2_mqtt_upstream_worker_t *worker;
    unsigned before = atomic_load(&disconnect_writes);
    uint64_t epoch = begin_mqtt(&worker, 8);
    atomic_store(&write_block, true); atomic_store(&write_entered, false);
    assert(!tb2_mqtt_upstream_send(worker, epoch, 2, packet(4), 4)); wait_flag(&write_entered);
    assert(!tb2_mqtt_upstream_send(worker, epoch, 3, packet(4), 4));
    tb2_mqtt_upstream_shutdown(worker, false, ERROR_ABORTED);
    atomic_store(&write_block, false);
    tb2_mqtt_upstream_event_t event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_TX_COMPLETE && event.token == 2);
    event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_TX_FAILED && event.token == 3);
    event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_DISCONNECT && !event.error && event.length == 2);
    event = event_wait(worker); assert(event.type == TB2_MQTT_UPSTREAM_DOWN);
    assert(atomic_load(&disconnect_writes) == before + 1);
    tb2_mqtt_upstream_release(worker); wait_idle(worker);

    epoch = begin_mqtt(&worker, 9);
    atomic_store(&write_block, true); atomic_store(&write_entered, false);
    assert(!tb2_mqtt_upstream_send(worker, epoch, 2, packet(4), 4)); wait_flag(&write_entered);
    systime_t started = osGetSystemTime();
    tb2_mqtt_upstream_shutdown(worker, false, ERROR_ABORTED);
    tb2_mqtt_upstream_release(worker);
    assert((systime_t)(osGetSystemTime() - started) < 200);
    atomic_store(&write_block, false); wait_idle(worker);
    assert(atomic_load(&disconnect_writes) == before + 2);
    assert(!tb2_mqtt_upstream_poll(worker, &event));

    begin_mqtt(&worker, 10);
    atomic_store(&write_fail_at, atomic_load(&writes) + 1);
    tb2_mqtt_upstream_shutdown(worker, false, ERROR_ABORTED);
    event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_DISCONNECT && event.error == ERROR_WRITE_FAILED);
    event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_DOWN && event.error == ERROR_WRITE_FAILED);
    assert(atomic_load(&disconnect_writes) == before + 2);
    tb2_mqtt_upstream_release(worker); wait_idle(worker);

    epoch = begin_mqtt(&worker, 11);
    atomic_store(&partial_failure, true);
    atomic_store(&write_fail_at, atomic_load(&writes) + 1);
    assert(!tb2_mqtt_upstream_send(worker, epoch, 2, packet(4), 4));
    event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_TX_FAILED && event.error == ERROR_TIMEOUT);
    event = event_wait(worker);
    assert(event.type == TB2_MQTT_UPSTREAM_DOWN && event.error == ERROR_TIMEOUT);
    assert(atomic_load(&disconnect_writes) == before + 2);
    tb2_mqtt_upstream_release(worker); wait_idle(worker);
    atomic_store(&partial_failure, false);
}

int main(void)
{
    test_blocked_dns_and_owner_release();
    test_tx_order_reconnect_and_reconfigure();
    test_tx_and_rx_limits();
    test_graceful_shutdown_and_detached_close();
    puts("MQTT upstream worker PASS: blocked DNS, snapshots, epochs, ordered completions, retry, bounds and graceful/detached shutdown");
    return 0;
}

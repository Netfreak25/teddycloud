#ifndef TB2_MQTT_UPSTREAM_H
#define TB2_MQTT_UPSTREAM_H

#include "core/net.h"

#define TB2_MQTT_UPSTREAM_SLOTS 32U
#define TB2_MQTT_UPSTREAM_QUEUE_MESSAGES 32U
#define TB2_MQTT_UPSTREAM_QUEUE_BYTES (1024U * 1024U)
#define TB2_MQTT_UPSTREAM_IO_TIMEOUT_MS 500U

typedef struct tb2_mqtt_upstream_worker tb2_mqtt_upstream_worker_t;

/** Strings are copied synchronously; the caller must keep them stable during
 * acquire/reconfigure (hold the settings lock when passing settings strings). */
typedef struct
{
    const char *hostname;
    uint16_t port;
    systime_t timeout_ms;
    const char *ca;
    const char *certificate;
    const char *private_key;
} tb2_mqtt_upstream_config_t;

typedef enum
{
    TB2_MQTT_UPSTREAM_CONNECTED,
    TB2_MQTT_UPSTREAM_RX,
    TB2_MQTT_UPSTREAM_TX_COMPLETE,
    TB2_MQTT_UPSTREAM_TX_FAILED,
    TB2_MQTT_UPSTREAM_DISCONNECT,
    TB2_MQTT_UPSTREAM_DOWN
} tb2_mqtt_upstream_event_type_t;

typedef struct
{
    tb2_mqtt_upstream_event_type_t type;
    uint64_t owner_serial;
    uint64_t epoch;
    uint64_t token;
    error_t error;
    uint8_t *data;
    size_t length;
} tb2_mqtt_upstream_event_t;

/** Main-thread lifecycle API. Fixed task slots own only transport/configuration,
 * never box/session pointers. No lifecycle call waits for DNS or network I/O. */
error_t tb2_mqtt_upstream_acquire(const tb2_mqtt_upstream_config_t *config,
                                uint64_t owner_serial,
                                tb2_mqtt_upstream_worker_t **worker);
error_t tb2_mqtt_upstream_reconfigure(tb2_mqtt_upstream_worker_t *worker,
                                    const tb2_mqtt_upstream_config_t *config,
                                    uint64_t owner_serial);
void tb2_mqtt_upstream_disable(tb2_mqtt_upstream_worker_t *worker);
void tb2_mqtt_upstream_reconnect(tb2_mqtt_upstream_worker_t *worker, error_t reason);
/** Mark the exact transport epoch as fully MQTT-ready (CONNACK and required
 * subscriptions accepted). Only this success resets failure backoff; merely
 * completing TLS does not. Stale or disconnected epochs are ignored. */
void tb2_mqtt_upstream_confirm_session(tb2_mqtt_upstream_worker_t *worker, uint64_t epoch);
/** Planned shutdown: finish the current write, cancel queued packets, then send
 * MQTT DISCONNECT if CONNECT was fully written and the transport is intact.
 * DISCONNECT event (length=2, data=NULL) reports the actual write result before
 * DOWN. This remains best-effort on a failed network. release() may detach this
 * bounded cleanup job; detached jobs never call back into their former owner. */
void tb2_mqtt_upstream_shutdown(tb2_mqtt_upstream_worker_t *worker,
                               bool_t reconnect, error_t reason);
/** Releases ownership and discards remaining events. A busy slot is not reused
 * until its worker has retired the old transport; its task itself remains. */
void tb2_mqtt_upstream_release(tb2_mqtt_upstream_worker_t *worker);

/** Ownership transfers only on NO_ERROR. No offline queue: epoch must identify
 * the currently connected transport. Each accepted token produces exactly one
 * TX_COMPLETE/TX_FAILED before DOWN, unless the owner explicitly releases it.
 * TX_COMPLETE means the entire buffer was written, not an MQTT acknowledgement. */
error_t tb2_mqtt_upstream_send(tb2_mqtt_upstream_worker_t *worker,
                             uint64_t epoch, uint64_t token,
                             uint8_t *owned_packet, size_t length);
bool_t tb2_mqtt_upstream_poll(tb2_mqtt_upstream_worker_t *worker,
                            tb2_mqtt_upstream_event_t *event);
void tb2_mqtt_upstream_event_free(tb2_mqtt_upstream_event_t *event);

#endif

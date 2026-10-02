#ifndef TB2_MQTT_PASSTHROUGH_H
#define TB2_MQTT_PASSTHROUGH_H

#include "compiler_port.h"
#include "error.h"
#include "settings.h"
#include "mqtt_delivery.h"

struct _HttpConnection;
struct _Socket;
struct _TlsContext;
typedef struct tb2_mqtt_passthrough_session tb2_mqtt_passthrough_session_t;
typedef enum
{
    TB2_MQTT_OBSERVER_FORWARD = 0,
    TB2_MQTT_OBSERVER_CONSUME,
    TB2_MQTT_OBSERVER_REWRITE,
} tb2_mqtt_observer_action_t;

typedef struct
{
    tb2_mqtt_observer_action_t action;
    uint8_t *payload;
    size_t payload_len;
    const char *filter_id;
    const char *capture_action;
    bool_t locally_processed;
} tb2_mqtt_observer_result_t;

typedef error_t (*tb2_mqtt_publish_observer_t)(
    void *context, bool_t box_to_upstream, const char *topic,
    const uint8_t *payload, size_t payload_len, uint8_t qos,
    tb2_mqtt_observer_result_t *result);
/** Mainloop completion, outside io_mutex. Failure/cancel handlers must not take
 * the session lifetime lock: connection teardown can already hold it. */
typedef void (*tb2_mqtt_local_write_completed_t)(void *context, error_t error);
typedef struct {
    const uint8_t *packet;
    size_t packet_size;
    const char *topic;
    uint16_t packet_id;
    const char *capture_action;
    tb2_mqtt_local_write_completed_t completed;
    void *context;
    size_t context_bytes;
} tb2_mqtt_local_publish_t;

/** Atomic admission: QUEUED owns callback/context until exactly one completion;
 * BUSY/FAILED own nothing. Packet/topic/action bytes are copied on admission. */
mqtt_delivery_result_t tb2_mqtt_passthrough_submit_local_batch(
    tb2_mqtt_passthrough_session_t *session,
    const tb2_mqtt_local_publish_t *publishes, size_t count);
mqtt_delivery_result_t tb2_mqtt_passthrough_submit_local_publish(
    tb2_mqtt_passthrough_session_t *session, const uint8_t *packet,
    size_t packet_size, const char *topic, uint16_t packet_id,
    const char *capture_action, tb2_mqtt_local_write_completed_t completed,
    void *context, size_t context_bytes);
typedef enum
{
    TB2_MQTT_CONTROL_SUBSCRIBE,
    TB2_MQTT_CONTROL_UNSUBSCRIBE,
    TB2_MQTT_CONTROL_LOCAL_PUBACK,
} tb2_mqtt_control_event_t;
typedef void (*tb2_mqtt_control_observer_t)(void *context,
                                            tb2_mqtt_control_event_t event,
                                            uint16_t packet_id,
                                            const uint8_t *payload,
                                            size_t payload_len);

error_t tb2_mqtt_passthrough_init(void);
void tb2_mqtt_passthrough_deinit(void);

bool_t tb2_mqtt_passthrough_is_enabled(void);
error_t tb2_mqtt_passthrough_start(struct _TlsContext *box_tls,
                                   struct _Socket *box_socket,
                                   tb2_mqtt_passthrough_session_t **session,
                                   bool_t *handled,
                                   tb2_mqtt_publish_observer_t observer,
                                   tb2_mqtt_control_observer_t control_observer,
                                   void *observer_context,
                                   settings_t **box_settings_out);
error_t tb2_mqtt_passthrough_forward_initial(tb2_mqtt_passthrough_session_t *session,
                                             const uint8_t *data, size_t length);
error_t tb2_mqtt_passthrough_task(tb2_mqtt_passthrough_session_t *session);
bool_t tb2_mqtt_passthrough_is_established(const tb2_mqtt_passthrough_session_t *session);
/* Diagnostic identities only; message/epoch are scoped to synchronous callbacks. */
uint64_t tb2_mqtt_passthrough_debug_owner(const tb2_mqtt_passthrough_session_t *session);
uint64_t tb2_mqtt_passthrough_debug_message(const tb2_mqtt_passthrough_session_t *session);
uint64_t tb2_mqtt_passthrough_debug_epoch(const tb2_mqtt_passthrough_session_t *session);
/* First terminal box-write error; recovered transient errors are not stored. */
error_t tb2_mqtt_passthrough_box_write_error(const tb2_mqtt_passthrough_session_t *session);
error_t tb2_mqtt_passthrough_reserve_local_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t *packet_id);
void tb2_mqtt_passthrough_release_local_packet_id(
    tb2_mqtt_passthrough_session_t *session, uint16_t packet_id);
error_t tb2_mqtt_passthrough_write_local_publish(
    tb2_mqtt_passthrough_session_t *session, const uint8_t *packet,
    size_t packet_size, const char *topic, uint16_t packet_id,
    const char *capture_action);
void tb2_mqtt_passthrough_close(tb2_mqtt_passthrough_session_t *session,
                                const char *result_code, bool_t success);

error_t tb2_mqtt_passthrough_write_status(struct _HttpConnection *connection);

#endif

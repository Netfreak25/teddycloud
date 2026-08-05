#ifndef TB2_MQTT_PASSTHROUGH_H
#define TB2_MQTT_PASSTHROUGH_H

#include "compiler_port.h"
#include "error.h"
#include "settings.h"

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

bool_t tb2_mqtt_passthrough_is_armed(void);
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

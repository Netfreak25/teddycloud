#ifndef TB2_MQTT_PASSTHROUGH_H
#define TB2_MQTT_PASSTHROUGH_H

#include "compiler_port.h"
#include "error.h"
#include "settings.h"

struct _HttpConnection;
struct _Socket;
struct _TlsContext;
typedef struct tb2_mqtt_passthrough_session tb2_mqtt_passthrough_session_t;
typedef void (*tb2_mqtt_publish_observer_t)(void *context, bool_t box_to_upstream,
                                            const char *topic, const uint8_t *payload,
                                            size_t payload_len, uint8_t qos);

error_t tb2_mqtt_passthrough_init(void);
void tb2_mqtt_passthrough_deinit(void);

bool_t tb2_mqtt_passthrough_is_armed(void);
error_t tb2_mqtt_passthrough_start(struct _TlsContext *box_tls,
                                   struct _Socket *box_socket,
                                   tb2_mqtt_passthrough_session_t **session,
                                   bool_t *handled,
                                   tb2_mqtt_publish_observer_t observer,
                                   void *observer_context,
                                   settings_t **box_settings_out);
error_t tb2_mqtt_passthrough_forward_initial(tb2_mqtt_passthrough_session_t *session,
                                             const uint8_t *data, size_t length);
error_t tb2_mqtt_passthrough_task(tb2_mqtt_passthrough_session_t *session);
void tb2_mqtt_passthrough_close(tb2_mqtt_passthrough_session_t *session,
                                const char *result_code, bool_t success);

error_t tb2_mqtt_passthrough_write_status(struct _HttpConnection *connection);

#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "core/net.h"
#include "core/socket.h"
#include "error.h"
#include "debug.h"
#include "settings.h"
#include "mqtt_server.h"
#include "tls.h"
#include "rand.h"
#include "tls_adapter.h"
#include "cJSON.h"
#include "handler.h"
#include "toniebox_state.h"

// Forward declaration of platform-specific function
uint_t tcpWaitForEvents(Socket *socket, uint_t eventMask, systime_t timeout);

#define MQTT_MAX_PACKET_SIZE 4096

typedef struct {
    Socket *socket;
    TlsContext *tlsContext;
    bool active;
    uint8_t buffer[MQTT_MAX_PACKET_SIZE];
    size_t buffer_len;
} MqttClientConnection;

static Socket *serverSocket = NULL;
static MqttClientConnection connection;

static char_t *mqtt_server_cert = NULL;
static size_t mqtt_server_cert_len = 0;
static char_t *mqtt_server_key = NULL;
static size_t mqtt_server_key_len = 0;

static char_t *mqtt_server_read_file(const char_t *filename, size_t *length)
{
    *length = 0;
    uint32_t fileSize = 0;
    error_t error = fsGetFileSize(filename, &fileSize);
    if (error != NO_ERROR)
        return NULL;

    char_t *buffer = osAllocMem(fileSize + 1);
    if (buffer == NULL)
        return NULL;

    FsFile *fp = fsOpenFile(filename, FS_FILE_MODE_READ);
    if (fp == NULL)
    {
        osFreeMem(buffer);
        return NULL;
    }

    size_t read = 0;
    error = fsReadFile(fp, buffer, fileSize, &read);
    fsCloseFile(fp);

    if (error != NO_ERROR || read != fileSize)
    {
        osFreeMem(buffer);
        return NULL;
    }

    buffer[fileSize] = '\0';
    *length = fileSize;
    return buffer;
}

error_t mqtt_server_tls_init(TlsContext *tlsContext)
{
    error_t error;

    error = tlsSetConnectionEnd(tlsContext, TLS_CONNECTION_END_SERVER);
    if (error)
        return error;

    error = tlsSetBufferSize(tlsContext, TLS_TX_BUFFER_SIZE, TLS_RX_BUFFER_SIZE);
    if (error)
        return error;

    error = tlsSetPrng(tlsContext, rand_get_algo(), rand_get_context());
    if (error)
        return error;

    error = tlsSetCache(tlsContext, tlsCache);
    if (error)
        return error;

    error = tlsEnableSecureRenegotiation(tlsContext, TRUE);
    if (error)
        return error;

    error = tlsSetClientAuthMode(tlsContext, TLS_CLIENT_AUTH_NONE);
    if (error)
        return error;

    if (mqtt_server_cert && mqtt_server_key)
    {
        error = tlsLoadCertificate(tlsContext, 0, mqtt_server_cert, mqtt_server_cert_len, mqtt_server_key, mqtt_server_key_len, NULL);
    }
    else
    {
        TRACE_ERROR("Certificates not loaded\r\n");
        error = ERROR_FAILURE;
    }

    return error;
}

void mqtt_server_init() {
    settings_t *settings = get_settings();
    if (!settings->mqtt_server.enabled) return;

    TRACE_INFO("Initializing on port %u\r\n", settings->mqtt_server.port);

    // Load certificates once
    char *cert_path = osAllocMem(256);
    char *key_path = osAllocMem(256);

    settings_resolve_dir(&cert_path, settings->mqtt_server.cert_crt, settings->internal.basedirfull);
    settings_resolve_dir(&key_path, settings->mqtt_server.cert_key, settings->internal.basedirfull);

    mqtt_server_cert = mqtt_server_read_file(cert_path, &mqtt_server_cert_len);
    mqtt_server_key = mqtt_server_read_file(key_path, &mqtt_server_key_len);

    if (!mqtt_server_cert || !mqtt_server_key)
    {
        TRACE_ERROR("Failed to load certificates (cert: %s, key: %s)\r\n", cert_path, key_path);
    }

    osFreeMem(cert_path);
    osFreeMem(key_path);

    serverSocket = socketOpen(SOCKET_TYPE_STREAM, SOCKET_IP_PROTO_TCP);
    if (serverSocket == NULL) {
        TRACE_ERROR("Failed to open socket\r\n");
        return;
    }

    socketSetTimeout(serverSocket, 0); // Non-blocking

    error_t error = socketBind(serverSocket, &IP_ADDR_ANY, settings->mqtt_server.port);
    if (error != NO_ERROR) {
        TRACE_ERROR("Failed to bind socket (port %u)\r\n", settings->mqtt_server.port);
        socketClose(serverSocket);
        serverSocket = NULL;
        return;
    }

    error = socketListen(serverSocket, 5);
    if (error != NO_ERROR) {
        TRACE_ERROR("Failed to listen on socket\r\n");
        socketClose(serverSocket);
        serverSocket = NULL;
        return;
    }

    osMemset(&connection, 0, sizeof(connection));
}

static const char *settings_json = "{\n"
"  \"settings_history\": {\n"
"    \"bi_tracking\": 0,\n"
"    \"max_headphone_volume\": 0,\n"
"    \"battery_threshold\": 0,\n"
"    \"scrubbing_enabled\": 0,\n"
"    \"fleet_obs_enabled\": 0,\n"
"    \"bedtime_max_headphone_volume\": 0,\n"
"    \"timezone_transitions\": 0,\n"
"    \"log_level\": 0,\n"
"    \"timezone\": 0,\n"
"    \"bedtime_lightring_color\": 0,\n"
"    \"bedtime_max_volume\": 0,\n"
"    \"skipping_direction\": 0,\n"
"    \"max_volume\": 1773690866874,\n"
"    \"bedtime_schedules\": 0,\n"
"    \"alarms\": 0,\n"
"    \"bedtime_lightring_brightness\": 1773690530304,\n"
"    \"age_mode\": 1773690456997,\n"
"    \"skipping_enabled\": 0,\n"
"    \"lightring_brightness\": 1773306190892\n"
"  },\n"
"  \"settings_applied\": false,\n"
"  \"bi_tracking\": true,\n"
"  \"max_headphone_volume\": 100,\n"
"  \"battery_threshold\": 1,\n"
"  \"scrubbing_enabled\": false,\n"
"  \"fleet_obs_enabled\": true,\n"
"  \"bedtime_max_headphone_volume\": 75,\n"
"  \"timezone_transitions\": [\n"
"    {\n"
"      \"time\": 0,\n"
"      \"offset\": 0\n"
"    }\n"
"  ],\n"
"  \"log_level\": \"info\",\n"
"  \"timezone\": null,\n"
"  \"bedtime_lightring_color\": \"#ffffff\",\n"
"  \"bedtime_max_volume\": 75,\n"
"  \"skipping_direction\": \"right\",\n"
"  \"max_volume\": 100,\n"
"  \"bedtime_schedules\": [],\n"
"  \"alarms\": [],\n"
"  \"bedtime_lightring_brightness\": 75,\n"
"  \"age_mode\": \"1+\",\n"
"  \"skipping_enabled\": true,\n"
"  \"lightring_brightness\": 100\n"
"}";

static void mqtt_server_publish(const char *topic, const char *payload)
{
    if (!connection.active) return;

    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t remaining_len = 2 + topic_len + payload_len;

    // Allocate memory for the packet
    size_t header_len = 1; // 0x30
    uint8_t length_bytes[4];
    size_t length_count = 0;
    size_t temp_len = remaining_len;
    do {
        uint8_t encoded_byte = temp_len % 128;
        temp_len /= 128;
        if (temp_len > 0) {
            encoded_byte |= 128;
        }
        length_bytes[length_count++] = encoded_byte;
    } while (temp_len > 0);

    size_t packet_size = header_len + length_count + remaining_len;
    uint8_t *packet = osAllocMem(packet_size);
    if (packet == NULL)
    {
        TRACE_ERROR("Failed to allocate memory for MQTT PUBLISH packet\r\n");
        return;
    }

    size_t p = 0;
    packet[p++] = 0x30; // PUBLISH (QoS 0)
    
    memcpy(&packet[p], length_bytes, length_count);
    p += length_count;

    packet[p++] = (topic_len >> 8) & 0xFF;
    packet[p++] = topic_len & 0xFF;

    memcpy(&packet[p], topic, topic_len);
    p += topic_len;

    memcpy(&packet[p], payload, payload_len);
    p += payload_len;

    size_t written = 0;
    if (connection.tlsContext)
    {
        tlsWrite(connection.tlsContext, packet, packet_size, &written, 0);
    }
    else
    {
        socketSend(connection.socket, packet, packet_size, &written, 0);
    }

    TRACE_INFO("MQTT PUBLISH: %s -> %s (len %zu)\r\n", topic, payload, payload_len);

    osFreeMem(packet);
}

void mqtt_server_task()
{
    if (serverSocket == NULL)
        return;

    if (connection.active)
    {
        size_t received = 0;
        error_t error = NO_ERROR;

        // Non-blocking check for data
        if (tcpWaitForEvents(connection.socket, SOCKET_EVENT_RX_READY, 0) & SOCKET_EVENT_RX_READY)
        {
            if (connection.tlsContext)
            {
                error = tlsRead(connection.tlsContext, connection.buffer + connection.buffer_len, MQTT_MAX_PACKET_SIZE - connection.buffer_len, &received, 0);
            }
            else
            {
                error = socketReceive(connection.socket, connection.buffer + connection.buffer_len, MQTT_MAX_PACKET_SIZE - connection.buffer_len, &received, 0);
            }

            if (error == NO_ERROR && received > 0)
            {
                connection.buffer_len += received;
                size_t processed_total = 0;

                while (processed_total + 2 <= connection.buffer_len)
                {
                    uint8_t *pkt = &connection.buffer[processed_total];
                    size_t remaining_len = 0;
                    size_t multiplier = 1;
                    size_t pos = 1;
                    uint8_t digit;
                    bool rem_len_complete = false;

                    do
                    {
                        if (processed_total + pos >= connection.buffer_len) break;
                        digit = pkt[pos++];
                        remaining_len += (digit & 127) * multiplier;
                        multiplier *= 128;
                        if ((digit & 128) == 0) rem_len_complete = true;
                    } while (!rem_len_complete && pos < 5);

                    if (!rem_len_complete) break; // Need more bytes for remaining_len

                    size_t packet_size = pos + remaining_len;
                    if (processed_total + packet_size > connection.buffer_len) break; // Incomplete packet

                    // Process full packet
                    uint8_t cmd_raw = pkt[0];
                    uint8_t cmd = cmd_raw & 0xF0;

                    if (cmd == 0x10) // CONNECT
                    {
                        TRACE_INFO("CONNECT received\r\n");
                        uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
                        size_t written = 0;
                        if (connection.tlsContext)
                            tlsWrite(connection.tlsContext, connack, sizeof(connack), &written, 0);
                        else
                            socketSend(connection.socket, connack, sizeof(connack), &written, 0);
                    }
                    else if (cmd == 0x30) // PUBLISH
                    {
                        uint8_t flags = cmd_raw & 0x0F;
                        uint8_t qos = (flags >> 1) & 0x03;
                        size_t p = pos;

                        size_t topic_len = (pkt[p] << 8) | pkt[p + 1];
                        p += 2;
                        char topic[256];
                        size_t copy_len = topic_len < sizeof(topic) - 1 ? topic_len : sizeof(topic) - 1;
                        memcpy(topic, &pkt[p], copy_len);
                        topic[copy_len] = '\0';
                        p += topic_len;

                        uint16_t packet_id = 0;
                        if (qos > 0)
                        {
                            packet_id = (pkt[p] << 8) | pkt[p + 1];
                            p += 2;
                        }

                        size_t payload_len = remaining_len - (p - pos);
                        char payload[256];
                        copy_len = payload_len < sizeof(payload) - 1 ? payload_len : sizeof(payload) - 1;
                        memcpy(payload, &pkt[p], copy_len);
                        payload[copy_len] = '\0';

                        bool is_log = (strncmp(topic, "toniebox/", 9) == 0 && 
                                       topic_len > 14 && 
                                       strcmp(topic + topic_len - 5, "/logs") == 0);

                        if (is_log)
                        {
                            TRACE_DEBUG("PUBLISH topic='%s', payload='%s' (QoS %u, len %zu)\r\n", topic, payload, qos, payload_len);
                        }
                        else
                        {
                            TRACE_INFO("PUBLISH topic='%s', payload='%s' (QoS %u, len %zu)\r\n", topic, payload, qos, payload_len);
                        }

                        bool is_settings_request = false;
                        char mac[32] = {0};
                        if (strncmp(topic, "toniebox/", 9) == 0 && topic_len > 26)
                        {
                            const char *suffix = "/settings/request";
                            size_t suffix_len = strlen(suffix);
                            if (strcmp(topic + topic_len - suffix_len, suffix) == 0)
                            {
                                size_t mac_len = topic_len - 9 - suffix_len;
                                if (mac_len < sizeof(mac))
                                {
                                    memcpy(mac, topic + 9, mac_len);
                                    mac[mac_len] = '\0';
                                    is_settings_request = true;
                                }
                            }
                        }

                        if (is_settings_request)
                        {
                            char response_topic[128];
                            TRACE_INFO("Settings request from mac=%s\r\n", mac);
                            osSnprintf(response_topic, sizeof(response_topic), "toniebox/%s/settings/desired", mac);
                            mqtt_server_publish(response_topic, settings_json);
                        }

                        if (qos == 1)
                        {
                            uint8_t puback[] = {0x40, 0x02, (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFF)};
                            size_t written = 0;
                            if (connection.tlsContext)
                                tlsWrite(connection.tlsContext, puback, sizeof(puback), &written, 0);
                            else
                                socketSend(connection.socket, puback, sizeof(puback), &written, 0);
                        }
                    }
                    else if (cmd == 0xC0) // PINGREQ
                    {
                        TRACE_INFO("PINGREQ received\r\n");
                        uint8_t pingresp[] = {0xD0, 0x00};
                        size_t written = 0;
                        if (connection.tlsContext)
                            tlsWrite(connection.tlsContext, pingresp, sizeof(pingresp), &written, 0);
                        else
                            socketSend(connection.socket, pingresp, sizeof(pingresp), &written, 0);
                    }
                    else if (cmd == 0x80) // SUBSCRIBE
                    {
                        TRACE_INFO("SUBSCRIBE received\r\n");
                        size_t p = pos;
                        // Packet Identifier
                        uint16_t packet_id = (pkt[p] << 8) | pkt[p + 1];
                        p += 2;

                        // Payload: List of Topic Filter/QoS pairs
                        while (p < packet_size)
                        {
                            size_t topic_len = (pkt[p] << 8) | pkt[p + 1];
                            p += 2;
                            char topic[256];
                            size_t copy_len = topic_len < sizeof(topic) - 1 ? topic_len : sizeof(topic) - 1;
                            memcpy(topic, &pkt[p], copy_len);
                            topic[copy_len] = '\0';
                            p += topic_len;
                            uint8_t qos = pkt[p++];
                            TRACE_INFO("  Topic='%s', QoS=%u\r\n", topic, qos);

                            // Check if this is the fresh-tonies topic subscription
                            bool is_fresh_tonies_sub = false;
                            char mac[32] = {0};
                            if (strncmp(topic, "toniebox/", 9) == 0 && topic_len > 22)
                            {
                                const char *suffix = "/fresh-tonies";
                                size_t suffix_len = strlen(suffix);
                                if (strcmp(topic + topic_len - suffix_len, suffix) == 0)
                                {
                                    size_t mac_len = topic_len - 9 - suffix_len;
                                    if (mac_len < sizeof(mac))
                                    {
                                        memcpy(mac, topic + 9, mac_len);
                                        mac[mac_len] = '\0';
                                        is_fresh_tonies_sub = true;
                                    }
                                }
                            }

                            if (is_fresh_tonies_sub)
                            {
                                settings_t *box_settings = get_settings_cn(mac);
                                if (box_settings != NULL && box_settings->internal.config_used)
                                {
                                    client_ctx_t temp_ctx;
                                    temp_ctx.settings = box_settings;
                                    temp_ctx.state = get_toniebox_state_id(box_settings->internal.overlayNumber);
                                    
                                    mqtt_server_publish_fresh_tonies(&temp_ctx);
                                }
                            }
                        }
                        
                        // Responding with SUBACK (0x90)
                        uint8_t suback[] = {0x90, 0x03, (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFF), 0x00};
                        size_t written = 0;
                        if (connection.tlsContext)
                            tlsWrite(connection.tlsContext, suback, sizeof(suback), &written, 0);
                        else
                            socketSend(connection.socket, suback, sizeof(suback), &written, 0);
                    }
                    else if (cmd == 0xE0) // DISCONNECT
                    {
                        TRACE_INFO("DISCONNECT received\r\n");
                        if (connection.tlsContext)
                            tlsFree(connection.tlsContext);
                        socketClose(connection.socket);
                        connection.active = false;
                        connection.tlsContext = NULL;
                        connection.socket = NULL;
                        connection.buffer_len = 0;
                        return; // Exit task to avoid further processing on this connection
                    }
                    else
                    {
                        TRACE_INFO("Unknown command 0x%02X received (%zu bytes total)\r\n", cmd_raw, packet_size);
                    }

                    processed_total += packet_size;
                }

                if (processed_total > 0)
                {
                    connection.buffer_len -= processed_total;
                    if (connection.buffer_len > 0)
                    {
                        memmove(connection.buffer, &connection.buffer[processed_total], connection.buffer_len);
                    }
                }
            }
            else if (error != ERROR_WOULD_BLOCK && error != NO_ERROR && error != ERROR_TIMEOUT)
            {
                TRACE_INFO("Connection closed (error: %s)\r\n", error2text(error));
                if (connection.tlsContext)
                {
                    tlsFree(connection.tlsContext);
                }
                socketClose(connection.socket);
                connection.active = false;
                connection.tlsContext = NULL;
                connection.socket = NULL;
                connection.buffer_len = 0;
            }
        }
    }
    else
    {
        // Non-blocking check for new connections
        if (tcpWaitForEvents(serverSocket, SOCKET_EVENT_ACCEPT, 0) & SOCKET_EVENT_ACCEPT)
        {
            IpAddr clientIpAddr;
            uint16_t clientPort;
            Socket *clientSocket = socketAccept(serverSocket, &clientIpAddr, &clientPort);
            if (clientSocket != NULL)
            {
                TRACE_INFO("Accepted connection from %s:%u\r\n", ipAddrToString(&clientIpAddr, NULL), clientPort);

                connection.socket = clientSocket;
                connection.active = true;
                connection.buffer_len = 0;
                socketSetTimeout(connection.socket, 0);

                connection.tlsContext = tlsInit();
                if (connection.tlsContext != NULL)
                {
                    if (mqtt_server_tls_init(connection.tlsContext) == NO_ERROR)
                    {
                        tlsSetSocket(connection.tlsContext, connection.socket);
                    }
                    else
                    {
                        TRACE_ERROR("TLS init failed\r\n");
                        tlsFree(connection.tlsContext);
                        connection.tlsContext = NULL;
                    }
                }
            }
        }
    }
}

void mqtt_server_deinit() {
    if (serverSocket != NULL) {
        socketClose(serverSocket);
        serverSocket = NULL;
    }
    if (connection.active)
    {
        if (connection.tlsContext)
            tlsFree(connection.tlsContext);
        socketClose(connection.socket);
        connection.active = false;
    }

    if (mqtt_server_cert)
    {
        osFreeMem(mqtt_server_cert);
        mqtt_server_cert = NULL;
    }
    if (mqtt_server_key)
    {
        osFreeMem(mqtt_server_key);
        mqtt_server_key = NULL;
    }
}

void mqtt_server_publish_fresh_tonies(client_ctx_t *client_ctx)
{
    if (client_ctx == NULL || client_ctx->state == NULL || client_ctx->settings == NULL)
    {
        return;
    }

    if (!client_ctx->settings->internal.freshnessCacheChanged)
    {
        return;
    }

    settings_set_bool_id("internal.freshnessCacheChanged", false, client_ctx->settings->internal.overlayNumber);

    size_t freshnessCacheLen = 0;
    uint64_t *freshnessCache = settings_get_u64_array_id("internal.freshnessCache", client_ctx->settings->internal.overlayNumber, &freshnessCacheLen);

    for (size_t i = 0; i < freshnessCacheLen; i++)
    {
        char ruidStr[17];
        char uidStr[17];
        osSnprintf(uidStr, sizeof(uidStr), "%016" PRIX64, freshnessCache[i]);
        for (int j = 0; j < 8; j++)
        {
            ruidStr[j * 2] = uidStr[14 - j * 2];
            ruidStr[j * 2 + 1] = uidStr[15 - j * 2];
        }
        ruidStr[16] = '\0';

        cJSON *obj = cJSON_CreateObject();
        if (obj != NULL)
        {
            cJSON_AddStringToObject(obj, "tonie", ruidStr);
            char *payload = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);

            if (payload != NULL)
            {
                char topic[128];
                osSnprintf(topic, sizeof(topic), "toniebox/%s/fresh-tonies", client_ctx->state->box.id);
                mqtt_server_publish(topic, payload);
                osFreeMem(payload);
            }
        }
    }
}

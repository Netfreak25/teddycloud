#include <string.h>

#include "cJSON.h"
#include "mqtt_forward_filter.h"
#include "settings.h"

typedef struct
{
    const char *source;
    const char *setting;
} mqtt_log_source_filter_t;

static const mqtt_log_source_filter_t log_source_filters[] = {
    {"dev_i2s_mcux", "mqtt_client_upstream.forward.logs.source.dev_i2s_mcux"},
    {"power_domain_gpio", "mqtt_client_upstream.forward.logs.source.power_domain_gpio"},
    {"usdhc", "mqtt_client_upstream.forward.logs.source.usdhc"},
    {"iw416_wifi", "mqtt_client_upstream.forward.logs.source.iw416_wifi"},
    {"led_ring", "mqtt_client_upstream.forward.logs.source.led_ring"},
    {"sd", "mqtt_client_upstream.forward.logs.source.sd"},
    {"usb_msc", "mqtt_client_upstream.forward.logs.source.usb_msc"},
    {"tns_error_handler", "mqtt_client_upstream.forward.logs.source.tns_error_handler"},
    {"tns_dfu", "mqtt_client_upstream.forward.logs.source.tns_dfu"},
    {"littlefs", "mqtt_client_upstream.forward.logs.source.littlefs"},
    {"tns_fs_storage", "mqtt_client_upstream.forward.logs.source.tns_fs_storage"},
    {"tns_signal_broker", "mqtt_client_upstream.forward.logs.source.tns_signal_broker"},
    {"production", "mqtt_client_upstream.forward.logs.source.production"},
    {"LIS2DW12", "mqtt_client_upstream.forward.logs.source.LIS2DW12"},
    {"tns_usb_audio", "mqtt_client_upstream.forward.logs.source.tns_usb_audio"},
    {"app", "mqtt_client_upstream.forward.logs.source.app"},
    {"iw416", "mqtt_client_upstream.forward.logs.source.iw416"},
    {"main_sm", "mqtt_client_upstream.forward.logs.source.main_sm"},
    {"tns_pman", "mqtt_client_upstream.forward.logs.source.tns_pman"},
    {"idle_timer_log", "mqtt_client_upstream.forward.logs.source.idle_timer_log"},
    {"app_volume_manager", "mqtt_client_upstream.forward.logs.source.app_volume_manager"},
    {"ocotp", "mqtt_client_upstream.forward.logs.source.ocotp"},
    {"wifi_nxp", "mqtt_client_upstream.forward.logs.source.wifi_nxp"},
    {"tns_cloud", "mqtt_client_upstream.forward.logs.source.tns_cloud"},
    {"tns_storage", "mqtt_client_upstream.forward.logs.source.tns_storage"},
    {"tns_wifi_conn", "mqtt_client_upstream.forward.logs.source.tns_wifi_conn"},
    {"headphones", "mqtt_client_upstream.forward.logs.source.headphones"},
    {"tns_metrics", "mqtt_client_upstream.forward.logs.source.tns_metrics"},
    {"bt_nxp_ctlr", "mqtt_client_upstream.forward.logs.source.bt_nxp_ctlr"},
    {"bt_hci_core", "mqtt_client_upstream.forward.logs.source.bt_hci_core"},
    {"bt_classic", "mqtt_client_upstream.forward.logs.source.bt_classic"},
    {"tns_wifi_settings", "mqtt_client_upstream.forward.logs.source.tns_wifi_settings"},
    {"tns_time", "mqtt_client_upstream.forward.logs.source.tns_time"},
    {"tns_ota", "mqtt_client_upstream.forward.logs.source.tns_ota"},
    {"cloud_settings", "mqtt_client_upstream.forward.logs.source.cloud_settings"},
    {"cloud_freshness", "mqtt_client_upstream.forward.logs.source.cloud_freshness"},
    {"tns_download_manager", "mqtt_client_upstream.forward.logs.source.tns_download_manager"},
    {"linear_playback", "mqtt_client_upstream.forward.logs.source.linear_playback"},
};

static bool_t mqtt_filter_effective(settings_t *box_settings, const char *setting)
{
    setting_item_t *global = settings_get_by_name_ovl(setting, NULL);
    if (global == NULL || global->type != TYPE_BOOL)
    {
        return TRUE;
    }

    if (box_settings != NULL && box_settings->internal.overlayNumber != 0)
    {
        setting_item_t *overlay = settings_get_by_name_ovl(
            setting, box_settings->internal.overlayUniqueId);
        if (overlay != NULL && overlay->type == TYPE_BOOL && overlay->overlayed)
        {
            return *((bool *)overlay->ptr) ? TRUE : FALSE;
        }
    }
    return *((bool *)global->ptr) ? TRUE : FALSE;
}

static const char *mqtt_topic_path(const char *topic)
{
    static const char prefix[] = "toniebox/";
    if (topic == NULL || strncmp(topic, prefix, sizeof(prefix) - 1) != 0)
    {
        return NULL;
    }
    const char *separator = strchr(topic + sizeof(prefix) - 1, '/');
    return separator != NULL && separator[1] != '\0' ? separator + 1 : NULL;
}

static bool_t mqtt_path_is_tree(const char *path, const char *root)
{
    const size_t length = strlen(root);
    return path != NULL && strncmp(path, root, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static const char *mqtt_group_child(const char *path, const char *group,
                                    char *child, size_t child_size)
{
    const size_t group_length = strlen(group);
    if (path == NULL || strncmp(path, group, group_length) != 0 ||
        (path[group_length] != '\0' && path[group_length] != '/'))
    {
        return NULL;
    }
    if (path[group_length] == '\0')
    {
        child[0] = '\0';
        return child;
    }

    const char *start = path + group_length + 1;
    const char *end = strchr(start, '/');
    const size_t length = end != NULL ? (size_t)(end - start) : strlen(start);
    if (length >= child_size)
    {
        child[0] = '\0';
        return child;
    }
    memcpy(child, start, length);
    child[length] = '\0';
    return child;
}

static const char *mqtt_classify_logs(const uint8_t *payload, size_t payload_len)
{
    const char *setting = "mqtt_client_upstream.forward.logs.other";
    cJSON *json = payload != NULL ?
                      cJSON_ParseWithLength((const char *)payload, payload_len) : NULL;
    cJSON *source = json != NULL ? cJSON_GetObjectItemCaseSensitive(json, "source") : NULL;
    if (cJSON_IsString(source) && source->valuestring != NULL)
    {
        for (size_t i = 0; i < sizeof(log_source_filters) / sizeof(log_source_filters[0]); i++)
        {
            if (strcmp(source->valuestring, log_source_filters[i].source) == 0)
            {
                setting = log_source_filters[i].setting;
                break;
            }
        }
    }
    cJSON_Delete(json);
    return setting;
}

static const char *mqtt_classify_group(const char *path, const char *group,
                                       const char *const *children,
                                       const char *const *settings_by_child,
                                       size_t count, const char *other_setting)
{
    char child[64];
    if (mqtt_group_child(path, group, child, sizeof(child)) == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < count; i++)
    {
        if (strcmp(child, children[i]) == 0)
        {
            return settings_by_child[i];
        }
    }
    return other_setting;
}

static const char *mqtt_classify_publish(const char *topic,
                                          const uint8_t *payload, size_t payload_len)
{
    static const char *metrics_children[] = {"fleet", "events", "headphones", "battery"};
    static const char *metrics_settings[] = {
        "mqtt_client_upstream.forward.metrics.fleet",
        "mqtt_client_upstream.forward.metrics.events",
        "mqtt_client_upstream.forward.metrics.headphones",
        "mqtt_client_upstream.forward.metrics.battery",
    };
    static const char *app_reply_children[] = {"bedtime-state"};
    static const char *app_reply_settings[] = {"mqtt_client_upstream.forward.app_reply.bedtime_state"};
    static const char *settings_children[] = {"desired", "confirm", "request"};
    static const char *settings_settings[] = {
        "mqtt_client_upstream.forward.settings.desired",
        "mqtt_client_upstream.forward.settings.confirm",
        "mqtt_client_upstream.forward.settings.request",
    };
    static const char *playback_children[] = {"state"};
    static const char *playback_settings[] = {"mqtt_client_upstream.forward.playback.state"};
    static const char *app_control_children[] = {
        "ping", "volume", "stl", "alarm-preview", "playback", "sleep",
    };
    static const char *app_control_settings[] = {
        "mqtt_client_upstream.forward.app_control.ping",
        "mqtt_client_upstream.forward.app_control.volume",
        "mqtt_client_upstream.forward.app_control.stl",
        "mqtt_client_upstream.forward.app_control.alarm_preview",
        "mqtt_client_upstream.forward.app_control.playback",
        "mqtt_client_upstream.forward.app_control.sleep",
    };

    const char *path = mqtt_topic_path(topic);
    if (path == NULL)
    {
        return "mqtt_client_upstream.forward.other";
    }
    if (mqtt_path_is_tree(path, "claim"))
        return "mqtt_client_upstream.forward.claim";
    if (mqtt_path_is_tree(path, "volume"))
        return "mqtt_client_upstream.forward.volume";
    if (mqtt_path_is_tree(path, "bi-events"))
        return "mqtt_client_upstream.forward.bi_events";
    if (mqtt_path_is_tree(path, "fresh-tonies"))
        return "mqtt_client_upstream.forward.fresh_tonies";
    if (mqtt_path_is_tree(path, "logs"))
        return mqtt_classify_logs(payload, payload_len);
    if (mqtt_path_is_tree(path, "setup"))
        return "mqtt_client_upstream.forward.setup";

    const char *setting = mqtt_classify_group(path, "metrics", metrics_children, metrics_settings, 4,
                                               "mqtt_client_upstream.forward.metrics.other");
    if (setting != NULL)
        return setting;
    setting = mqtt_classify_group(path, "app-reply", app_reply_children, app_reply_settings, 1,
                                   "mqtt_client_upstream.forward.app_reply.other");
    if (setting != NULL)
        return setting;
    setting = mqtt_classify_group(path, "settings", settings_children, settings_settings, 3,
                                   "mqtt_client_upstream.forward.settings.other");
    if (setting != NULL)
        return setting;
    setting = mqtt_classify_group(path, "playback", playback_children, playback_settings, 1,
                                   "mqtt_client_upstream.forward.playback.other");
    if (setting != NULL)
        return setting;
    setting = mqtt_classify_group(path, "app-control", app_control_children, app_control_settings, 6,
                                   "mqtt_client_upstream.forward.app_control.other");
    return setting != NULL ? setting : "mqtt_client_upstream.forward.other";
}

mqtt_forward_filter_result_t mqtt_forward_filter_evaluate(
    settings_t *box_settings, mqtt_forward_route_t route, const char *topic,
    const uint8_t *payload, size_t payload_len)
{
    mqtt_forward_filter_result_t result = {
        .route = route,
        .action = MQTT_FORWARD_ACTION_LOCAL,
        .setting_id = NULL,
        .reason = MQTT_FORWARD_REASON_LOCAL,
    };
    if (route == MQTT_FORWARD_ROUTE_LOCAL_TO_BOX)
        return result;

    // Keep the matched rule visible even when the global manual master bypasses it.
    result.setting_id = mqtt_classify_publish(topic, payload, payload_len);
    if (!settings_get_bool("mqtt_client_upstream.filters_enabled"))
    {
        result.action = MQTT_FORWARD_ACTION_FORWARD;
        result.reason = MQTT_FORWARD_REASON_MASTER_BYPASS;
        return result;
    }

    bool_t allowed = mqtt_filter_effective(box_settings, result.setting_id);
    result.action = allowed ? MQTT_FORWARD_ACTION_FORWARD : MQTT_FORWARD_ACTION_BLOCK;
    result.reason = allowed ? MQTT_FORWARD_REASON_RULE_ALLOWED : MQTT_FORWARD_REASON_RULE_BLOCKED;
    return result;
}

const char *mqtt_forward_route_name(mqtt_forward_route_t route)
{
    switch (route)
    {
    case MQTT_FORWARD_ROUTE_BOX_TO_TONIES:
        return "box_to_tonies";
    case MQTT_FORWARD_ROUTE_TONIES_TO_BOX:
        return "tonies_to_box";
    case MQTT_FORWARD_ROUTE_LOCAL_TO_BOX:
        return "local_to_box";
    default:
        return "unknown";
    }
}

const char *mqtt_forward_reason_name(mqtt_forward_reason_t reason)
{
    switch (reason)
    {
    case MQTT_FORWARD_REASON_RULE_ALLOWED:
        return "rule_allowed";
    case MQTT_FORWARD_REASON_RULE_BLOCKED:
        return "rule_blocked";
    case MQTT_FORWARD_REASON_MASTER_BYPASS:
        return "master_bypass";
    case MQTT_FORWARD_REASON_LOCAL:
        return "local";
    case MQTT_FORWARD_REASON_NOT_EVALUATED_LOCAL_CONSUME:
        return "not_evaluated_local_consume";
    case MQTT_FORWARD_REASON_NOT_EVALUATED_AUTOMATIC_BLOCK:
        return "not_evaluated_automatic_block";
    default:
        return "unknown";
    }
}

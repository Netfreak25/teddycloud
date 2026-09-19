#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_forward_filter.h"

static bool master_enabled = true;
static bool global_forward = true;
static bool global_other = false;
static bool box_forward = true;
static unsigned settings_reads = 0;
static setting_item_t global_rule = {.type = TYPE_BOOL, .ptr = &global_forward};
static setting_item_t other_rule = {.type = TYPE_BOOL, .ptr = &global_other};
static setting_item_t box_rule = {.type = TYPE_BOOL, .ptr = &box_forward, .overlayed = true};

bool settings_get_bool(const char *item)
{
    assert(strcmp(item, "mqtt_client_upstream.filters_enabled") == 0);
    settings_reads++;
    return master_enabled;
}

setting_item_t *settings_get_by_name_ovl(const char *item, const char *overlay)
{
    assert(strncmp(item, "mqtt_client_upstream.forward.", 29) == 0);
    settings_reads++;
    if (overlay != NULL)
        return &box_rule;
    return strcmp(item, "mqtt_client_upstream.forward.other") == 0 ? &other_rule : &global_rule;
}

static void check_decision(settings_t *box, mqtt_forward_route_t route,
                           const char *topic, const char *payload,
                           const char *setting_suffix,
                           mqtt_forward_action_t action, mqtt_forward_reason_t reason)
{
    mqtt_forward_filter_result_t result = mqtt_forward_filter_evaluate(
        box, route, topic, (const uint8_t *)payload, payload != NULL ? strlen(payload) : 0);
    assert(result.route == route);
    assert(result.action == action);
    assert(result.reason == reason);
    if (setting_suffix == NULL)
    {
        assert(result.setting_id == NULL);
        return;
    }
    assert(result.setting_id != NULL);
    assert(strncmp(result.setting_id, "mqtt_client_upstream.forward.", 29) == 0);
    assert(strcmp(result.setting_id + 29, setting_suffix) == 0);
}

typedef struct
{
    const char *path;
    const char *setting;
} topic_case_t;

static const topic_case_t topic_cases[] = {
    {"claim/8194F21C500304E0", "claim"},
    {"volume/state", "volume"},
    {"bi-events", "bi_events"},
    {"fresh-tonies", "fresh_tonies"},
    {"metrics/fleet", "metrics.fleet"},
    {"metrics/events", "metrics.events"},
    {"metrics/headphones", "metrics.headphones"},
    {"metrics/battery", "metrics.battery"},
    {"metrics/new", "metrics.other"},
    {"app-reply/bedtime-state", "app_reply.bedtime_state"},
    {"app-reply/pong", "app_reply.other"},
    {"settings/desired", "settings.desired"},
    {"settings/confirm", "settings.confirm"},
    {"settings/request", "settings.request"},
    {"settings/new", "settings.other"},
    {"playback/state", "playback.state"},
    {"playback/new", "playback.other"},
    {"app-control/ping", "app_control.ping"},
    {"app-control/volume", "app_control.volume"},
    {"app-control/stl", "app_control.stl"},
    {"app-control/alarm-preview", "app_control.alarm_preview"},
    {"app-control/playback", "app_control.playback"},
    {"app-control/sleep", "app_control.sleep"},
    {"app-control/new", "app_control.other"},
    {"setup/status", "setup"},
    {"new-topic", "other"},
    // Preserve tree boundaries and the existing per-group Other rules.
    {"claiming", "other"},
    {"volume-extra/state", "other"},
    {"logs-extra", "other"},
    {"setup-extra/status", "other"},
    {"settings-extra/desired", "other"},
    {"metrics", "metrics.other"},
    {"metrics/fleet/subtopic", "metrics.fleet"},
    {"settings/desired-extra", "settings.other"},
};

static const char *log_sources[] = {
    "dev_i2s_mcux", "power_domain_gpio", "usdhc", "iw416_wifi",
    "led_ring", "sd", "usb_msc", "tns_error_handler", "tns_dfu",
    "littlefs", "tns_fs_storage", "tns_signal_broker", "production",
    "LIS2DW12", "tns_usb_audio", "app", "iw416", "main_sm", "tns_pman",
    "idle_timer_log", "app_volume_manager", "ocotp", "wifi_nxp", "tns_cloud",
    "tns_storage", "tns_wifi_conn", "headphones", "tns_metrics", "bt_nxp_ctlr",
    "bt_hci_core", "bt_classic", "tns_wifi_settings", "tns_time", "tns_ota",
    "cloud_settings", "cloud_freshness", "tns_download_manager", "linear_playback",
};

int main(void)
{
    settings_t box = {0};
    box.internal.overlayNumber = 1;
    box.internal.overlayUniqueId = "TEST-BOX";
    const mqtt_forward_route_t routes[] = {
        MQTT_FORWARD_ROUTE_BOX_TO_TONIES,
        MQTT_FORWARD_ROUTE_TONIES_TO_BOX,
    };

    for (size_t route_index = 0; route_index < sizeof(routes) / sizeof(routes[0]); route_index++)
    {
        mqtt_forward_route_t route = routes[route_index];
        for (size_t index = 0; index < sizeof(topic_cases) / sizeof(topic_cases[0]); index++)
        {
            char topic[160];
            snprintf(topic, sizeof(topic), "toniebox/123456789ABC/%s", topic_cases[index].path);
            bool other = strcmp(topic_cases[index].setting, "other") == 0;
            check_decision(NULL, route, topic, "{}", topic_cases[index].setting,
                           other ? MQTT_FORWARD_ACTION_BLOCK : MQTT_FORWARD_ACTION_FORWARD,
                           other ? MQTT_FORWARD_REASON_RULE_BLOCKED : MQTT_FORWARD_REASON_RULE_ALLOWED);
        }

        for (size_t index = 0; index < sizeof(log_sources) / sizeof(log_sources[0]); index++)
        {
            char payload[96];
            char setting[80];
            snprintf(payload, sizeof(payload), "{\"source\":\"%s\"}", log_sources[index]);
            snprintf(setting, sizeof(setting), "logs.source.%s", log_sources[index]);
            check_decision(NULL, route, "toniebox/123456789ABC/logs", payload, setting,
                           MQTT_FORWARD_ACTION_FORWARD, MQTT_FORWARD_REASON_RULE_ALLOWED);
        }

        const char *other_logs[] = {
            "{}", "not-json", "{\"source\":false}", "{\"source\":\"APP\"}",
            "{\"source\":\"new-source\"}", "[{\"source\":\"app\"}]",
        };
        for (size_t index = 0; index < sizeof(other_logs) / sizeof(other_logs[0]); index++)
            check_decision(NULL, route, "toniebox/123456789ABC/logs", other_logs[index], "logs.other",
                           MQTT_FORWARD_ACTION_FORWARD, MQTT_FORWARD_REASON_RULE_ALLOWED);

        check_decision(NULL, route, "outside/toniebox", "{}", "other",
                       MQTT_FORWARD_ACTION_BLOCK, MQTT_FORWARD_REASON_RULE_BLOCKED);
        check_decision(NULL, route, "toniebox/123456789ABC", "{}", "other",
                       MQTT_FORWARD_ACTION_BLOCK, MQTT_FORWARD_REASON_RULE_BLOCKED);

        const char *topic = "toniebox/123456789ABC/playback/state";
        global_forward = false;
        check_decision(NULL, route, topic, "{}", "playback.state",
                       MQTT_FORWARD_ACTION_BLOCK, MQTT_FORWARD_REASON_RULE_BLOCKED);
        check_decision(&box, route, topic, "{}", "playback.state",
                       MQTT_FORWARD_ACTION_FORWARD, MQTT_FORWARD_REASON_RULE_ALLOWED);
        check_decision(&box, route, "outside/toniebox", "{}", "other",
                       MQTT_FORWARD_ACTION_FORWARD, MQTT_FORWARD_REASON_RULE_ALLOWED);

        box_forward = false;
        check_decision(&box, route, topic, "{}", "playback.state",
                       MQTT_FORWARD_ACTION_BLOCK, MQTT_FORWARD_REASON_RULE_BLOCKED);
        master_enabled = false;
        check_decision(&box, route, topic, "{}", "playback.state",
                       MQTT_FORWARD_ACTION_FORWARD, MQTT_FORWARD_REASON_MASTER_BYPASS);
        check_decision(NULL, route, "outside/toniebox", "{}", "other",
                       MQTT_FORWARD_ACTION_FORWARD, MQTT_FORWARD_REASON_MASTER_BYPASS);
        assert(!global_forward && !global_other && !box_forward && box_rule.overlayed);

        master_enabled = true;
        check_decision(&box, route, topic, "{}", "playback.state",
                       MQTT_FORWARD_ACTION_BLOCK, MQTT_FORWARD_REASON_RULE_BLOCKED);
        box_rule.overlayed = false;
        global_forward = true;
        check_decision(&box, route, topic, "{}", "playback.state",
                       MQTT_FORWARD_ACTION_FORWARD, MQTT_FORWARD_REASON_RULE_ALLOWED);
        check_decision(&box, route, "outside/toniebox", "{}", "other",
                       MQTT_FORWARD_ACTION_BLOCK, MQTT_FORWARD_REASON_RULE_BLOCKED);
        box_rule.overlayed = true;
        box_forward = true;
    }

    // Trusted local delivery never reads manual settings or parses a payload.
    unsigned previous_reads = settings_reads;
    global_forward = false;
    box_forward = false;
    check_decision(&box, MQTT_FORWARD_ROUTE_LOCAL_TO_BOX, NULL, NULL, NULL,
                   MQTT_FORWARD_ACTION_LOCAL, MQTT_FORWARD_REASON_LOCAL);
    assert(settings_reads == previous_reads);
    return 0;
}

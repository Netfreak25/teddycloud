#include <assert.h>
#include <string.h>

#include "mqtt_forward_filter.h"

static bool master_enabled = true;
static bool global_forward = false;
static bool box_forward = true;
static setting_item_t global_rule = {.type = TYPE_BOOL, .ptr = &global_forward};
static setting_item_t box_rule = {.type = TYPE_BOOL, .ptr = &box_forward, .overlayed = true};

bool settings_get_bool(const char *item)
{
    assert(strcmp(item, "mqtt_client_upstream.filters_enabled") == 0);
    return master_enabled;
}

setting_item_t *settings_get_by_name_ovl(const char *item, const char *overlay)
{
    assert(strncmp(item, "mqtt_client_upstream.forward.", 29) == 0);
    return overlay != NULL ? &box_rule : &global_rule;
}

int main(void)
{
    settings_t box = {0};
    box.internal.overlayNumber = 1;
    box.internal.overlayUniqueId = "TEST-BOX";
    const char *topic = "toniebox/123456789ABC/playback/state";
    const char *filter_id = NULL;
    const uint8_t payload[] = "{}";

    assert(mqtt_forward_filter_should_block(NULL, topic, payload, 2, &filter_id));
    assert(filter_id != NULL);
    assert(!mqtt_forward_filter_should_block(&box, topic, payload, 2, &filter_id));

    box_forward = false;
    assert(mqtt_forward_filter_should_block(&box, topic, payload, 2, &filter_id));
    master_enabled = false;
    assert(!mqtt_forward_filter_should_block(NULL, topic, payload, 2, &filter_id));
    assert(filter_id == NULL);
    assert(!mqtt_forward_filter_should_block(&box, topic, payload, 2, &filter_id));
    assert(!global_forward && !box_forward && box_rule.overlayed);

    // Existing connection/settings object immediately uses the restored rules.
    master_enabled = true;
    assert(mqtt_forward_filter_should_block(&box, topic, payload, 2, &filter_id));
    box_rule.overlayed = false;
    global_forward = true;
    assert(!mqtt_forward_filter_should_block(&box, topic, payload, 2, &filter_id));
    return 0;
}

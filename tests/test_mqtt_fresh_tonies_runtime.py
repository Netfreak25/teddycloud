#!/usr/bin/env python3
"""Exercise the production freshness delivery state machine with controlled I/O.

Only the clock, publish boundary and settings storage are stubbed. Production
queue/sender/pump/acknowledgement bodies are compiled verbatim, like the existing
MQTT server runtime tests. Run using Python and gcc on Linux/WSL.
"""
from pathlib import Path
import re
import subprocess
import tempfile


def check_cache_replacement(root, directory):
    """Prove the production array setter keeps pending state on allocation failure."""
    source = (root / "src/settings.c").read_text(encoding="utf-8")
    setter = re.search(
        r"^bool settings_set_u64_array_id\([^;]*?\n\{[\s\S]*?\n\}", source, re.M
    ).group()
    fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
enum { TYPE_U64_ARRAY };
typedef struct { int type; void *ptr; size_t size; bool overlayed, internal; } setting_item_t;
static uint64_t *values;
static setting_item_t option = {TYPE_U64_ARRAY, &values, 0, false, true};
static bool fail_alloc;
static void *osAllocMem(size_t size) { return fail_alloc ? NULL : malloc(size); }
#define osFreeMem free
#define osMemcpy memcpy
static setting_item_t *settings_get_by_name_id(const char *name, uint8_t id)
{ assert(!strcmp(name, "internal.freshnessCache") && id == 1); return &option; }
static void settings_changed_id(uint8_t id) { (void)id; assert(false); }
/* SETTER */
int main(void) {
    uint64_t first[] = {1, 2}, next[] = {9};
    assert(settings_set_u64_array_id("internal.freshnessCache", first, 2, 1));
    uint64_t *original = values;
    fail_alloc = true;
    assert(!settings_set_u64_array_id("internal.freshnessCache", next, 1, 1));
    assert(values == original && option.size == 2 && values[0] == 1 && values[1] == 2);
    fail_alloc = false;
    assert(settings_set_u64_array_id("internal.freshnessCache", values + 1, 1, 1));
    assert(values[0] == 2 && option.size == 1 && option.overlayed);
    assert(settings_set_u64_array_id("internal.freshnessCache", NULL, 0, 1));
    assert(values == NULL && option.size == 0);
    puts("Freshness cache replacement PASS: failed allocation retained, aliased input copied, clear");
    return 0;
}
'''.replace("/* SETTER */", setter)
    generated = Path(directory) / "cache_replace.c"
    executable = Path(directory) / "cache_replace"
    generated.write_text(fixture, encoding="utf-8")
    subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", str(generated), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/mqtt_server.c").read_text(encoding="utf-8")
    names = (
        "mqtt_connection_debug_owner", "mqtt_server_debug_hass_duration",
        "mqtt_uid_to_ruid", "mqtt_debug_fresh_event",
        "mqtt_fresh_tonies_reset_connection",
        "mqtt_fresh_tonies_publish_state", "mqtt_debug_fresh_generation", "mqtt_mark_fresh_tonies_pending",
        "mqtt_clear_fresh_tonies_pending", "mqtt_fresh_tonie_find",
        "mqtt_fresh_tonies_sync", "mqtt_fresh_tonies_next",
        "mqtt_fresh_tonies_cache_contains", "mqtt_build_fresh_tonie_payload",
        "mqtt_server_freshness_init", "mqtt_server_freshness_forget_overlay", "mqtt_server_freshness_begin_reload",
        "mqtt_server_freshness_reconcile_overlays", "mqtt_server_freshness_begin",
        "mqtt_server_freshness_cache", "mqtt_freshness_snapshot_generation",
        "mqtt_freshness_uid_contains", "mqtt_server_freshness_prepare", "mqtt_server_freshness_finish",
        "mqtt_local_fresh_completed", "mqtt_send_fresh_tonie", "mqtt_fresh_tonies_select", "mqtt_fresh_tonies_pump",
        "mqtt_handle_fresh_tonies_puback", "mqtt_server_publish_fresh_tonies",
        "mqtt_server_publish_fresh_tonies_for_overlay", "mqtt_server_publish_fresh_tonie_for_overlay",
        "mqtt_server_publish_fresh_tonie_for_overlay_locked",
    )
    functions = []
    for name in names:
        match = re.search(
            r"^(?:static )?[\w *]+\b" + name + r"\([^;]*?\n\{[\s\S]*?\n\}",
            source, re.M,
        )
        if match is None:
            raise AssertionError(f"Freshness function missing: {name}")
        functions.append(match.group())
    start = source.index("typedef struct {\n    char topic[256];")
    end = source.index("} MqttFreshToniesPublishState;", start)
    end += len("} MqttFreshToniesPublishState;")
    declarations = "\n".join(re.findall(
        r"^#define MQTT_(?:MAX_(?:PACKET_SIZE|CONNECTIONS|SUBSCRIPTIONS)|"
        r"FRESH_TONIES_\w+|MILLISECONDS_PER_SECOND|DEBUG_SLOW(?:_REPORT)?_MS) .+$", source, re.M,
    )) + "\n" + source[start:end]
    declarations += "\n" + re.search(
        r"typedef struct \{\n    MqttClientConnection \*conn;\n"
        r"    tb2_mqtt_passthrough_session_t \*session;\n    uint64_t uid;[\s\S]*?\n\} MqttFreshDelivery;",
        source).group()
    declarations += "\n" + re.search(r"struct mqtt_freshness_snapshot \{[\s\S]*?\n\};", source).group()
    fixture = (root / "tests/test_mqtt_fresh_tonies_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* SERVER_TYPES */", declarations)
    fixture = fixture.replace("/* SERVER_FUNCTIONS */", "\n\n".join(functions))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-fresh-delivery-") as directory:
        generated = Path(directory) / "fresh_delivery.c"
        executable = Path(directory) / "fresh_delivery"
        generated.write_text(fixture, encoding="utf-8")
        subprocess.run([
            "gcc", "-Wall", "-Wextra", "-Werror", "-DGPL_LICENSE_TERMS_ACCEPTED",
            "-DHTTP_SERVER_MAX_CONNECTIONS=32", "-DTRACE_NOPATH_FILE",
            *["-I" + path for path in includes], str(generated),
            "tests/mqtt_debug_stubs.c", "cJSON/cJSON.c", "-o", str(executable),
        ], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)
        check_cache_replacement(root, directory)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Exercise production bool setter/reset/save against a controlled option map."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def definition(source, signature):
    start = source.index(signature + "\n{")
    end = source.index("\n}", start) + 2
    return source[start:end]


def main():
    source = (ROOT / "src/settings.c").read_text(encoding="utf-8")
    option = next(line for line in source.splitlines()
                  if 'OPTION_BOOL("cloud.cacheOtaV3BothSlots"' in line)
    defines = "\n".join(re.findall(r'^#define [A-Z_0-9]+ "[^"\n]*"$', source, re.M))
    code = r'''
#define TRACE_LEVEL 0
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include "settings.h"
#include "fs_ext.h"
#include "debug.h"
static settings_t Settings_Overlay[MAX_OVERLAYS];
static setting_item_t options[2];
static setting_item_t *Option_Map_Overlay[MAX_OVERLAYS];
static char *config_file_path = "config.ini", *config_overlay_file_path = "overlay.ini";
static unsigned changed, notified;
static bool enabled;
settings_t *get_settings(void) { return &Settings_Overlay[0]; }
static setting_item_t *settings_get_by_name_id(const char *name, uint8_t id)
{ (void)id; return !strcmp(name, "cloud.cacheOtaV3BothSlots") ? &options[0] : NULL; }
void settings_changed_id(uint8_t id) { assert(!id); changed++; }
static bool settings_is_tb2_https_mode(const char *name) { (void)name; return false; }
static void settings_select_tb2_https_mode(uint8_t id, const char *name) { (void)id; (void)name; assert(false); }
static void settings_reconcile_sni_generation_cycle(settings_t *s) { (void)s; assert(false); }
void tb2_ota_cache_set_enabled(bool value) { enabled = value; notified++; }
void overlay_settings_init_opt(setting_item_t *opt, setting_item_t *global)
{ (void)global; *(bool *)opt->ptr = opt->init.bool_value; }
char *custom_asprintf(const char *format, ...)
{ va_list args; va_start(args, format); char *result = NULL; int n = vasprintf(&result, format, args); va_end(args); return n < 0 ? NULL : result; }
error_t fsFlushFile(FsFile *file) { return fflush(file) ? ERROR_WRITE_FAILED : NO_ERROR; }
void osFreeMem(void *pointer) { free(pointer); }
'''
    code += defines + "\n"
    for signature in ("bool settings_set_bool_id(const char *item, bool value, uint8_t settingsId)",
                      "bool settings_reset_id(const char *item, uint8_t settingsId)",
                      "static error_t settings_save_ovl(bool overlay)"):
        code += definition(source, signature) + "\n"
    code += r'''
int main(void)
{
    settings_t *settings = get_settings();
    setting_item_t registered[] = {
'''
    code += option + "\n"
    code += r'''
    };
    options[0] = registered[0]; *(bool *)options[0].ptr = options[0].init.bool_value;
    options[1].type = TYPE_END;
    Option_Map_Overlay[0] = options; settings->internal.config_used = true;
    assert(!settings->cloud.cacheOtaV3BothSlots);
    assert(!settings_set_bool_id("cloud.cacheOtaV3BothSlots", true, 1));
    assert(!settings_set_bool_id("cloud.cacheOtaV3BothSlots", false, 1));
    assert(!options[0].overlayed && !changed && !notified);
    assert(settings_set_bool_id("cloud.cacheOtaV3BothSlots", true, 0));
    assert(enabled && changed == 1 && notified == 1);
    assert(!settings_save_ovl(false));
    FILE *file = fopen(config_file_path, "rb"); char line[128];
    assert(file && fgets(line, sizeof(line), file)); fclose(file);
    assert(!strcmp(line, "cloud.cacheOtaV3BothSlots=true\n"));
    assert(settings_reset_id("cloud.cacheOtaV3BothSlots", 0));
    assert(!enabled && !settings->cloud.cacheOtaV3BothSlots && notified == 2);
    assert(!settings_save_ovl(false));
    file = fopen(config_file_path, "rb"); assert(file && fgets(line, sizeof(line), file)); fclose(file);
    assert(!strcmp(line, "cloud.cacheOtaV3BothSlots=false\n"));
    puts("PASS production settings: default, box rejection, save, reset");
}
'''
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-ota-settings-") as directory:
        fixture = Path(directory) / "settings.c"
        fixture.write_text(code, encoding="utf-8")
        executable = Path(directory) / "settings"
        objects = []
        for index, unit in enumerate([str(fixture), "cyclone/common/fs_port_posix.c", "cyclone/common/date_time.c"]):
            output = Path(directory) / f"{index}.o"
            flags = ["-Wall", "-Wextra", "-Werror"] if index == 0 else ["-w"]
            subprocess.run(["gcc", *flags, "-ffunction-sections", "-DGPL_LICENSE_TERMS_ACCEPTED",
                            *["-I" + path for path in includes], "-c", unit, "-o", str(output)], cwd=ROOT, check=True)
            objects.append(str(output))
        subprocess.run(["gcc", *objects, "-Wl,--gc-sections", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], cwd=directory, check=True)


if __name__ == "__main__":
    main()

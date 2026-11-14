#!/usr/bin/env python3
"""Compile production original-content selection with controlled cache inputs."""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/handler_cloud.c").read_text(encoding="utf-8")
    names = ("freshness_original_selected_version", "freshness_get_natural_server_audio_id",
             "tonie_cloud_access_allowed", "v3_native_original_content_allowed",
             "v3_native_serve_cached_original", "freshness_response_add_uid",
             "freshness_get_effective_server_audio_id",
             "freshness_original_reconcile_locked", "original_content_cache_v3_complete",
             "freshness_original_finish_response", "freshness_confirm_v3_content_version")
    functions = []
    for name in names:
        match = re.search(r"^(?:static )?[\w *]+\b" + name +
                          r"\([^;]*?\n\{[\s\S]*?\n\}", source, re.M)
        assert match is not None, name
        functions.append(match.group())
    fixture = (root / "tests/test_v3_original_sync_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* ORIGINAL_SELECTION_FUNCTIONS */", "\n\n".join(functions))
    freshness = source[source.index("error_t handleCloudFreshnessCheckV3("):]
    builder_start = freshness.index("        cJSON *cloudReqJson = cJSON_CreateObject();")
    builder_end = freshness.index("        char *cloudRequest =", builder_start)
    fixture = fixture.replace("/* CLOUD_FRESHNESS_REQUEST_BUILDER */", """
static cJSON *build_cloud_freshness_request(TonieFreshnessCheckRequest freshReqCloud, cbr_ctx_t ctx)
{
""" + freshness[builder_start:builder_end] + "\n    assert(requestReady);\n    return cloudReqJson;\n}\n")
    route = source[source.index("error_t handleCloudContentMetaV3("):]
    cloud_check = re.search(r"    bool_t cache_stale =.*?;", route, re.S)
    assert cloud_check is not None
    fixture = fixture.replace("/* METADATA_ROUTE_CLOUD_CHECK */", """
static bool_t metadata_route_cloud_check(client_ctx_t *client_ctx,
    tonie_info_t *tonieInfo, const char *canonical_ruid)
{
    bool_t cache_ruid_valid = TRUE;
    bool_t source_configured = tonieInfo->json.source != NULL && tonieInfo->json.source[0] != '\\0';
    uint64_t cache_uid = 0;
""" + cloud_check.group() + "\n    return cache_stale;\n}\n")
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-v3-original-sync-") as directory:
        generated = Path(directory) / "selection.c"
        executable = Path(directory) / "selection"
        generated.write_text(fixture, encoding="utf-8")
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
            "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
            *["-I" + path for path in includes], str(generated), "src/tb2_ruid.c", "cJSON/cJSON.c",
            "-o", str(executable),
        ], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()

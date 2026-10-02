#!/usr/bin/env python3
"""Target-architecture FIFO regression using the production relay functions."""
from pathlib import Path
import re
import subprocess
import tempfile


FUNCTION = re.compile(r"^(?:static )?[\w *]+\b(\w+)\([^;]*?\n\{[\s\S]*?\n\}", re.M)


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/tb2_mqtt_passthrough.c").read_text()
    fixture = (root / "tests/test_tb2_mqtt_box_queue_runtime.c").read_text()
    production = {match[1]: match.group() for match in FUNCTION.finditer(source)}
    boundaries = set(re.findall(r"^(?:static )?[\w *]+\b(tb2_mqtt_\w+)\([^;]*?\)\s*\{", fixture, re.M))
    required = {"tb2_mqtt_passthrough_submit_local_batch", "tb2_mqtt_box_write_pump",
                "tb2_mqtt_box_writes_cancel", "tb2_mqtt_process_stream",
                "tb2_mqtt_passthrough_is_established", "tb2_mqtt_passthrough_task",
                "tb2_mqtt_box_write_append", "tb2_mqtt_debug_ack",
                "tb2_mqtt_passthrough_debug_message", "tb2_mqtt_passthrough_debug_epoch"}
    selected = set()
    while required:
        name = required.pop()
        if name in selected or name in boundaries:
            continue
        selected.add(name)
        required.update(callee for callee in re.findall(r"\b(tb2_mqtt_\w+)\s*\(", production[name])
                        if callee in production and callee not in selected)
    bodies = [body for name, body in production.items() if name in selected]
    generated = fixture.replace("/* DECLARATIONS */", source[:FUNCTION.search(source).start()])
    generated = generated.replace("/* PROTOTYPES */", "\n".join(
        body[:body.index("\n{")] + ";" for body in bodies))
    generated = generated.replace("/* PRODUCTION */", "\n\n".join(bodies))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-box-queue-") as directory:
        path = Path(directory) / "queue.c"
        executable = Path(directory) / "queue"
        path.write_text(generated)
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-Wno-unused-variable", "-Wno-unused-parameter", "-DGPL_LICENSE_TERMS_ACCEPTED",
                        "-DHTTP_SERVER_MAX_CONNECTIONS=32", "-DTRACE_NOPATH_FILE",
                        *["-I" + item for item in includes], str(path), "tests/mqtt_debug_stubs.c",
                        "cJSON/cJSON.c", "cyclone/cyclone_crypto/hash/sha256.c",
                        "cyclone/common/cpu_endian.c", "-pthread", "-o", str(executable)],
                       cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()

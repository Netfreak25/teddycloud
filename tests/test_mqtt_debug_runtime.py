#!/usr/bin/env python3
"""Exercise the production collector/writer/export on a temporary filesystem."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-debug-") as directory:
        executable = Path(directory) / "debug-test"
        data = Path(directory) / "data"
        data.mkdir()
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-Wno-unused-parameter", "-Wno-cast-function-type",
                        "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
                        *["-I" + path for path in includes], "tests/test_mqtt_debug.c",
                        "src/cyclone/common/os_port_posix.c", "cJSON/cJSON.c",
                        "-pthread", "-lm", "-o", str(executable)], cwd=root, check=True)
        subprocess.run([str(executable), str(data)], cwd=root, check=True, timeout=20)


if __name__ == "__main__":
    main()

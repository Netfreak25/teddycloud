#!/usr/bin/env python3
"""Run the actual upstream worker/queues with only DNS/TLS boundaries stubbed."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-upstream-") as directory:
        executable = Path(directory) / "worker"
        subprocess.run(["gcc", "-Wall", "-Werror",
                        "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
                        *["-I" + path for path in includes],
                        "tests/test_mqtt_upstream_worker.c",
                        "src/cyclone/common/os_port_posix.c", "-pthread", "-o", str(executable)],
                       cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == "__main__":
    main()

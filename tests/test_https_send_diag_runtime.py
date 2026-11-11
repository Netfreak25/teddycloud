#!/usr/bin/env python3
"""Exercise production chapter diagnostics/socketSend at controlled OS boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    platform = (root / "src/platform/platform_linux.c").read_text(encoding="utf-8")
    match = re.search(r"^error_t socketSend\([^;]*?\n\{[\s\S]*?\n\}", platform, re.M)
    assert match is not None, "production socketSend missing"
    fixture = (root / "tests/test_https_send_diag_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* PRODUCTION_DIAGNOSTICS */",
                              (root / "src/https_send_diag.c").read_text(encoding="utf-8"))
    fixture = fixture.replace("/* PRODUCTION_SOCKET_SEND */", match.group())
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-https-send-diag-") as directory:
        source = Path(directory) / "https-send.c"
        executable = Path(directory) / "https-send"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-D_DEFAULT_SOURCE", "-DGPL_LICENSE_TERMS_ACCEPTED", "-DTRACE_NOPATH_FILE",
            "-DHTTP_SERVER_MAX_CONNECTIONS=32", *["-I" + path for path in includes],
            str(source), "-pthread", "-o", str(executable),
        ], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()

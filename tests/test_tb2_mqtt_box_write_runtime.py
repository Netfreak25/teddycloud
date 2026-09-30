#!/usr/bin/env python3
"""Exercise TB2 writes through the unchanged vendored TLS buffering functions.

Function bodies are inserted verbatim, as in the session runtime suite. Only the
socket, clock, handshake and cryptographic boundaries are controlled by the test;
the deterministic cipher verifies buffer continuation, not real cryptography.
"""
from pathlib import Path
import re
import subprocess
import tempfile


FUNCTION = re.compile(
    r"^(?:static )?[\w *]+\b(\w+)\([^;]*?\n\{[\s\S]*?\n\}", re.M
)


def functions(source):
    return {match[1]: match.group() for match in FUNCTION.finditer(source)}


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/tb2_mqtt_passthrough.c").read_text(encoding="utf-8")
    production = functions(source)
    required = {"tb2_mqtt_tls_write_step", "tb2_mqtt_passthrough_box_write_error",
                "tb2_mqtt_box_send", "tb2_mqtt_box_receive", "tb2_mqtt_box_control_step"}
    selected = set()
    while required:
        name = required.pop()
        if name in selected:
            continue
        if name not in production:
            raise AssertionError(f"Production function missing: {name}")
        selected.add(name)
        required.update(callee for callee in re.findall(r"\b(tb2_mqtt_\w+)\s*\(", production[name])
                        if callee in production and callee not in selected)
    bodies = [body for name, body in production.items() if name in selected]
    vendor = {}
    for filename in ("tls.c", "tls_record.c", "tls_misc.c"):
        vendor.update(functions((root / "cyclone/cyclone_ssl" / filename).read_text(encoding="utf-8")))
    vendor_names = ("tlsGetState", "tlsChangeState", "tlsProcessError", "tlsWriteRecord",
                    "tlsWriteProtocolData", "tlsWrite")
    fixture = (root / "tests/test_tb2_mqtt_box_write_runtime.c").read_text(encoding="utf-8")
    declarations = source[:FUNCTION.search(source).start()]
    generated = fixture.replace("/* PRODUCTION_DECLARATIONS */", declarations)
    generated = generated.replace("/* PRODUCTION_PROTOTYPES */",
                                  "\n".join(body[:body.index("\n{")] + ";" for body in bodies))
    generated = generated.replace("/* VENDOR_FUNCTIONS */", "\n\n".join(vendor[name] for name in vendor_names))
    generated = generated.replace("/* PRODUCTION_FUNCTIONS */", "\n\n".join(bodies))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-box-write-") as directory:
        test_source = Path(directory) / "box-write.c"
        executable = Path(directory) / "box-write"
        test_source.write_text(generated, encoding="utf-8")
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-Wno-unused-variable", "-Wno-unused-parameter", "-DGPL_LICENSE_TERMS_ACCEPTED",
                        "-DHTTP_SERVER_MAX_CONNECTIONS=32", "-DTRACE_NOPATH_FILE",
                        *["-I" + path for path in includes], str(test_source), "cyclone/common/cpu_endian.c",
                        "-pthread", "-o", str(executable)], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()

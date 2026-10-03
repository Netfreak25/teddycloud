#!/usr/bin/env python3
"""Compile the production V3 response completion with controlled HTTP writes."""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/handler.c").read_text(encoding="utf-8")
    names = ("finishFreshnessResponseV3", "mergeFreshnessCloudResponseV3",
             "receiveFreshnessCloudResponseV3")
    functions = []
    for name in names:
        match = re.search(r"^(?:static )?[\w *]+\b" + name +
                          r"\([^;]*?\n\{[\s\S]*?\n\}", source, re.M)
        assert match is not None, name
        functions.append(match.group())
    body = source[source.index("void cbrCloudBodyPassthrough("):]
    case = body[body.index("case V3_FRESHNESS_CHECK:"):
                body.index("case V1_FRESHNESS_CHECK:")]
    assert "receiveFreshnessCloudResponseV3" in case and "return;" in case
    disconnect = source[source.index("void cbrCloudServerDiscoPassthrough("):
                        source.index("char *strupr(")]
    assert "finishFreshnessResponseV3(ctx);" in disconnect
    fixture = (root / "tests/test_v3_freshness_response_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* RESPONSE_FUNCTIONS */", "\n\n".join(functions))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-v3-fresh-response-") as directory:
        generated = Path(directory) / "response.c"
        executable = Path(directory) / "response"
        generated.write_text(fixture, encoding="utf-8")
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
            "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
            "-DTRACE_NOPATH_FILE", *["-I" + path for path in includes],
            str(generated), "cJSON/cJSON.c", "-o", str(executable),
        ], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()

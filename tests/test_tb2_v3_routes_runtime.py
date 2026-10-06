#!/usr/bin/env python3
"""Compile and execute the real V3 native cache against a temporary POSIX store."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    sources = ["tests/test_tb2_v3_routes_runtime.c", "src/v3_native_cache.c",
               "src/tb2_ruid.c", "src/fs_ext.c", "cyclone/common/fs_port_posix.c",
               "cyclone/common/path.c", "cyclone/common/str.c",
               "cyclone/common/date_time.c", "cyclone/common/cpu_endian.c", "cJSON/cJSON.c",
               "cyclone/cyclone_crypto/hash/sha256.c"]
    with tempfile.TemporaryDirectory(prefix="tc-v3-routes-") as directory:
        executable = Path(directory) / "routes"
        handler = (root / "src/handler_cloud.c").read_text(encoding="utf-8")
        helpers = handler[handler.index("static bool_t tonie_cloud_access_allowed("):
                          handler.index("void markCustomTonie(")]
        fixture = (root / sources[0]).read_text(encoding="utf-8")
        assert "/* HANDLER_AUTH_AND_POLICY_FUNCTIONS */" in fixture
        context = re.search(r"typedef struct\s*\{[^}]*\}\s*v3_native_chapter_cbr_t;",
                            handler)
        assert context is not None
        callbacks = [context.group()]
        for name in ("v3_native_chapter_response", "v3_native_chapter_body"):
            function = re.search(r"^static void " + name + r"\([^;]*?\n\{[\s\S]*?^\}",
                                 handler, re.M)
            assert function is not None, name
            callbacks.append(function.group())
        fixture = fixture.replace("/* HANDLER_AUTH_AND_POLICY_FUNCTIONS */", helpers)
        fixture = fixture.replace("/* HANDLER_CHAPTER_CALLBACKS */", "\n\n".join(callbacks))
        generated = Path(directory) / "routes.c"
        generated.write_text(fixture, encoding="utf-8")
        sources[0] = str(generated)
        flags = [
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
            "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
            "-DTRACE_NOPATH_FILE", "-ffunction-sections", "-fdata-sections",
            *["-I" + path for path in includes],
        ]
        objects = []
        for index, source in enumerate(sources):
            output = Path(directory) / f"unit-{index}.o"
            # Existing POSIX adapters have these unrelated warnings; keep the
            # changed production cache and fixture under the full warning gate.
            warnings = {"src/fs_ext.c": ["-Wno-unused-parameter"],
                        "cyclone/common/fs_port_posix.c": ["-Wno-sign-compare"]}
            subprocess.run([*flags, *warnings.get(source, []), "-c", source,
                            "-o", str(output)], cwd=root, check=True)
            objects.append(str(output))
        subprocess.run(["gcc", *objects, "-Wl,--gc-sections", "-o", str(executable)],
                       cwd=root, check=True)
        cases = sys.argv[1:] or subprocess.check_output(
            [str(executable), "--list"], text=True).splitlines()
        failures = []
        for case in cases:
            work = Path(directory) / case
            work.mkdir()
            if case.startswith("restart-"):
                # Two actual processes, sharing only the persisted cache/library.
                subprocess.run([str(executable), "--seed", case], cwd=work,
                               timeout=20, check=True)
            result = subprocess.run([str(executable), case], cwd=work, timeout=20)
            if result.returncode:
                failures.append(case)
        if failures:
            raise SystemExit("FAIL: " + ", ".join(failures))
        print(f"PASS: {len(cases)} production V3 route runtime cases")


if __name__ == "__main__":
    main()

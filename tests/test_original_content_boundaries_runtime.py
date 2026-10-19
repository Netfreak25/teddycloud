#!/usr/bin/env python3
"""Run production query parsers and TAF transfer callbacks at controlled transport boundaries."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cyclone/cyclone_crypto/pkc",
                "cyclone/cyclone_crypto/pkix", "cyclone/cyclone_crypto/rng", "cJSON"]
    sources = ["tests/test_original_content_boundaries_runtime.c", "src/fs_ext.c",
               "src/cyclone/common/os_port_posix.c", "cyclone/common/fs_port_posix.c",
               "cyclone/common/path.c", "cyclone/common/cpu_endian.c", "cyclone/common/date_time.c",
               "cyclone/cyclone_crypto/hash/sha1.c", "cyclone/cyclone_crypto/hash/sha256.c",
               "src/proto/proto/toniebox.pb.taf-header.pb-c.c", "src/proto/protobuf-c.c", "cJSON/cJSON.c"]
    flags = ["-std=c11", "-D_DEFAULT_SOURCE", "-ffunction-sections", "-fdata-sections",
             "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32"]
    with tempfile.TemporaryDirectory(prefix="tc-original-boundaries-") as directory:
        executable = Path(directory) / "boundaries"
        objects = []
        for index, source in enumerate(sources):
            output = Path(directory) / f"{index}.o"
            warnings = ["-Wall", "-Wextra", "-Werror"] if index == 0 else ["-w"]
            subprocess.run(["gcc", *flags, *warnings, *["-I" + path for path in includes],
                            "-c", source, "-o", str(output)], cwd=root, check=True)
            objects.append(str(output))
        subprocess.run(["gcc", *objects, "-Wl,--gc-sections", "-pthread", "-lm", "-o", str(executable)], check=True)
        for case in ["query", "overlay", "uint", "success", "repeated-eof", "missing-eof",
                     "partial", "write-error", "flush-error", "transport-error", "status-error"]:
            work = Path(directory) / case
            work.mkdir()
            subprocess.run([str(executable), case], cwd=work, timeout=15, check=True)


if __name__ == "__main__":
    main()

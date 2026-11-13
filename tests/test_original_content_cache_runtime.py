#!/usr/bin/env python3
"""Execute the production original-cache queue/policy/worker; stub HTTP only."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cyclone/cyclone_crypto/pkc",
                "cyclone/cyclone_crypto/pkix", "cyclone/cyclone_crypto/rng", "cJSON"]
    sources = ["tests/test_original_content_cache_runtime.c", "src/cyclone/common/os_port_posix.c",
               "cyclone/common/fs_port_posix.c", "cyclone/common/cpu_endian.c",
               "cyclone/common/date_time.c", "src/tb2_ruid.c", "cJSON/cJSON.c"]
    with tempfile.TemporaryDirectory(prefix="tc-original-cache-") as directory:
        executable = Path(directory) / "original-cache"
        objects = []
        for index, source in enumerate(sources):
            output = Path(directory) / f"{index}.o"
            flags = ["-Wall", "-Wextra", "-Werror"] if index == 0 else ["-w"]
            subprocess.run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-ffunction-sections", *flags,
                            "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
                            *["-I" + path for path in includes], "-c", source,
                            "-o", str(output)], cwd=root, check=True)
            objects.append(str(output))
        subprocess.run(["gcc", *objects, "-Wl,--gc-sections", "-pthread", "-lm", "-o", str(executable)], check=True)
        for case in ["queue", "policy", "worker", "refresh-group", "refresh-api", "refresh-worker"]:
            work = Path(directory) / case
            work.mkdir()
            subprocess.run([str(executable), case], cwd=work, timeout=15, check=True)


if __name__ == "__main__":
    main()

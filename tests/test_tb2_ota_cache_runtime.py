#!/usr/bin/env python3
"""Run the production OTA cache with real files/SHA/threads and fake HTTP."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cyclone/cyclone_crypto/pkc",
                "cyclone/cyclone_crypto/pkix", "cyclone/cyclone_crypto/rng", "cJSON"]
    sources = ["tests/test_tb2_ota_cache_runtime.c", "src/cyclone/common/os_port_posix.c",
               "cyclone/common/fs_port_posix.c", "cyclone/common/cpu_endian.c", "cyclone/common/date_time.c",
               "cyclone/cyclone_crypto/hash/sha256.c", "cJSON/cJSON.c"]
    with tempfile.TemporaryDirectory(prefix="tc-ota-") as directory:
        executable = Path(directory) / "ota"
        objects = []
        defines = ["-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
                   "-DBUILD_GIT_IS_DIRTY=0", "-DWEB_GIT_IS_DIRTY=0",
                   '-DBUILD_PLATFORM="linux"', '-DBUILD_OS="Linux"',
                   '-DBUILD_OS_ID="ubuntu"', '-DBUILD_ARCH="x86_64"', '-DBUILD_ARCH_BITS="64"']
        for unit in ("tb2_ota_cache", "cloud_request", "handler_cloud", "settings", "server"):
            subprocess.run(["gcc", "-fsyntax-only", "-Wall", "-Werror", *defines,
                            *["-I" + path for path in includes], f"src/{unit}.c"], cwd=root, check=True)
            print(f"PASS compiler: {unit}", flush=True)
        for index, source in enumerate(sources):
            output = Path(directory) / f"{index}.o"
            flags = ["-Wall", "-Wextra", "-Werror"] if index == 0 else ["-w"]
            subprocess.run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-ffunction-sections", *flags,
                            "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
                            *["-I" + path for path in includes], "-c", source,
                            "-o", str(output)], cwd=root, check=True)
            objects.append(str(output))
        subprocess.run(["gcc", *objects, "-Wl,--gc-sections", "-pthread", "-lm", "-o", str(executable)], check=True)
        for case in ["disabled", "slot-zero", "slot-one", "invalid-offers", "failures",
                     "concurrent", "concurrent-failure", "legacy-first", "policy", "both-present", "counterpart-failure",
                     "disable-and-snapshot", "shutdown"]:
            work = Path(directory) / case
            work.mkdir()
            subprocess.run([str(executable), case], cwd=work, timeout=15, check=True)


if __name__ == "__main__":
    main()

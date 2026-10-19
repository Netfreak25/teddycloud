#!/usr/bin/env python3
"""Exercise the production TAF reference module with real files, SHA and mutexes."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cyclone/cyclone_crypto/pkc",
                "cyclone/cyclone_crypto/pkix", "cyclone/cyclone_crypto/rng", "cJSON"]
    sources = ["tests/test_taf_original_cache_runtime.c", "src/fs_ext.c",
               "src/cyclone/common/os_port_posix.c", "cyclone/common/fs_port_posix.c",
               "cyclone/common/path.c", "cyclone/common/cpu_endian.c", "cyclone/common/date_time.c",
               "cyclone/cyclone_crypto/hash/sha1.c", "cyclone/cyclone_crypto/hash/sha256.c",
               "src/proto/proto/toniebox.pb.taf-header.pb-c.c", "src/proto/protobuf-c.c", "cJSON/cJSON.c"]
    flags = ["-std=c11", "-D_DEFAULT_SOURCE", "-ffunction-sections", "-fdata-sections",
             "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32"]
    with tempfile.TemporaryDirectory(prefix="tc-taf-reference-") as directory:
        executable = Path(directory) / "taf-reference"
        objects = []
        for index, source in enumerate(sources):
            output = Path(directory) / f"{index}.o"
            warnings = ["-Wall", "-Wextra", "-Werror"] if index == 0 else ["-w"]
            subprocess.run(["gcc", *flags, *warnings, *["-I" + path for path in includes],
                            "-c", source, "-o", str(output)], cwd=root, check=True)
            objects.append(str(output))
        subprocess.run(["gcc", *objects, "-Wl,--gc-sections", "-pthread", "-lm", "-o", str(executable)], check=True)
        for case in ["readonly", "import", "collision", "corrupt", "invalid-library",
                     "invalid-reference", "copy-failure", "reference-failure", "flush-failure",
                     "rename-failure", "cleanup-failure", "scope", "concurrent"]:
            work = Path(directory) / case
            work.mkdir()
            subprocess.run([str(executable), case], cwd=work, timeout=15, check=True)
        work = Path(directory) / "restart"
        work.mkdir()
        subprocess.run([str(executable), "import"], cwd=work, timeout=15, check=True)
        subprocess.run([str(executable), "restart"], cwd=work, timeout=15, check=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run actual versioned playlist metadata and Ogg parsing with local fixtures."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON",
                "ogg/include", "ogg/src", "opus/include", "opus/src", "opus/celt",
                "opus/silk", "opus/silk/float"]
    sources = ["tests/test_v3_playlist_metadata_runtime.c", "src/encoder_api.c",
               "src/v3_native_cache.c", "src/tb2_ruid.c", "src/fs_ext.c", "src/server_helpers.c",
               "cyclone/common/fs_port_posix.c", "cyclone/common/path.c", "cyclone/common/str.c",
               "cyclone/common/date_time.c", "cyclone/common/cpu_endian.c", "cJSON/cJSON.c",
               "cyclone/cyclone_crypto/hash/sha256.c", "ogg/src/framing.c", "ogg/src/bitwise.c",
               "opus/src/opus.c", "opus/src/opus_decoder.c"]
    warnings = {"src/fs_ext.c": ["-Wno-unused-parameter"],
                "src/server_helpers.c": ["-Wno-sign-compare", "-Wno-unused-parameter"],
                "cyclone/common/fs_port_posix.c": ["-Wno-sign-compare"],
                "opus/src/opus_decoder.c": ["-Wno-unused-parameter"]}
    with tempfile.TemporaryDirectory(prefix="tc-v3-playlist-") as directory:
        work = Path(directory)
        objects = []
        for index, source in enumerate(sources):
            output = work / f"{index}.o"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
                            "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
                            "-DTRACE_NOPATH_FILE", "-DUSE_ALLOCA", "-DOPUS_BUILD",
                            "-ffunction-sections", "-fdata-sections", *["-I" + path for path in includes],
                            *warnings.get(source, []), "-c", source, "-o", str(output)], cwd=root, check=True)
            objects.append(str(output))
        executable = work / "metadata"
        boundaries = ["fsWriteFile", "fsFlushFile", "fsRenameFile",
                      "encoder_read_opus_metadata", "v3_native_cache_metadata_snapshot"]
        subprocess.run(["cc", *objects, "-Wl,--gc-sections",
                        *["-Wl,--wrap=" + name for name in boundaries], "-lm", "-o", str(executable)], check=True)
        for case in ["structure", "collection", "atomic", "enrich", "concurrent-observe",
                     "corruption", "assigned-collection", "attempt-write", "attempt-restart"]:
            case_dir = work / ("attempt" if case.startswith("attempt-") else case)
            case_dir.mkdir(exist_ok=True)
            subprocess.run([str(executable), case], cwd=case_dir, timeout=15, check=True)


if __name__ == "__main__":
    main()

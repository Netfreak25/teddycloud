#!/usr/bin/env python3
"""Exercise real native encoder handlers without a server or cloud connection."""
from pathlib import Path
import argparse
import subprocess
import tempfile


def build(root, directory, executable):
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON",
                "ogg/include", "ogg/src", "opus/include", "opus/src", "opus/celt",
                "opus/silk", "opus/silk/float"]
    sources = ["tests/test_encoder_native_runtime.c", "src/v3_native_cache.c",
               "src/tb2_ruid.c", "src/fs_ext.c", "src/server_helpers.c", "src/taf_upload.c",
               "src/proto/protobuf-c.c", "src/proto/proto/toniebox.pb.taf-header.pb-c.c",
               "cyclone/common/fs_port_posix.c", "cyclone/common/path.c",
               "cyclone/common/str.c", "cyclone/common/date_time.c",
               "cyclone/common/cpu_endian.c", "cJSON/cJSON.c",
               "cyclone/cyclone_crypto/hash/sha256.c", "cyclone/cyclone_crypto/hash/sha1.c",
               "ogg/src/framing.c", "ogg/src/bitwise.c", "opus/src/opus.c", "opus/src/opus_decoder.c"]
    flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
             "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
             "-DTRACE_NOPATH_FILE", "-DUSE_ALLOCA", "-DOPUS_BUILD",
             "-ffunction-sections", "-fdata-sections", *["-I" + path for path in includes]]
    objects = []
    for index, source in enumerate(sources):
        output = directory / f"unit-{index}.o"
        # Existing adapters/vendor units are unchanged. Keep changed handlers and
        # library publication under the full warning gate.
        warnings = {"src/fs_ext.c": ["-Wno-unused-parameter"],
                    "src/server_helpers.c": ["-Wno-sign-compare", "-Wno-unused-parameter"],
                    "cyclone/common/fs_port_posix.c": ["-Wno-sign-compare"],
                    "opus/src/opus_decoder.c": ["-Wno-unused-parameter"]}
        subprocess.run([*flags, *warnings.get(source, []), "-c", source, "-o", str(output)],
                       cwd=root, check=True)
        objects.append(str(output))
    subprocess.run(["cc", *objects, "-Wl,--gc-sections", "-Wl,--wrap=fsFlushFile", "-lm", "-o", str(executable)],
                   cwd=root, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-only", type=Path, help="Keep a bridge executable at this path")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="tc-encoder-") as directory:
        work = Path(directory)
        executable = args.build_only.resolve() if args.build_only else work / "encoder"
        executable.parent.mkdir(parents=True, exist_ok=True)
        build(root, work, executable)
        if args.build_only:
            print(executable)
        else:
            subprocess.run([str(executable)], cwd=work, timeout=30, check=True)


if __name__ == "__main__":
    main()

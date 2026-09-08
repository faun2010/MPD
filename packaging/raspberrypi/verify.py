#!/usr/bin/env python3
"""Exercise ARMv8-A decoder output and (optionally) a relocated runtime bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=120, **kwargs)


def chunk(name, data):
    return name + struct.pack(">Q", len(data)) + data + b"\0" * (len(data) % 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/rpi-arm64"))
    parser.add_argument("--report-dir", type=Path)
    parser.add_argument("--bundle-dir", type=Path)
    parser.add_argument("--iso-uri", help="Local ISO path followed by its virtual track filename")
    parser.add_argument("--iso-reference", type=Path, help="Independently decoded raw DSD")
    parser.add_argument("--iso-seek", type=float, default=0)
    args = parser.parse_args()
    if bool(args.iso_uri) != bool(args.iso_reference):
        parser.error("--iso-uri and --iso-reference must be used together")
    build = args.build_dir.resolve()
    report_dir = (args.report_dir or build / "verification").resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    qemu = ["qemu-aarch64", "-cpu", "cortex-a53", "-L", "/"]
    config = report_dir / "decoder.conf"
    config.write_text('decoder {\n plugin "dsdiff"\n lsbitfirst "no"\n use_stdio "yes"\n}\n'
                      'decoder {\n plugin "sacdiso"\n lsbitfirst "no"\n use_stdio "yes"\n}\n')

    # Byte-pattern DSD and integer PCM are generated without musical samples.
    dsd = bytes((n * 37 + n // 17) % 256 for n in range(705600))
    prop = (b"SND " + chunk(b"FS  ", struct.pack(">I", 2822400))
            + chunk(b"CHNL", struct.pack(">H", 2) + b"SLFTSRGT")
            + chunk(b"CMPR", b"DSD " + b"\x0enot compressed"))
    body = b"DSD " + chunk(b"FVER", b"\x01\x05\0\0") + chunk(b"PROP", prop) + chunk(b"DSD ", dsd)
    (report_dir / "synthetic.dff").write_bytes(b"FRM8" + struct.pack(">Q", len(body)) + body)
    pcm = b"".join(struct.pack("<hh", (n * 97 % 60001) - 30000,
                               (n * 193 % 60001) - 30000) for n in range(8820))
    helper = report_dir / "codec_probe.c"
    helper.write_text(r'''
#include <FLAC/stream_encoder.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    if (!avcodec_find_decoder_by_name("ape") || !av_find_input_format("ape")) return 2;
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    if (!e) return 3;
    FLAC__int32 data[8820 * 2];
    for (unsigned n = 0; n < 8820; ++n) {
        data[n * 2] = (int)(n * 97 % 60001) - 30000;
        data[n * 2 + 1] = (int)(n * 193 % 60001) - 30000;
    }
    int ok = FLAC__stream_encoder_set_channels(e, 2)
        && FLAC__stream_encoder_set_bits_per_sample(e, 16)
        && FLAC__stream_encoder_set_sample_rate(e, 44100)
        && FLAC__stream_encoder_init_file(e, argv[1], NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK
        && FLAC__stream_encoder_process_interleaved(e, data, 8820)
        && FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    return ok ? 0 : 4;
}
''')
    run(["aarch64-linux-gnu-gcc", "-march=armv8-a", str(helper), "-o",
         str(report_dir / "codec_probe"), "-lFLAC", "-lavcodec", "-lavformat"])
    run(qemu + [str(report_dir / "codec_probe"), str(report_dir / "synthetic.flac")])
    cases = [("DFF", "dsdiff", str(report_dir / "synthetic.dff"), dsd, 0),
             ("FLAC", "flac", str(report_dir / "synthetic.flac"), pcm, 0)]
    if args.iso_uri:
        cases.append(("SACD ISO", "sacdiso", args.iso_uri,
                      args.iso_reference.read_bytes(), args.iso_seek))
    rows = []
    for label, decoder, uri, expected, seek in cases:
        command = qemu + [str(build / "test/run_decoder"), "--config=" + str(config)]
        if seek:
            command.append("--seek=" + str(seek))
        output = report_dir / (decoder + ".raw")
        with output.open("wb") as stream:
            result = run(command + [decoder, uri], stdout=stream, stderr=subprocess.PIPE)
        (report_dir / (decoder + ".log")).write_bytes(result.stderr)
        actual = output.read_bytes()
        if actual != expected or not actual:
            raise RuntimeError(f"{label}: decoded bytes differ from independent reference")
        rows.append({"case": label, "bytes": len(actual), "matches_reference": True,
                     "sha256": hashlib.sha256(actual).hexdigest()})
        print(f"PASS {label}: {len(actual)} reference bytes", flush=True)

    testlog = build / "meson-logs/testlog.json"
    tests = [json.loads(line) for line in testlog.read_text().splitlines() if line]
    if not tests or any(item["result"] != "OK" for item in tests):
        raise RuntimeError("ARM64 Meson tests have not all passed")
    report = {
        "qemu_cpu": "cortex-a53 (ARMv8-A)",
        "unit_test_groups_passed": len(tests),
        "unit_tests": [{"name": item["name"], "result": item["result"]} for item in tests],
        "decode_checks": rows,
        "sacd_iso_sample_tested": bool(args.iso_uri),
        "ape_decoder_and_demuxer_available": True,
        "ape_sample_decode_tested": False,
        "full_mpd_startup": "not tested under QEMU user (MPD startup thread guard)",
        "raspberry_pi_hardware_playback": "not tested",
        "binary_sha256": hashlib.sha256((build / "mpd").read_bytes()).hexdigest(),
    }
    if args.bundle_dir:
        # This can be pointed at an extracted bundle to prove relocation.
        bundle = args.bundle_dir.resolve()
        result = run(qemu + ["-E", "LD_LIBRARY_PATH=" + str(bundle / "lib"),
                            "/lib/ld-linux-aarch64.so.1", "--list", str(bundle / "bin/mpd")],
                     capture_output=True, text=True)
        if "not found" in result.stdout + result.stderr:
            raise RuntimeError("Bundled dependency not found")
        import re
        manifest = json.loads((bundle / "BUILD.json").read_text())
        resolved = dict(re.findall(r"^\s*(\S+) => (.+?) \(0x[0-9a-fA-F]+\)\s*$",
                                   result.stdout, re.MULTILINE))
        for dependency in manifest["bundled_libraries"]:
            if resolved.get(dependency) != str(bundle / "lib" / dependency):
                raise RuntimeError(f"Dependency escaped relocated bundle: {dependency}")
        (report_dir / "bundled-loader.txt").write_text(result.stdout + result.stderr)
        report["bundled_dynamic_loader_check"] = "passed"
    (report_dir / "VALIDATION.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

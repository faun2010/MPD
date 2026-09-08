#!/usr/bin/env python3
"""Fully decode local SACD ISO/DFF samples using MPD's named native decoders.

Raw output lives in one temporary regular file at a time, avoiding pipe short
writes in DumpDecoderClient. DFF output must match the source DSD payload.
This checks decoder output, not a sound device or audible playback.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time


def digest_region(path, offset=0, length=None):
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as source:
        source.seek(offset)
        while length is None or total < length:
            data = source.read(min(1024 * 1024, length - total)
                               if length is not None else 1024 * 1024)
            if not data:
                break
            total += len(data)
            digest.update(data)
    if length is not None and total != length:
        raise ValueError(f"Truncated payload: {path}")
    return {"bytes": total, "sha256": digest.hexdigest()}


def dff_payload(path):
    """Read DSDIFF's big-endian chunk headers without trusting decoder metadata."""
    result = {}
    with path.open("rb") as source:
        header = source.read(16)
        if header[:4] != b"FRM8" or header[12:] != b"DSD ":
            raise ValueError(f"Not a DSDIFF file: {path}")
        end = 12 + struct.unpack(">Q", header[4:12])[0]
        if end > path.stat().st_size:
            raise ValueError(f"Truncated FRM8: {path}")
        offset = 16
        while offset + 12 <= end:
            source.seek(offset)
            chunk = source.read(12)
            size = struct.unpack(">Q", chunk[4:])[0]
            if offset + 12 + size > end:
                raise ValueError(f"Truncated {chunk[:4]!r} chunk: {path}")
            if chunk[:4] == b"DSD ":
                result.update(offset=offset + 12, bytes=size)
            elif chunk[:4] == b"PROP":
                prop = source.read(size)
                if prop[:4] != b"SND ":
                    raise ValueError(f"Unexpected PROP type: {path}")
                pos = 4
                while pos + 12 <= len(prop):
                    name = prop[pos:pos + 4]
                    n = struct.unpack(">Q", prop[pos + 4:pos + 12])[0]
                    data = prop[pos + 12:pos + 12 + n]
                    if len(data) != n:
                        raise ValueError(f"Truncated PROP subchunk: {path}")
                    if name == b"FS  ":
                        result["dsd_sample_rate"] = struct.unpack(">I", data)[0]
                    elif name == b"CHNL":
                        result["channels"] = struct.unpack(">H", data[:2])[0]
                    elif name == b"CMPR":
                        result["compression"] = data[:4].decode("ascii")
                    pos += 12 + n + n % 2
            offset += 12 + size + size % 2
    if result.get("compression") != "DSD " or "offset" not in result:
        raise ValueError(f"Raw DSD payload comparison requires uncompressed DFF: {path}")
    result.update(digest_region(path, result["offset"], result["bytes"]))
    result["duration_seconds"] = (result["bytes"] * 8 /
                                  result["dsd_sample_rate"] / result["channels"])
    return result


def capture(command, timeout):
    started = time.monotonic()
    completed = subprocess.run(command, capture_output=True, text=True,
                               errors="replace", timeout=timeout)
    return {"command": command, "returncode": completed.returncode,
            "wall_seconds": round(time.monotonic() - started, 3),
            "stdout": completed.stdout, "stderr": completed.stderr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("music", type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build/sacd-debug"))
    parser.add_argument("--report-dir", type=Path)
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    report_dir = (args.report_dir or build / "verification" / "decode").resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    config = report_dir / "mpd-decoders.conf"
    config.write_text('''decoder {
    plugin "ffmpeg"
    enabled "no"
}
decoder {
    plugin "dvdaiso"
    enabled "no"
}
decoder {
    plugin "sacdiso"
    lsbitfirst "no"
}
decoder {
    plugin "dsdiff"
    lsbitfirst "no"
}
''')
    helpers = {name: str(build / "test" / name)
               for name in ("ContainerScan", "run_decoder", "read_tags")}
    for helper in helpers.values():
        if not Path(helper).is_file():
            parser.error(f"Build helper first: {helper}")
    files = sorted(p for p in args.music.resolve().rglob("*")
                   if p.is_file() and p.suffix.lower() in (".iso", ".dff"))
    if not files:
        parser.error("No ISO/DFF samples found")
    report = {"music_root": str(args.music.resolve()),
              "config": str(config), "helpers": {
                  name: dict(path=path, **digest_region(Path(path)))
                  for name, path in helpers.items()},
              "scope": "Full native decoder output; no sound-device evidence",
              "containers": [], "tracks": [], "failures": []}

    def save():
        (report_dir / "report.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n")

    jobs = []
    for path in files:
        if path.suffix.lower() == ".iso":
            scan = capture([helpers["ContainerScan"], str(path)], args.timeout)
            names = re.findall(r"^song_begin: (.+)$", scan["stdout"], re.MULTILINE)
            report["containers"].append(dict(path=str(path), tracks=names, **scan))
            if scan["returncode"] != 0 or not names:
                report["failures"].append(f"Container scan failed: {path}")
            for name in names:
                jobs.append(("sacdiso", path / name, None))
        else:
            jobs.append(("dsdiff", path, dff_payload(path)))
    save()
    for index, (plugin, uri, payload) in enumerate(jobs, 1):
        print(f"[{index}/{len(jobs)}] {plugin} {uri.name}", flush=True)
        item = {"plugin": plugin, "uri": str(uri), "checks": {}}
        if payload:
            item["source_dsd_payload"] = payload
        command = [helpers["run_decoder"], f"--config={config}", plugin, str(uri)]
        item["command"] = command
        try:
            tags = capture([helpers["read_tags"], f"--config={config}", plugin,
                            str(uri)], args.timeout)
            item["tags"] = tags
            started = time.monotonic()
            with tempfile.NamedTemporaryFile(prefix="mpd-dsd-", suffix=".raw",
                                             dir="/tmp") as output:
                decoded = subprocess.run(command, stdout=output,
                                         stderr=subprocess.PIPE, text=True,
                                         errors="replace", timeout=args.timeout)
                item["wall_seconds"] = round(time.monotonic() - started, 3)
                item["output"] = digest_region(Path(output.name))
            item["returncode"] = decoded.returncode
            item["stderr"] = decoded.stderr
            (report_dir / f"{index:03d}-{plugin}.stderr").write_text(decoded.stderr)
            match = re.search(r"audio_format=(\S+) duration=([0-9.]+) seekable=(\S+)",
                              decoded.stderr)
            item["checks"].update(exit_success=decoded.returncode == 0,
                                  nonempty=item["output"]["bytes"] > 0,
                                  tags_success=tags["returncode"] == 0,
                                  format_and_duration=match is not None,
                                  no_decoder_errors=not re.search(
                                      r"(?i)(?:error|failed|exception|invalid|unrecognized)",
                                      decoded.stderr))
            if match:
                item["audio_format"] = match[1]
                item["reported_duration_seconds"] = float(match[2])
                item["seekable"] = match[3] == "true"
                format_match = re.fullmatch(r"dsd(\d+):(\d+)", match[1])
                if format_match:
                    sample_rate = int(format_match[1]) * 44100
                    channels = int(format_match[2])
                    duration = item["output"]["bytes"] * 8 / sample_rate / channels
                    item["decoded_duration_seconds"] = duration
                    item["duration_difference_seconds"] = duration - float(match[2])
                    # Ready() rounds duration to milliseconds; a missing 1/75 s
                    # DSD frame must still fail this independent length check.
                    item["checks"]["duration_matches"] = abs(duration - float(match[2])) < 0.001
                    if payload:
                        item["checks"]["source_format_matches"] = (
                            sample_rate == payload["dsd_sample_rate"] and
                            channels == payload["channels"])
                else:
                    item["checks"]["dsd_format"] = False
            if payload:
                item["checks"]["payload_bytes_match"] = item["output"]["bytes"] == payload["bytes"]
                item["checks"]["payload_sha256_match"] = item["output"]["sha256"] == payload["sha256"]
            item["passed"] = all(item["checks"].values())
        except Exception as error:
            item["error"] = repr(error)
            item["passed"] = False
        report["tracks"].append(item)
        if not item["passed"]:
            report["failures"].append(str(uri))
        save()
        print(f"  {'PASS' if item['passed'] else 'FAIL'} "
              f"{item.get('output', {}).get('bytes', 0)} bytes; "
              f"{item.get('decoded_duration_seconds', '?')} s", flush=True)
    report["summary"] = {
        "iso_files": sum(p.suffix.lower() == ".iso" for p in files),
        "dff_files": sum(p.suffix.lower() == ".dff" for p in files),
        "decoded_tracks": len(report["tracks"]),
        "passed_tracks": sum(t["passed"] for t in report["tracks"]),
        "decoded_bytes": sum(t.get("output", {}).get("bytes", 0) for t in report["tracks"]),
        "passed": bool(report["tracks"]) and not report["failures"],
    }
    save()
    print(json.dumps(report["summary"], ensure_ascii=False), flush=True)
    print(f"Report: {report_dir / 'report.json'}", flush=True)
    return 0 if report["summary"]["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

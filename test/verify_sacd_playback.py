#!/usr/bin/env python3
"""Exercise the isolated local MPD configured in build/sacd-debug/runtime.

This replaces that test instance's queue and plays through its ALSA output.
Run after starting MPD with runtime/mpd.conf; never aim at a personal queue.
"""

import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import socket
import sys
import time
import wave


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.file = self.socket.makefile("rb")
        assert self.file.readline().startswith(b"OK MPD ")

    def command(self, name, *args, allow_ack=False):
        def quote(value):
            return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'
        message = " ".join([name, *(quote(a) for a in args)]) + "\n"
        self.socket.sendall(message.encode())
        result = []
        while True:
            line = self.file.readline().decode().rstrip("\n")
            if line == "OK":
                return result
            if line.startswith("ACK"):
                if allow_ack:
                    return result + [line]
                raise RuntimeError(line)
            if not line:
                raise RuntimeError("MPD disconnected")
            result.append(line)
            if line.startswith("binary: "):
                size = int(line.split(": ", 1)[1])
                assert len(self.file.read(size)) == size
                assert self.file.read(1) == b"\n"

    def status(self):
        return dict(line.split(": ", 1) for line in self.command("status"))

    def playing(self):
        value = self.status()
        assert value["state"] == "play" and "error" not in value, value
        assert value["audio"] == "dsd64:2", value
        return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=6601)
    parser.add_argument("--runtime", type=Path, default=Path("build/sacd-debug/runtime"))
    parser.add_argument("--report", type=Path, default=Path("build/sacd-debug/verification/playback.json"))
    parser.add_argument("--dsd-output", default="Shanling UA2 Plus native DSD",
                        help="name of audio_output 0, the native DSD ALSA device")
    parser.add_argument("--pcm-output", default="PCM verification",
                        help="name of audio_output 1, the recorder capturing PCM")
    parser.add_argument("--alsa-card", default="Plus",
                        help="ALSA card id under /proc/asound used for hardware pointer checks")
    args = parser.parse_args()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    client = Client(args.port)
    outputs = client.command("outputs")
    assert f"outputname: {args.dsd_output}" in outputs, outputs
    assert f"outputname: {args.pcm_output}" in outputs, outputs
    client.command("stop")
    client.command("clear")
    client.command("random", 0)
    client.command("repeat", 0)
    client.command("single", 0)
    client.command("consume", 0)
    client.command("enableoutput", 0)
    client.command("disableoutput", 1)
    client.command("update")
    while "updating_db" in client.status():
        time.sleep(0.1)
    songs = sorted(line[6:] for line in client.command("listall") if line.startswith("file: "))
    assert songs and len(songs) == len(set(songs)), songs
    isos = [s for s in songs if ".iso/" in s]
    dffs = [s for s in songs if s.lower().endswith(".dff") and ".dff/" not in s]
    assert not any(".dff/" in s for s in songs), "DFF must not be scanned as a container"
    assert isos and dffs, songs
    # Other formats in the library (FLAC, CUE...) are covered by verify_format_switch.py.
    songs = isos + dffs
    pid = int((args.runtime / "pid").read_text())
    with Path(f"/proc/{pid}/exe").open("rb") as binary:
        binary_hash = hashlib.file_digest(binary, "sha256").hexdigest()
    report = {"outputs": outputs, "mpd_pid": pid, "mpd_sha256": binary_hash,
              "iso_tracks": len(isos), "dff_files": len(dffs), "tracks": []}
    hw = Path(f"/proc/asound/{args.alsa_card}/pcm0p/sub0")
    try:
        for index, song in enumerate(songs):
            client.command("clear")
            client.command("add", song)
            client.command("play", 0)
            deadline = time.monotonic() + 10
            while "audio" not in client.status() and time.monotonic() < deadline:
                time.sleep(0.05)
            client.playing()
            client.command("seekcur", 30)
            time.sleep(0.6)
            before = client.playing()
            params = (hw / "hw_params").read_text()
            hardware_before = (hw / "status").read_text()
            assert "format: DSD_U32_BE" in params and "channels: 2" in params, params
            assert "state: RUNNING" in hardware_before, hardware_before
            # Concurrent scan and artwork requests exercise independent readers.
            if song in (isos[0], dffs[0]):
                client.command("rescan")
                other = dffs[-1] if song in dffs else isos[-1]
                artwork = client.command("readpicture", other, 0, allow_ack=True)
            else:
                artwork = []
            time.sleep(0.6)
            after = client.playing()
            hardware_after = (hw / "status").read_text()
            assert float(after["elapsed"]) > float(before["elapsed"]) >= 30, (before, after)
            def pointer(text):
                return int(next(line.split(":", 1)[1] for line in text.splitlines() if line.startswith("hw_ptr")))
            assert pointer(hardware_after) > pointer(hardware_before), hardware_after
            client.command("pause", 1)
            assert client.status()["state"] == "pause"
            client.command("pause", 0)
            time.sleep(0.1)
            client.playing()
            client.command("stop")
            report["tracks"].append({"uri": song, "status_before": before, "status_after": after,
                                     "hw_params": params, "hardware_before": hardware_before,
                                     "hardware_after": hardware_after, "artwork": artwork})
            print(f"PASS {index + 1}/{len(songs)} {song}", flush=True)

        # Capture actual PCM conversion from both decoders alongside native DSD.
        report["pcm"] = []
        capture = args.runtime / "capture.wav"
        for kind, song in (("iso", isos[0]), ("dff", dffs[0])):
            client.command("clear")
            client.command("add", song)
            client.command("enableoutput", 1)
            client.command("play", 0)
            time.sleep(0.2)
            client.command("seekcur", 30)
            time.sleep(3)
            client.playing()
            client.command("stop")
            client.command("disableoutput", 1)
            deadline = time.monotonic() + 5
            while not capture.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            target = args.report.parent / f"{kind}-sample.wav"
            capture.replace(target)
            with wave.open(str(target)) as wav:
                assert (wav.getframerate(), wav.getsampwidth(), wav.getnchannels()) == (44100, 2, 2)
                samples = array.array("h", wav.readframes(wav.getnframes()))
                if sys.byteorder != "little":
                    samples.byteswap()
            peak = max(abs(v) for v in samples)
            rms = math.sqrt(sum(v * v for v in samples) / len(samples))
            assert rms > 10 and peak > 100, (rms, peak)
            report["pcm"].append({"kind": kind, "uri": song, "file": str(target),
                                  "seconds": len(samples) / 88200, "peak": peak, "rms": rms})
        report["passed"] = True
    finally:
        client.command("stop")
        client.command("disableoutput", 1)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS: {len(songs)} tracks, native DSD and PCM captures; {args.report}")


if __name__ == "__main__":
    main()

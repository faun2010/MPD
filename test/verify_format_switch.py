#!/usr/bin/env python3
"""Switch the isolated local MPD between PCM (FLAC/CUE) and native DSD (DFF, SACD ISO).

Every transition must leave MPD in state "play" with no error, the expected
audio format in "status", the ALSA device RUNNING with the expected sample
format and a hardware pointer that keeps advancing.  Transitions are exercised
both as a fresh queue ("play") and as queue advances ("next") without stop,
plus seek and pause/resume inside each format.  Run against the MPD started by
run-local.sh; never aim at a personal queue.
"""

import argparse
import json
from pathlib import Path
import socket
import time


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.file = self.socket.makefile("rb")
        assert self.file.readline().startswith(b"OK MPD ")

    def command(self, name, *args):
        def quote(value):
            return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'
        self.socket.sendall((" ".join([name, *(quote(a) for a in args)]) + "\n").encode())
        result = []
        while True:
            line = self.file.readline().decode().rstrip("\n")
            if line == "OK":
                return result
            if line.startswith("ACK") or not line:
                raise RuntimeError(line or "MPD disconnected")
            result.append(line)

    def status(self):
        return dict(line.split(": ", 1) for line in self.command("status"))


def classify(uri):
    lower = uri.lower()
    if ".iso/" in lower:
        return "iso"
    if lower.endswith(".dff") or lower.endswith(".dsf"):
        return "dsd"
    if ".cue/" in lower:
        return "cue"
    if lower.endswith(".flac"):
        return "flac"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=6601)
    parser.add_argument("--alsa-card", default="Plus")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--report", type=Path,
                        default=Path("build/sacd-debug/verification/format-switch.json"))
    args = parser.parse_args()
    hw = Path(f"/proc/asound/{args.alsa_card}/pcm0p/sub0")
    client = Client(args.port)
    client.command("stop")
    client.command("clear")
    for option in ("random", "repeat", "single", "consume"):
        client.command(option, 0)
    client.command("enableoutput", 0)
    client.command("update")
    while "updating_db" in client.status():
        time.sleep(0.1)
    songs = sorted(line[6:] for line in client.command("listall") if line.startswith("file: "))
    by_kind = {}
    for song in songs:
        kind = classify(song)
        if kind:
            by_kind.setdefault(kind, []).append(song)
    pcm_kinds = [k for k in ("flac", "cue") if k in by_kind]
    dsd_kinds = [k for k in ("dsd", "iso") if k in by_kind]
    assert pcm_kinds and dsd_kinds, f"need PCM and DSD samples: {sorted(by_kind)}"
    # Alternate PCM and DSD, covering every kind, e.g. flac dsd cue iso flac ...
    plan = []
    for round_index in range(args.rounds):
        for step in range(max(len(pcm_kinds), len(dsd_kinds))):
            for kinds in (pcm_kinds, dsd_kinds):
                kind = kinds[step % len(kinds)]
                candidates = by_kind[kind]
                plan.append((kind, candidates[(round_index * 3 + step) % len(candidates)]))
    plan.append(plan[0])  # end where we started: DSD back to the first PCM

    def pointer(text):
        return int(next(line.split(":", 1)[1] for line in text.splitlines()
                        if line.startswith("hw_ptr")))

    def check(kind, label):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status = client.status()
            if status.get("state") == "play" and "audio" in status and "error" not in status:
                break
            time.sleep(0.05)
        status = client.status()
        assert status["state"] == "play" and "error" not in status, (label, status)
        time.sleep(0.8)
        status = client.status()
        assert "error" not in status, (label, status)
        params = (hw / "hw_params").read_text()
        before = (hw / "status").read_text()
        assert "state: RUNNING" in before, (label, before)
        if kind in ("dsd", "iso"):
            assert status["audio"].startswith("dsd"), (label, status)
            assert "format: DSD_" in params, (label, params)
        else:
            assert not status["audio"].startswith("dsd"), (label, status)
            assert "format: DSD_" not in params, (label, params)
        time.sleep(0.5)
        after = (hw / "status").read_text()
        assert pointer(after) > pointer(before), (label, before, after)
        assert float(client.status()["elapsed"]) > float(status["elapsed"]), label
        return {"status": status, "hw_params": params.strip(),
                "hw_ptr_before": pointer(before), "hw_ptr_after": pointer(after)}

    report = {"plan": plan, "transitions": [], "passed": False}
    previous = None
    try:
        # Phase 1: fresh queue per song ("play" after "clear").
        for index, (kind, song) in enumerate(plan):
            client.command("clear")
            client.command("add", song)
            client.command("play", 0)
            label = f"play {previous or 'start'} -> {kind}"
            result = check(kind, label)
            client.command("seekcur", 20)
            result["after_seek"] = check(kind, label + " seek")
            client.command("pause", 1)
            assert client.status()["state"] == "pause", label
            client.command("pause", 0)
            result["after_resume"] = check(kind, label + " resume")
            report["transitions"].append({"mode": "play", "from": previous, "to": kind,
                                          "uri": song, **result})
            print(f"PASS {index + 1}/{len(plan)} {label}: {result['status']['audio']} "
                  f"{[l for l in result['hw_params'].splitlines() if l.startswith('format')][0]}",
                  flush=True)
            previous = kind
        # Phase 2: one queue, advance with "next" while playing (output re-opened live).
        client.command("stop")
        client.command("clear")
        for kind, song in plan:
            client.command("add", song)
        client.command("play", 0)
        previous = None
        for index, (kind, song) in enumerate(plan):
            if index:
                client.command("next")
            label = f"next {previous or 'start'} -> {kind}"
            result = check(kind, label)
            assert client.status()["song"] == str(index), (label, client.status())
            report["transitions"].append({"mode": "next", "from": previous, "to": kind,
                                          "uri": song, **result})
            print(f"PASS {index + 1}/{len(plan)} {label}: {result['status']['audio']}", flush=True)
            previous = kind
        # Phase 3: natural end-of-song transitions near the end of each track.
        client.command("stop")
        client.command("clear")
        short = plan[:4]
        for kind, song in short:
            client.command("add", song)
        client.command("play", 0)
        previous = None
        for index, (kind, song) in enumerate(short):
            label = f"end-of-song {previous or 'start'} -> {kind}"
            result = check(kind, label)
            assert client.status()["song"] == str(index), (label, client.status())
            report["transitions"].append({"mode": "end-of-song", "from": previous, "to": kind,
                                          "uri": song, **result})
            print(f"PASS {index + 1}/{len(short)} {label}: {result['status']['audio']}", flush=True)
            if index + 1 < len(short):
                duration = float(client.status()["duration"])
                client.command("seekcur", max(0.0, duration - 3))
                deadline = time.monotonic() + 20
                while client.status().get("song") == str(index) and time.monotonic() < deadline:
                    time.sleep(0.1)
            previous = kind
        report["passed"] = True
    finally:
        client.command("stop")
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS: {len(report['transitions'])} transitions; {args.report}")


if __name__ == "__main__":
    main()

#!/bin/sh
set -eu

cd "$(dirname "$0")"
build_dir=${BUILD_DIR:-build/sacd-debug}
test -x "$build_dir/mpd" || { echo "Run ./mybuild.sh first" >&2; exit 1; }

python3 - "$build_dir" "${MUSIC_DIR:-$HOME/Music}" \
    "${AUDIO_DEVICE:-default}" "${MPD_PORT:-6601}" "${DSD_SWITCH_BRIDGE:-no}" \
    "${OUTPUT_NAME:-Local ALSA output}" <<'PY'
import json
from pathlib import Path
import sys

build, music, device, port, bridge, output_name = sys.argv[1:]
runtime = Path(build).resolve() / 'runtime'
music = Path(music).expanduser().resolve()
if not music.is_dir():
    raise SystemExit(f'Music directory does not exist: {music}')
port = int(port)
if not 1 <= port <= 65535:
    raise SystemExit('Invalid MPD_PORT')
if bridge not in ('yes', 'no'):
    raise SystemExit('DSD_SWITCH_BRIDGE must be yes or no')
(runtime / 'playlists').mkdir(parents=True, exist_ok=True)
q = lambda value: json.dumps(str(value), ensure_ascii=False)
config = f'''music_directory {q(music)}
playlist_directory {q(runtime / 'playlists')}
db_file {q(runtime / 'database')}
pid_file {q(runtime / 'pid')}
state_file {q(runtime / 'state')}
bind_to_address "127.0.0.1"
port "{port}"
bind_to_address {q(runtime / 'socket')}
zeroconf_enabled "no"
auto_update "no"
log_level "verbose"
replaygain "off"
decoder {{
    plugin "sacdiso"
    playable_area "stereo"
    use_stdio "yes"
}}
decoder {{
    plugin "dsdiff"
    playable_area "stereo"
    use_stdio "yes"
}}
audio_output {{
    type "alsa"
    name {q(output_name)}
    device {q(device)}
    mixer_type "none"
    dop "no"
    dsd_to_pcm_bridge "{bridge}"
    pcm_to_dsd_bridge "{bridge}"
    stop_dsd_silence "yes"
}}
audio_output {{
    type "recorder"
    name "PCM verification"
    encoder "wave"
    format "44100:16:2"
    path {q(runtime / 'capture.wav')}
    enabled "no"
}}
'''
(runtime / 'mpd.conf').write_text(config)
print(f'Local MPD: 127.0.0.1:{port}; music: {music}', file=sys.stderr)
PY

exec "$build_dir/mpd" --no-daemon --stderr "$build_dir/runtime/mpd.conf"

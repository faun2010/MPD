/*
 * Copyright (C) 2003-2025 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include <sacd_media.h>
#include <sacd_reader.h>
#include <sacd_disc.h>
#include <sacd_metabase.h>
#include <dst_decoder.h>
#include "SacdIsoDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "song/DetachedSong.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/FileSystem.hxx"
#include "thread/Cond.hxx"
#include "thread/Mutex.hxx"
#include "util/BitReverse.hxx"
#include "util/AllocatedString.hxx"
#include "uri/Extract.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <memory>
#include <vector>

static constexpr Domain sacdiso_domain("sacdiso");

namespace sacdiso {

constexpr auto SACD_TRACKXXX_FMT{ "%cC_AUDIO__TRACK%03u.%3s" };

bool        param_edited_master;
bool        param_single_track;
bool        param_lsbitfirst;
area_id_e   param_playable_area;
std::string param_tags_path;
bool        param_tags_with_iso;
bool        param_use_stdio;

// Database updates, tag scans and decoders run on different threads.  Keep
// their open files and reader positions independent.  Declaration order keeps
// the media alive until its reader has been destroyed at thread exit.
thread_local AllocatedPath                    sacd_path{ nullptr };
thread_local std::unique_ptr<sacd_media_t>    sacd_media;
thread_local std::unique_ptr<sacd_reader_t>   sacd_reader;
thread_local std::unique_ptr<sacd_metabase_t> sacd_metabase;

static unsigned
get_subsong(Path path_fs) {
	auto ptr = path_fs.GetBase().c_str();
	char area = '\0';
	unsigned index = 0;
	char suffix[4];
	auto params = std::sscanf(ptr, SACD_TRACKXXX_FMT, &area, &index, suffix);
	if (area == 'M') {
		index += sacd_reader->get_tracks(AREA_TWOCH);
	}
	index--;
	return (params == 3) ? index : 0;
}

static bool
container_update(Path path_fs) {
	if (path_fs.IsNull()) {
		return false;
	}
	auto curr_path = AllocatedPath(path_fs);
	if (!FileExists(curr_path)) {
		curr_path = path_fs.GetDirectoryName();
	}
	if (sacd_path == curr_path) {
		return true;
	}
	if (sacd_reader) {
		sacd_reader->close();
		sacd_reader.reset();
	}
	if (sacd_media) {
		sacd_media->close();
		sacd_media.reset();
	}
	sacd_metabase.reset();
	sacd_path.SetNull();
	if (FileExists(curr_path)) {
		if (param_use_stdio) {
			sacd_media = std::make_unique<sacd_media_file_t>();
		}
		else {
			sacd_media = std::make_unique<sacd_media_stream_t>();
		}
		if (!sacd_media) {
			LogError(sacdiso_domain, "new sacd_media_t() failed");
			return false;
		}
		auto suffix = curr_path.GetExtension();
		auto is_iso = StringIsEqualIgnoreCase(suffix, "dat") || StringIsEqualIgnoreCase(suffix, "iso");
		if (is_iso) {
			sacd_reader = std::make_unique<sacd_disc_t>();
		}
		if (!sacd_reader) {
			LogError(sacdiso_domain, "new sacd_disc_t() failed");
			return false;
		}
		if (!sacd_media->open(curr_path.c_str())) {
			std::string err;
			err  = "sacd_media->open('";
			err += curr_path.c_str();
			err += "') failed";
			LogWarning(sacdiso_domain, err.c_str());
			return false;
		}
		if (!sacd_reader->open(sacd_media.get(), param_single_track ? MODE_SINGLE_TRACK : MODE_MULTI_TRACK)) {
			//LogWarning(sacdiso_domain, "sacd_reader->open(...) failed");
			return false;
		}
		if (is_iso) {
			if (!param_tags_path.empty() || param_tags_with_iso) {
				std::string tags_file;
				if (param_tags_with_iso) {
					tags_file = curr_path.c_str();
					tags_file.resize(tags_file.rfind('.') + 1);
					tags_file.append("xml");
				}
				sacd_metabase = std::make_unique<sacd_metabase_t>(reinterpret_cast<sacd_disc_t*>(sacd_reader.get()), param_tags_path.empty() ? nullptr : param_tags_path.c_str(), tags_file.empty() ? nullptr : tags_file.c_str());
			}
		}
		sacd_path = curr_path;
	}
	return static_cast<bool>(sacd_reader);
}

static void
scan_info(unsigned track, unsigned track_index, TagHandler& handler) {
	auto tag_value = std::to_string(track + 1);
	handler.OnTag(TAG_TRACK, tag_value.c_str());
	handler.OnDuration(SongTime::FromS(sacd_reader->get_duration(track)));
	if (sacd_metabase) {
		sacd_metabase->get_track_info(track_index + 1, handler);
	}
	sacd_reader->get_info(track, handler);
	if (handler.WantPicture()) {
		auto has_albumart{ false };
		if (sacd_metabase) {
			has_albumart = sacd_metabase->get_albumart(handler);
		}
		if (!has_albumart) {
			static constexpr auto art_names = std::array {
				"cover.png",
				"cover.jpg",
				"cover.webp",
			};
			for (const auto art_name : art_names) {
				auto art_file = AllocatedPath::Build(sacd_path.GetDirectoryName(), art_name);
				try {
					Mutex mutex;
					auto is = InputStream::OpenReady(art_file.c_str(), mutex);
					if (is && is->KnownSize()) {
						std::unique_lock lock{mutex};
						std::vector<std::byte> art_data;
						art_data.resize(is->GetSize());
						is->ReadFull(lock, art_data);
						handler.OnPicture({}, art_data);
						break;
					}
				}
				catch (...) {
				}
			}
		}
	}
}

static bool
init(const ConfigBlock& block) {
	param_edited_master  = block.GetBlockValue("edited_master", false);
	param_single_track   = block.GetBlockValue("single_track", false);
	param_lsbitfirst     = block.GetBlockValue("lsbitfirst", false);
	auto playable_area = block.GetBlockValue("playable_area", nullptr);
	param_playable_area = AREA_BOTH;
	if (playable_area != nullptr) {
		if (strcmp(playable_area, "stereo") == 0) {
			param_playable_area = AREA_TWOCH;
		}
		if (strcmp(playable_area, "multichannel") == 0) {
			param_playable_area = AREA_MULCH;
		}
	}
	param_tags_path = block.GetBlockValue("tags_path", "");
	param_tags_with_iso = block.GetBlockValue("tags_with_iso", false);
	param_use_stdio = block.GetBlockValue("use_stdio", true);
	return true;
}

static void
finish() noexcept {
	sacd_metabase.reset();
	sacd_reader.reset();
	sacd_media.reset();
	sacd_path.SetNull();
}

static std::forward_list<DetachedSong>
container_scan(Path path_fs) {
	std::forward_list<DetachedSong> list;
	if (!container_update(path_fs)) {
		return list;
	}
	TagBuilder tag_builder;
	auto tail = list.before_begin();
	auto suffix = path_fs.GetExtension();
	auto twoch_count = sacd_reader->get_tracks(AREA_TWOCH);
	auto mulch_count = sacd_reader->get_tracks(AREA_MULCH);
	if (twoch_count > 0 && param_playable_area != AREA_MULCH) {
		sacd_reader->select_area(AREA_TWOCH);
		for (auto track = 0u; track < twoch_count; track++) {
			AddTagHandler handler(tag_builder);
			scan_info(track, track, handler);
			char track_name[64];
			std::sprintf(track_name, SACD_TRACKXXX_FMT, '2', track + 1, suffix);
			tail = list.emplace_after(
				tail,
				track_name,
				tag_builder.Commit()
			);
		}
	}
	if (mulch_count > 0 && param_playable_area != AREA_TWOCH) {
		sacd_reader->select_area(AREA_MULCH);
		for (auto track = 0u; track < mulch_count; track++) {
			AddTagHandler handler(tag_builder);
			scan_info(track, track + twoch_count, handler);
			char track_name[64];
			std::sprintf(track_name, SACD_TRACKXXX_FMT, 'M', track + 1, suffix);
			tail = list.emplace_after(
				tail,
				track_name,
				tag_builder.Commit()
			);
		}
	}
	return list;
}

static void
bit_reverse_buffer(uint8_t* p, uint8_t* end) {
	for (; p < end; ++p) {
		*p = uint8_t(BitReverse(std::byte(*p)));
	}
}

static void
file_decode(DecoderClient &client, Path path_fs) {
	if (!container_update(path_fs.GetDirectoryName())) {
		return;
	}

	auto track = get_subsong(path_fs);

	// initialize reader
	sacd_reader->set_emaster(param_edited_master);
	auto twoch_count = sacd_reader->get_tracks(AREA_TWOCH);
	auto mulch_count = sacd_reader->get_tracks(AREA_MULCH);
	if (track < twoch_count) {
		sacd_reader->select_area(AREA_TWOCH);
		if (!sacd_reader->select_track(track, AREA_TWOCH)) {
			LogError(sacdiso_domain, "cannot select track in stereo area");
			return;
		}
	}
	else {
		track -= twoch_count;
		if (track < mulch_count) {
			sacd_reader->select_area(AREA_MULCH);
			if (!sacd_reader->select_track(track, AREA_MULCH)) {
				LogError(sacdiso_domain, "cannot select track in multichannel area");
				return;
			}
		}
	}
	auto dsd_channels = sacd_reader->get_channels();
	auto dsd_samplerate = sacd_reader->get_samplerate();
	auto dsd_framerate = sacd_reader->get_framerate();
	std::vector<uint8_t> dsx_buf;

	// initialize decoder
	AudioFormat audio_format = CheckAudioFormat(dsd_samplerate / 8, SampleFormat::DSD, dsd_channels);
	SongTime songtime = SongTime::FromS(sacd_reader->get_duration(track));
	client.Ready(audio_format, true, songtime);

	// play
	dst_decoder_t dst_decoder;
	auto dst_decoder_initialized = false;
	auto frame_read = true;
	for (;;) {
		dsx_buf.resize(dsd_samplerate / 8 / dsd_framerate * dsd_channels);
		auto frame_size = dsx_buf.size();
		auto frame_type = FRAME_INVALID;
		frame_read = frame_read && sacd_reader->read_frame(dsx_buf.data(), &frame_size, &frame_type);
		if (frame_read) {
			dsx_buf.resize(frame_size);
			switch (frame_type) {
			case FRAME_DSD:
				break;
			case FRAME_DST:
				if (!dst_decoder_initialized) {
					if (dst_decoder.init(dsd_channels, dsd_samplerate / 8 / dsd_framerate) == 0) {
						dst_decoder_initialized = true;
					}
					else {
						LogError(sacdiso_domain, "dst_decoder_t.init() failed");
					}
				}
				break;
			default:
				dsx_buf.assign(frame_size, 0xAA);
				break;
			}
		}
		else {
			dsx_buf.resize(0);
		}
		if (dst_decoder_initialized) {
			dst_decoder.run(dsx_buf);
		}
		if (dsx_buf.empty()) {
			if (!frame_read) {
				break;
			}
		}
		else {
			if (param_lsbitfirst) {
				bit_reverse_buffer(dsx_buf.data(), dsx_buf.data() + dsx_buf.size());
			}
			auto kbit_rate = dsd_channels * dsd_samplerate / 1000;
			auto cmd = client.SubmitAudio(nullptr, std::span{ dsx_buf.data(), dsx_buf.size() }, kbit_rate);
			if (cmd == DecoderCommand::SEEK) {
				auto seconds = client.GetSeekTime().ToDoubleS();
				if (sacd_reader->seek(seconds)) {
					if (dst_decoder_initialized) {
						dst_decoder.flush();
					}
					frame_read = true;
					client.CommandFinished();
				}
				else {
					client.SeekError();
				}
				cmd = client.GetCommand();
			}
			if (cmd == DecoderCommand::STOP) {
				break;
			}
		}
	}
}

static bool
scan_file(Path path_fs, TagHandler& handler) noexcept {
	if (!container_update(path_fs.GetDirectoryName())) {
		return false;
	}
	auto track_index = get_subsong(path_fs);
	auto track = track_index;
	auto twoch_count = sacd_reader->get_tracks(AREA_TWOCH);
	auto mulch_count = sacd_reader->get_tracks(AREA_MULCH);
	if (track < twoch_count) {
		sacd_reader->select_area(AREA_TWOCH);
	}
	else {
		track -= twoch_count;
		if (track < mulch_count) {
			sacd_reader->select_area(AREA_MULCH);
		}
		else {
			LogError(sacdiso_domain, "subsong index is out of range");
			return false;
		}
	}
	scan_info(track, track_index, handler);
	return true;
}

static const char* const suffixes[] {
	"dat",
	"iso",
	nullptr
};

}

constexpr DecoderPlugin sacdiso_decoder_plugin =
	DecoderPlugin("sacdiso", sacdiso::file_decode, sacdiso::scan_file)
	.WithInit(sacdiso::init, sacdiso::finish)
	.WithContainer(sacdiso::container_scan)
	.WithSuffixes(sacdiso::suffixes);

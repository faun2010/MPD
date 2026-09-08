/*
* DVD-Audio Decoder plugin
* Copyright (c) 2009-2024 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
*
* DVD-Audio Decoder is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* DVD-Audio Decoder is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "audio_track.h"
#include "mlp_audio_stream.h"
#include "pcm_audio_stream.h"

bool audio_track_t::check_chmode(chmode_e chmode, bool downmix) {
	int channels = audio_stream_info.group1_channels + audio_stream_info.group2_channels;
	switch (chmode) {
	case CHMODE_TWOCH:
		return channels <= 2 || (downmix && audio_stream_info.can_downmix);
		break;
	case CHMODE_MULCH:
		return channels > 2 && (!downmix);
		break;
	default:
		break;
	}
	return true;
}

int track_list_t::get_track_index(int titleset, int title, int track, bool downmix) {
	return (((titleset & 0xff) + 1) << 16) | (((title + 1) & 0xff) << 8) | ((track + 1) & 0xff) | (downmix ? 0x01000000 : 0); 
}

int track_list_t::get_track_index(int track_index) {
	for (unsigned int i = 0; i < track_list.size(); i++) {
		if (track_index == track_list[i].track_index)
			return i;
	}
	return -1;
}

void track_list_t::init(dvda_zone_t& dvda_zone, bool downmix, chmode_e chmode, double threshold_time, dvda_metabase_t* dvda_metabase, bool no_untagged) {
	int track_number = 1;
	for (auto ts = 0u; ts < dvda_zone.get_titlesets().size(); ts++) {
		auto dvda_titleset = dvda_zone.get_titleset(ts);
		if (dvda_titleset.is_audio_ts()) {
			for (auto ti = 0u; ti < dvda_titleset.get_titles().size(); ti++) {
				auto dvda_title = dvda_titleset.get_title(ti);
				for (auto tr = 0u; tr < dvda_title.get_tracks().size(); tr++) {
					auto dvda_track = dvda_title.get_track(tr);
					audio_track_t audio_track;
					audio_track.dvda_titleset = ts + 1;
					audio_track.dvda_title    = ti + 1;
					audio_track.dvda_track    = tr + 1;
					audio_track.track_number  = track_number;
					audio_track.block_first   = dvda_track.get_first();
					audio_track.block_last    = dvda_track.get_last();
					audio_track.duration      = dvda_track.get_time();
					if (!(audio_track.duration < threshold_time) && get_audio_stream_info(dvda_zone, ts, audio_track.block_first, audio_track.audio_stream_info)) {
						audio_track.track_downmix = downmix;
						audio_track.track_index = get_track_index(ts, ti, tr, audio_track.track_downmix);
						auto add_track = false;
						if (!audio_track.track_downmix) {
							add_track = true;
						}
						else {
							if (audio_track.audio_stream_info.stream_id == PCM_STREAM_ID) {
								int downmix_matrix = dvda_track.get_downmix_matrix();
								if (downmix_matrix >= 0) {
									for (int ch = 0; ch < DOWNMIX_CHANNELS; ch++) {
										audio_track.LR_dmx_coef[ch][0] = dvda_titleset.get_downmix_coef(downmix_matrix, ch, 0);
										audio_track.LR_dmx_coef[ch][1] = dvda_titleset.get_downmix_coef(downmix_matrix, ch, 1);
									}
									audio_track.audio_stream_info.can_downmix = true;
								}
							}
							if (audio_track.audio_stream_info.can_downmix) {
								if (audio_track.check_chmode(chmode, downmix)) {
									add_track = true;
								}
							}
						}
						if (add_track && dvda_metabase && no_untagged) {
							NullTagHandler handler(TagHandler::WANT_TAG);
							dvda_metabase->get_track_info(audio_track.track_index, downmix, handler);
							auto tag_exists = handler.WantTag(); // non funtional
							add_track = tag_exists;
						}
						if (add_track) {
							add(audio_track);
							track_number++;
						}
					}
				}
			}
		}
	}
}

bool track_list_t::get_audio_stream_info(dvda_zone_t& dvda_zone, int titleset, uint32_t block_no, audio_stream_info_t& audio_stream_info) {
	uint8_t block[SEGMENT_HEADER_BLOCKS * DVD_BLOCK_SIZE];
 	int blocks_read = (int)dvda_zone.get_blocks(titleset, block_no, SEGMENT_HEADER_BLOCKS, block); 
	uint8_t ps1_buffer[SEGMENT_HEADER_SIZE];
	auto bytes_written{ 0 };
	auto ok{ false };
	sub_header_t ps1_info;
	dvda_block_t::get_ps1(block, blocks_read, ps1_buffer, &bytes_written, &ps1_info);
	auto stream_id = stream_id_e(ps1_info.header.stream_id);
	if (stream_id) {
		auto audio_stream = audio_stream_t::create_stream(stream_id);
		if (audio_stream) {
			switch (stream_id) {
			case MLP_STREAM_ID:
				audio_stream_info = audio_stream->get_info(ps1_buffer, bytes_written);
				ok = true;
				break;
			case PCM_STREAM_ID:
				audio_stream_info = audio_stream->get_info((uint8_t*)&ps1_info.extra_header, ps1_info.header.extra_header_length);
				ok = true;
				break;
			default:
				break;
			}
		}
	}
	return ok;
}

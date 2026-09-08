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

#ifndef _AUDIO_TRACK_H_INCLUDED
#define _AUDIO_TRACK_H_INCLUDED

#include "dvda_zone.h"
#include "dvda_metabase.h"

class audio_track_t {
public:
	int                 dvda_titleset;
	int                 dvda_title;
	int                 dvda_track;
	int                 track_index;
	int                 track_number;
	uint32_t            block_first;
	uint32_t            block_last;
	double              duration;
	bool                track_downmix;
	double              LR_dmx_coef[DOWNMIX_CHANNELS][2];
	audio_stream_info_t audio_stream_info;
	bool check_chmode(chmode_e chmode, bool downmix);
};

class track_list_t {
	std::vector<audio_track_t> track_list;
public:
	static int get_track_index(int titleset, int title, int track, bool downmix);
	int size() const {
		return (int)track_list.size();
	}
	void clear() {
		track_list.clear();
	}
	void add(const audio_track_t audio_track) {
		track_list.push_back(audio_track);
	}
	audio_track_t get_track_by_index(int track_index) {
		return track_list[track_index];
	}
	int get_track_index(int track_index);
	void init(dvda_zone_t& dvda_zone, bool downmix = false, chmode_e chmode = CHMODE_BOTH, double threshold_time = 0, dvda_metabase_t* dvda_metabase = nullptr, bool no_untagged = false);
	bool get_audio_stream_info(dvda_zone_t& dvda_zone, int titleset, uint32_t block_no, audio_stream_info_t& audio_stream_info);
};

#endif

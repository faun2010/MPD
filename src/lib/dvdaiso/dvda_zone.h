/*
* DVD-Audio Decoder plugin
* Copyright (c) 2009-2025 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
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

#pragma once

#include "ifo.h"
#include "dvda_block.h"
#include "dvda_error.h"
#include "dvda_filesystem.h"
#include <stdint.h>
#include <array>
#include <vector>

enum class dvda_titleset_e{ DVDTitlesetUnknown, DVDTitlesetAudio, DVDTitlesetVideo };

class dvda_sector_pointer_t;
class dvda_track_t;
class dvda_title_t;
class dvda_titleset_t;
class dvda_zone_t;

class dvda_object_t {
};

class aob_object_t : public dvda_object_t {
public:
	double get_time() const { return 0.0; }
	uint32_t get_length_pts() const { return 0; }
	uint32_t get_first() const { return 0; }
	uint32_t get_last() const { return 0; }
};

class dvda_sector_pointer_t : public aob_object_t {
	dvda_track_t& dvda_track;
	int           index;
	uint32_t      first;
	uint32_t      last;
public:
	dvda_sector_pointer_t(dvda_track_t& p_dvda_track, ats_track_sector_t& p_ats_track_sector, int sp_index);
	double get_time();
	uint32_t get_length_pts();
	uint32_t get_index() const {
		return index; 
	}
	uint32_t get_first() const {
		return first; 
	}
	uint32_t get_last() const {
		return last; 
	}
};

class dvda_track_t : public aob_object_t {
	std::vector<dvda_sector_pointer_t> dvda_sector_pointers;
	int index;
	int track;
	uint32_t first_pts;
	uint32_t length_pts;
	int downmix_matrix;
public:
	dvda_track_t(ats_track_timestamp_t& p_ats_track_timestamp, int p_track_no);
	std::vector<dvda_sector_pointer_t>& get_sector_pointers() {
		return dvda_sector_pointers;
	}
	dvda_sector_pointer_t& get_sector_pointer(size_t p_index) {
		return dvda_sector_pointers[p_index];
	}
	uint32_t get_index() const {
		return index; 
	}
	int get_track() const {
		return track; 
	}
	uint32_t get_length_pts() const {
		return length_pts;
	}
	int get_downmix_matrix() const {
		return downmix_matrix;
	}
	double get_time() const;
	uint32_t get_first();
	uint32_t get_last();
};

class dvda_title_t : public dvda_object_t {
	std::vector<dvda_track_t> dvda_tracks;
	int                       title;
	uint32_t                  length_pts;
	int                       indexes;
	int                       tracks;
public:
	dvda_title_t(ats_title_t* p_ats_title, ats_title_idx_t* p_ats_title_idx);
	std::vector<dvda_track_t>& get_tracks() {
		return dvda_tracks;
	}
	dvda_track_t& get_track(size_t track) {
		return dvda_tracks[track];
	}
	int get_title() const {
		return title; 
	}
	double get_time() const;
};

class dvda_aob_t {
public:
	char                file_name[13];
	uint32_t            block_first;
	uint32_t            block_last;
	dvda_fileobject_ptr dvda_fileobject;
	dvda_aob_t() {
		file_name[0] = '\0';
		block_first = block_last = 0;
	}
};

class dvda_downmix_channel_t {
public:
	bool    inv_phase;
	uint8_t coef;
};

class dvda_downmix_matrix_t {
	dvda_downmix_channel_t LR_dmx[DOWNMIX_CHANNELS][2];
public:
	dvda_downmix_channel_t* get_downmix_channel(int channel, int dmx_channel);
	double get_downmix_coef(int channel, int dmx_channel);
};

class dvda_titleset_t : public dvda_object_t {
	dvda_zone_t&              dvda_zone;
	size_t                    dvda_titleset;
	std::vector<dvda_title_t> dvda_titles;

	bool                      is_open;
	dvda_titleset_e           dvda_titleset_type;
	dvda_aob_t                aobs[9];
	dvda_downmix_matrix_t     downmix_matrices[DOWNMIX_MATRICES];
	uint32_t                  aobs_last_sector;
public:
	dvda_titleset_t(dvda_zone_t& zone);
	~dvda_titleset_t();
	std::vector<dvda_title_t>& get_titles() {
		return dvda_titles;
	}
	dvda_title_t& get_title(size_t title) {
		return dvda_titles[title];
	}
	uint32_t get_last() {
		return aobs_last_sector; 
	}
	int get_titleset() {
		return (int)dvda_titleset;
	}
	bool is_audio_ts() {
		return dvda_titleset_type == dvda_titleset_e::DVDTitlesetAudio;
	}
	bool is_video_ts() {
		return dvda_titleset_type == dvda_titleset_e::DVDTitlesetVideo;
	}
	double get_downmix_coef(int matrix, int channel, int dmx_channel) {
		if (matrix >= 0 && matrix < DOWNMIX_MATRICES)
			return downmix_matrices[matrix].get_downmix_coef(channel, dmx_channel);
		return 0.0;
	}
	bool open() {
		return is_open;
	}
	bool open(size_t titleset_index);
	DVDAERROR get_block(uint32_t block_no, uint8_t* buf_ptr);
	size_t get_blocks(uint32_t block_first, uint32_t block_last, uint8_t* buf_ptr);
	void close_aobs();
};

class dvda_zone_t : public dvda_object_t {
	dvda_filesystem_t&           dvda_filesystem;
	std::vector<dvda_titleset_t> dvda_titlesets;
	size_t                       audio_titlesets;
	size_t                       video_titlesets;
public:
	dvda_zone_t(dvda_filesystem_t& filesystem) : dvda_filesystem(filesystem), audio_titlesets(0), video_titlesets(0) {
	}
	dvda_filesystem_t& get_filesystem() {
		return dvda_filesystem;
	}
	std::vector<dvda_titleset_t>& get_titlesets() {
		return dvda_titlesets;
	}
	dvda_titleset_t& get_titleset(size_t titleset) {
		return dvda_titlesets[titleset];
	}
	bool open();
	void close();
	DVDAERROR get_block(size_t titleset, uint32_t block_no, uint8_t* buf_ptr);
	size_t get_blocks(size_t titleset, uint32_t block_no, size_t blocks, uint8_t* buf_ptr);
};

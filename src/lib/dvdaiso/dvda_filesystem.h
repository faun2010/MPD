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

#include "dvda_media.h"
#include "udf/dvd_input.h"
#include "udf/dvd_udf.h"
#include <cstdint>

class dvda_fileobject_t;
typedef std::shared_ptr<dvda_fileobject_t> dvda_fileobject_ptr;

class dvda_fileobject_t {
	friend class dvda_filesystem_t;
	dvda_media_t* media_ref;
	bool media_close;
	int64_t base;
	int64_t size;
public:
	dvda_fileobject_t();
	~dvda_fileobject_t();
	size_t read(void* buffer, size_t count);
	bool seek(int64_t offset);
	int64_t get_size() const;
};

class dvda_filesystem_t {
	std::string   fs_path;
	dvda_media_t* fs_media;
	dvd_reader_t* fs_reader;
	bool          mounted;
public:
	dvda_filesystem_t();
	~dvda_filesystem_t();
	bool is_mounted() {
		return mounted;
	}
	bool mount(const char* path);
	bool mount(dvda_media_t* media);
	void unmount();
	bool get_name(char* name);
	dvda_fileobject_ptr open(const char* file_name);
};

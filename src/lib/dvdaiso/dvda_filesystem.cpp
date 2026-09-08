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

#include "dvda_filesystem.h"
#include <filesystem>
#include <string>
#include <typeinfo>
#include <utility>

dvda_fileobject_t::dvda_fileobject_t() :media_ref(nullptr), media_close(false), base(0), size(0) {
}

dvda_fileobject_t::~dvda_fileobject_t() {
	if (media_close) {
		media_ref->close();
	}
}

size_t dvda_fileobject_t::read(void* buffer, size_t count) {
	return media_ref ? media_ref->read(buffer, count) : 0;
}

bool dvda_fileobject_t::seek(int64_t offset) {
	return (media_ref && (offset < size)) ? media_ref->seek(base + offset) : false;
}

int64_t dvda_fileobject_t::get_size() const {
	return size;
}

dvda_filesystem_t::dvda_filesystem_t() : fs_media(nullptr), fs_reader(nullptr), mounted(false) {
}

dvda_filesystem_t::~dvda_filesystem_t() {
	unmount();
}

bool dvda_filesystem_t::mount(const char* path) {
	fs_path = path;
	mounted = true;
	return mounted;
}

bool dvda_filesystem_t::mount(dvda_media_t* media) {
	fs_media = media;
	fs_reader = DVDOpen(fs_media);
	if (fs_reader) {
		mounted = true;
	}
	return mounted;
}

bool dvda_filesystem_t::get_name(char* name) {
	return UDFGetVolumeIdentifier(fs_reader, name, 32) > 0;
}

void dvda_filesystem_t::unmount() {
	fs_path.clear();
	if (fs_reader) {
		DVDClose(fs_reader);
		fs_reader = nullptr;
		fs_media = nullptr;
	}
	mounted = false;
}

dvda_fileobject_ptr dvda_filesystem_t::open(const char* file_name) {
	dvda_fileobject_ptr fo;
	if (fs_reader) {
		std::string file_path("/AUDIO_TS/");
		file_path += file_name;
		uint32_t file_size;
		auto lba = UDFFindFile(fs_reader, (char*)(file_path.data()), &file_size);
		if (lba) {
			fo = std::make_shared<dvda_fileobject_t>();
			if (fo) {
				fo->media_ref = fs_media;
				fo->media_close = false;
				fo->base = 2048 * (uint64_t)lba;
				fo->size = (uint64_t)file_size;
				fo->seek(0);
			}
		}
		else {
			fo.reset();
		}
	}
	else {
		auto file_path{ fs_path };
		file_path += std::filesystem::path::preferred_separator;
		file_path += file_name;
		auto dvda_file = new dvda_media_file_t();
		if (dvda_file) {
			if (dvda_file->open(file_path.data())) {
				fo = std::make_shared<dvda_fileobject_t>();
				if (fo) {
					fo->media_ref = dvda_file;
					fo->media_close = true;
					fo->base = 0;
					fo->size = dvda_file->get_size();
					fo->seek(0);
				}
			}
			else {
				delete dvda_file;
			}
		}
	}
	return fo;
}

/*
* MPD DVD-Audio Decoder plugin
* Copyright (c) 2014-2025 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
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

#include <string>
#include <upnp/ixml.h>

#include "config.h"
#include "tag/Handler.hxx"

class dvda_disc_t;

class dvda_metabase_t {
	dvda_disc_t*   disc;
	std::string    store_id;
	std::string    store_path;
	std::string    store_file;
	std::string    xml_file;
	IXML_Document* xml_doc;
	bool           initialized;
public:
	dvda_metabase_t(dvda_disc_t* dvda_disc, const char* tags_path = nullptr, const char* tags_file = nullptr);
	~dvda_metabase_t();
	bool get_track_info(unsigned track_number, bool downmix, TagHandler& handler);
	bool get_albumart(TagHandler& handler);
private:
	bool init_xmldoc();
	IXML_Node* get_node(const char* tag_name, const char* att_id);
	std::string track_number_to_id(unsigned track_number);
};

/*
* MPD DVD-Audio Decoder plugin
* Copyright (c) 2020-2023 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
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

#include "config.h"
#include <string.h>
#include <unistd.h>
#include "dvda_metabase.h"
#include "dvda_disc.h"
#include "lib/crypto/Base64.hxx"
#include "lib/crypto/MD5.hxx"
#include "util/ASCII.hxx"
#include "tag/Handler.hxx"
#include "tag/Type.hxx"
#include "tag/Names.hxx"
#include <vector>

auto MB_TAG_ROOT       = "root";
auto MB_TAG_STORE      = "store";
auto MB_TAG_TRACK      = "track";
auto MB_TAG_INFO       = "info";
auto MB_TAG_META       = "meta";
auto MB_TAG_REPLAYGAIN = "replaygain";
auto MB_TAG_ALBUMART   = "albumart";

auto MB_ATT_ID      = "id";
auto MB_ATT_NAME    = "name";
auto MB_ATT_TYPE    = "type";
auto MB_ATT_VALUE   = "value";
auto MB_ATT_VALSEP  = ";";
auto MB_ATT_VERSION = "version";

auto METABASE_TYPE    = "DVDA";
auto METABASE_VERSION = "1.2";

static auto utf2xml = [](auto src) {
	auto dst{ std::string() };
	for (auto i = 0; src[i] != 0; i++) {
		if (src[i] == '\r') {
			dst += "&#13;";
		}
		else if (src[i] == '\n') {
			dst += "&#10;";
		}
		else {
			dst += string(&src[i], 1);
		}
	}
	return dst;
};

static auto xml2utf = [](auto src) {
	auto dst{ std::string() };
	for (auto i = 0; src[i] != 0; i++) {
		if (strncmp(&src[i], "&#13;", 5) == 0) {
			dst += "\r";
			i += 4;
		}
		else if (strncmp(&src[i], "&#10;", 5) == 0) {
			dst += "\n";
			i += 4;
		}
		else {
			dst += std::string(&src[i], 1);
		}
	}
	return dst;
};

static std::string get_md5(dvda_filesystem_t& filesystem) {
	auto md5_string{ std::string() };
	auto tag_file = filesystem.open("AUDIO_TS.IFO");
	if (tag_file) {
		auto tag_size = size_t(tag_file->get_size());
		std::vector<std::byte> tag_data;
		tag_data.resize(tag_size);
		if (tag_file->read(tag_data.data(), tag_size) == tag_size) {
			GlobalInitMD5();
			for (auto md5_hash_value : MD5(tag_data)) {
				char hex_byte[3];
				sprintf(hex_byte, "%02X", (uint8_t)md5_hash_value);
				md5_string += hex_byte;
			}
		}
	}
	return md5_string;
}

dvda_metabase_t::dvda_metabase_t(dvda_disc_t* dvda_disc, const char* tags_path, const char* tags_file) {
	disc = dvda_disc;
	initialized = false;
	store_id = get_md5(*dvda_disc->get_filesystem());
	if (store_id.empty()) {
		return;
	}
	if (tags_path) {
		store_path = tags_path;
		store_file = store_path;
		store_file += "/";
		store_file += store_id;
		store_file += ".xml";
		if (access(store_file.c_str(), F_OK) == 0) {
			if (tags_file && access(tags_file, F_OK) == -1) {
				auto s = fopen(store_file.c_str(), "rb");
				auto d = fopen(tags_file, "wb");
				if (s) {
					if (d) {
						char buf[64];
						size_t size;
						while ((size = fread(buf, 1, sizeof(buf), s)) > 0) {
							fwrite(buf, 1, size, d);
						}
						fclose(d);
					}
					fclose(s);
				}
			}
		}
	}
	xml_file = tags_file ? tags_file : store_file;
	initialized = init_xmldoc();
}

dvda_metabase_t::~dvda_metabase_t() {
	if (xml_doc) {
		ixmlDocument_free(xml_doc);
	}
}

bool dvda_metabase_t::get_track_info(unsigned track_number, bool downmix, TagHandler& handler) {
	if (!initialized) {
		return false;
	}
	auto node_track {get_node(MB_TAG_TRACK, track_number_to_id(track_number).c_str())};
	if (!node_track) {
		return false;
	}
	auto list_tags {ixmlNode_getChildNodes(node_track)};
	if (!list_tags) {
		return false;
	}
	for (auto tag_index = 0u; tag_index < ixmlNodeList_length(list_tags); tag_index++) {
		IXML_Node* node_tag {ixmlNodeList_item(list_tags, tag_index)};
		if (node_tag) {
			std::string node_name = ixmlNode_getNodeName(node_tag);
			if (node_name == MB_TAG_META) {
				auto attr_tag {ixmlNode_getAttributes(node_tag)};
				if (attr_tag) {
					std::string tag_name;
					std::string tag_value;
					auto att_name {ixmlNamedNodeMap_getNamedItem(attr_tag, MB_ATT_NAME)};
					if (att_name) {
						tag_name = ixmlNode_getNodeValue(att_name);
					}
					auto att_value = ixmlNamedNodeMap_getNamedItem(attr_tag, MB_ATT_VALUE);
					if (att_value) {
						tag_value = xml2utf(ixmlNode_getNodeValue(att_value));
					}
					if (tag_name.length() > 0) {
						TagType tag_type = TAG_NUM_OF_ITEM_TYPES;
						for (auto i = 0; i < TAG_NUM_OF_ITEM_TYPES; i++) {
							if (StringEqualsCaseASCII(tag_item_names[i], tag_name.c_str())) {
								tag_type = static_cast<TagType>(i);
								break;
							}
						}
						if (tag_type != TAG_NUM_OF_ITEM_TYPES) {
							if (downmix) {
								if (tag_type == TAG_TITLE) {
									tag_value += " (stereo downmix)";
								}
							}
							handler.OnTag(tag_type, {tag_value.c_str(), tag_value.size()});
						}
					}
				}
			}
		}
	}
	return true;
}

bool dvda_metabase_t::get_albumart(TagHandler& handler) {
	if (!initialized) {
		return false;
	}
	IXML_Node* node_albumart = nullptr;
	for (auto& att_id : {"3", "4", "6", "2", "8"}) {
		node_albumart = get_node(MB_TAG_ALBUMART , att_id);
		if (node_albumart) {
			break;
		}
	}
	if (!node_albumart) {
		return false;
	}
	auto node_cdata {ixmlNode_getFirstChild(node_albumart)};
	if (!node_cdata) {
		return false;
	}
	auto cdata_value {ixmlNode_getNodeValue(node_cdata)};
	if (!cdata_value) {
		return false;
	}
	auto debase64_size = CalculateBase64OutputSize(strlen(cdata_value));
	std::vector<std::byte> debase64_data(debase64_size);
	debase64_size =	DecodeBase64(debase64_data, cdata_value);
	handler.OnPicture({}, debase64_data);
	return true;
}

bool dvda_metabase_t::init_xmldoc() {
	xml_doc = ixmlLoadDocument(xml_file.c_str());
	return xml_doc != nullptr;
}

IXML_Node* dvda_metabase_t::get_node(const char* tag_name, const char* att_id) {
	IXML_NodeList* list_item = nullptr;
	IXML_Node* node_item_id = nullptr;
	auto node_root {ixmlNodeList_item(ixmlDocument_getElementsByTagName(xml_doc, MB_TAG_ROOT), 0)};
	if (node_root) {
		auto list_store {ixmlNode_getChildNodes(node_root)};
		if (list_store) {
			for (auto i = 0u; i < ixmlNodeList_length(list_store); i++) {
				auto node_store {ixmlNodeList_item(list_store, i)};
				if (node_store) {
					auto attr_store {ixmlNode_getAttributes(node_store)};
					if (attr_store) {
						IXML_Node* node_attr;
						std::string attr_id;
						std::string attr_type;
						node_attr = ixmlNamedNodeMap_getNamedItem(attr_store, MB_ATT_ID);
						if (node_attr) {
							attr_id = ixmlNode_getNodeValue(node_attr);
						}
						node_attr = ixmlNamedNodeMap_getNamedItem(attr_store, MB_ATT_TYPE);
						if (node_attr) {
							attr_type = ixmlNode_getNodeValue(node_attr);
						}
						if (attr_id == store_id) {
							list_item = ixmlNode_getChildNodes(node_store);
							break;
						}
					}
				}
			}
		}
	}
	if (list_item) {
		for (auto i = 0u; i < ixmlNodeList_length(list_item); i++) {
			auto node_item {ixmlNodeList_item(list_item, i)};
			if (node_item) {
				std::string node_name {ixmlNode_getNodeName(node_item)};
				if (node_name == tag_name) {
					auto attr_item {ixmlNode_getAttributes(node_item)};
					if (attr_item) {
						auto node_attr {ixmlNamedNodeMap_getNamedItem(attr_item, MB_ATT_ID)};
						if (node_attr) {
							std::string attr_value = ixmlNode_getNodeValue(node_attr);
							if (attr_value == att_id) {
								node_item_id = node_item;
								break;
							}
						}
					}
				}
			}
		}
	}
	return node_item_id;
}

std::string dvda_metabase_t::track_number_to_id(unsigned track_number) {
	std::string track_id;
	auto audio_track = disc->get_track(track_number - 1);
	track_id += std::to_string(audio_track.dvda_titleset);
	track_id += ".";
	track_id += std::to_string(audio_track.dvda_title);
	track_id += ".";
	track_id += std::to_string(audio_track.dvda_track);
	return track_id;
}

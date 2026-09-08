// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "Walk.hxx"
#include "UpdateDomain.hxx"
#include "song/DetachedSong.hxx"
#include "db/DatabaseLock.hxx"
#include "db/plugins/simple/Directory.hxx"
#include "db/plugins/simple/Song.hxx"
#include "storage/StorageInterface.hxx"
#include "decoder/DecoderPlugin.hxx"
#include "decoder/DecoderList.hxx"
#include "fs/AllocatedPath.hxx"
#include "storage/FileInfo.hxx"
#include "Log.hxx"

#include <list>

bool
UpdateWalk::UpdateContainerFile(Directory &directory,
				std::string_view name, std::string_view suffix,
				const StorageFileInfo &info) noexcept
{
	std::list<const DecoderPlugin *> plugins;
	for (unsigned i = 0; decoder_plugins[i] != nullptr; ++i)
		if (decoder_plugins_enabled[i] && decoder_plugins[i]->SupportsContainerSuffix(suffix))
			plugins.push_back(decoder_plugins[i]);

	if (plugins.empty())
		return false;

	Directory *contdir;
	{
		const ScopeDatabaseLock protect;
		contdir = MakeVirtualDirectoryIfModified(directory, name,
							 info,
							 DEVICE_CONTAINER);
		if (contdir == nullptr)
			/* not modified */
			return true;
	}

	const auto pathname = storage.MapFS(contdir->GetPath());
	if (pathname.IsNull()) {
		/* not a local file: skip, because the container API
		   supports only local files */
		editor.LockDeleteDirectory(contdir);
		return false;
	}

	auto track_count{ 0 };
	for (auto plugin : plugins) {
		try {
			auto v = plugin->container_scan(pathname);
			if (v.empty()) {
				continue;
			}

			for (auto &vtrack : v) {
				auto song = std::make_unique<Song>(std::move(vtrack),
				*contdir);

				// shouldn't be necessary but it's there..
				song->mtime = info.mtime;

				FmtNotice(update_domain, "added {}/{}",
						contdir->GetPath(),
						song->filename);

				{
					const ScopeDatabaseLock protect;
					contdir->AddSong(std::move(song));
					track_count++;
				}

				modified = true;
			}
		}	catch (...) {
			LogError(std::current_exception());
		}
	}

	if (track_count == 0) {
		editor.LockDeleteDirectory(contdir);
		return false;
	}

	return true;
}

#pragma once

#include "Strata/Core/Base.h"

#include <chrono>
#include <filesystem>

namespace Strata
{

	// Ends a FileWatcher thread's wait early when something in a directory tree changes, where the platform reports such
	// changes (engine-internal; implemented per platform in Platform/<OS>/). The watcher still rescans the tree to find
	// out what changed, so a notification may be spurious or cover several changes.
	class FileChangeNotifier
	{
	public:
		// Null when the platform reports no changes for `directory` (the watcher then polls). A platform that does, but
		// cannot watch this directory, logs a warning.
		static Scope<FileChangeNotifier> Create(const std::filesystem::path& directory, bool recursive);

		virtual ~FileChangeNotifier() = default;

		// Blocks until a change is reported, Wake() was called or `timeout` has passed.
		virtual void Wait(std::chrono::milliseconds timeout) = 0;
		// Ends the current Wait and every later one at once (the watcher is stopping). May be called from any thread.
		virtual void Wake() = 0;
	};

}

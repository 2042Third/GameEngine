#include "stpch.h"
#include "Strata/Core/FileChangeNotifier.h"

namespace Strata
{

	// Linux and macOS can report changes too, but not with one handle for a whole tree: inotify needs a watch per
	// directory, kept up to date as directories come and go, and FSEvents a dispatch queue or run loop. The watcher polls
	// there instead.
	Scope<FileChangeNotifier> FileChangeNotifier::Create(const std::filesystem::path&, bool)
	{
		return nullptr;
	}

}

#pragma once

#include <Strata/Core/Base.h>

#include <cstdint>
#include <filesystem>

namespace Strata
{

	class EditorContext;
	class TextureReadback;

	// The picture of a project the launcher shows on its card: after each scene save, the scene from its primary camera (the
	// editor camera without one) at c_Width x c_Height, rendered by the viewport's capture renderer on the next frames (until
	// nothing is loading, at most c_MaxFrames), read back without stalling (TextureReadback) and written as PNG on an I/O job
	// to <project>/.strata/Thumbnail.png (GetFile). Main thread; nothing happens without a GPU.
	class ProjectThumbnail
	{
	public:
		static constexpr uint32_t c_Width = 480;
		static constexpr uint32_t c_Height = 270;
		// Frames rendered while assets still load before the picture is taken as it is.
		static constexpr uint32_t c_MaxFrames = 120;

		// Where a project keeps its thumbnail (its intermediate directory).
		static std::filesystem::path GetFile(const std::filesystem::path& projectDirectory);

		ProjectThumbnail();
		~ProjectThumbnail();

		ProjectThumbnail(const ProjectThumbnail&) = delete;
		ProjectThumbnail& operator=(const ProjectThumbnail&) = delete;

		// After a save: the thumbnail is made on the next frames (a request while one is being made is made after it).
		void Request();
		// Drops the picture being made (the project closes).
		void Cancel();
		// Once per frame.
		void Update(EditorContext& context);
		// Whether a picture is being made or waits to be; how many were written (tests).
		bool IsPending() const;
		uint64_t GetWrittenCount() const;
	private:
		struct WriteState;

		bool m_Requested = false;
		bool m_RequestedAgain = false;
		uint32_t m_Frames = 0;
		Scope<TextureReadback> m_Readback;
		std::filesystem::path m_File;
		Ref<WriteState> m_Write;
	};

}

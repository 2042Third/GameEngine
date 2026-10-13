#pragma once

#include <Strata/Core/Base.h>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace Strata
{

	class EditorContext;
	class TextureReadback;
	struct ReadbackImage;

	// The picture of a project the launcher shows on its card: after each scene save, the scene from its primary camera (the
	// editor camera without one), rendered by the viewport's capture renderer on the next frames at c_RenderScale times
	// c_Width x c_Height, the size of a game's window, so that the game's screen-space text, sized in pixels, keeps its
	// share of the picture (until nothing is loading and its textures have the levels the view wants, at most c_MaxFrames),
	// read back without stalling (TextureReadback), averaged down to c_Width x c_Height (Downscale) and written as PNG on an
	// I/O job to <project>/.strata/Thumbnail.png (GetFile). Main thread; nothing happens without a GPU.
	class ProjectThumbnail
	{
	public:
		static constexpr uint32_t c_Width = 480;
		static constexpr uint32_t c_Height = 270;
		static constexpr uint32_t c_RenderScale = 3; // Rendered at 1440x810
		// Frames rendered while assets still load before the picture is taken as it is.
		static constexpr uint32_t c_MaxFrames = 120;

		// Where a project keeps its thumbnail (its intermediate directory).
		static std::filesystem::path GetFile(const std::filesystem::path& projectDirectory);
		// An RGBA8 image averaged over blocks of `factor` x `factor` pixels (the box filter of an exact downscale); nothing
		// for other formats or sizes that are not multiples of the factor.
		static std::optional<ReadbackImage> Downscale(const ReadbackImage& image, uint32_t factor);

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

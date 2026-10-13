#include "Editor/ProjectThumbnail.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/ViewportRenderer.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Project/Project.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Renderer/TextureReadback.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	struct ProjectThumbnail::WriteState
	{
		std::atomic<uint32_t> Running = 0;
		std::atomic<uint64_t> Written = 0;
	};

	std::filesystem::path ProjectThumbnail::GetFile(const std::filesystem::path& projectDirectory)
	{
		return projectDirectory / ".strata" / "Thumbnail.png";
	}

	std::optional<ReadbackImage> ProjectThumbnail::Downscale(const ReadbackImage& image, uint32_t factor)
	{
		if (factor == 0 || image.BytesPerPixel != 4 || image.Width % factor != 0 || image.Height % factor != 0
			|| image.Pixels.size() != static_cast<size_t>(image.Width) * image.Height * 4)
			return std::nullopt;
		ReadbackImage result;
		result.Width = image.Width / factor;
		result.Height = image.Height / factor;
		result.Format = image.Format;
		result.BytesPerPixel = 4;
		result.Pixels.resize(static_cast<size_t>(result.Width) * result.Height * 4);
		const uint32_t samples = factor * factor;
		for (uint32_t y = 0; y < result.Height; y++)
		{
			for (uint32_t x = 0; x < result.Width; x++)
			{
				uint32_t sums[4] = {};
				for (uint32_t row = 0; row < factor; row++)
				{
					const uint8_t* source = image.Pixels.data() + ((static_cast<size_t>(y) * factor + row) * image.Width + static_cast<size_t>(x) * factor) * 4;
					for (uint32_t column = 0; column < factor * 4; column++)
						sums[column % 4] += source[column];
				}
				uint8_t* target = result.Pixels.data() + (static_cast<size_t>(y) * result.Width + x) * 4;
				for (uint32_t channel = 0; channel < 4; channel++)
					target[channel] = static_cast<uint8_t>((sums[channel] + samples / 2) / samples);
			}
		}
		return result;
	}

	ProjectThumbnail::ProjectThumbnail()
		: m_Write(CreateRef<WriteState>())
	{
	}

	ProjectThumbnail::~ProjectThumbnail() = default;

	void ProjectThumbnail::Request()
	{
		if (m_Requested && (m_Readback || m_Frames > 0))
			m_RequestedAgain = true;
		m_Requested = true;
	}

	void ProjectThumbnail::Cancel()
	{
		m_Requested = false;
		m_RequestedAgain = false;
		m_Frames = 0;
		m_Readback.reset();
	}

	bool ProjectThumbnail::IsPending() const
	{
		return m_Requested || m_Write->Running.load() > 0;
	}

	uint64_t ProjectThumbnail::GetWrittenCount() const
	{
		return m_Write->Written.load();
	}

	void ProjectThumbnail::Update(EditorContext& context)
	{
		if (!m_Requested)
			return;
		if (!Renderer::IsInitialized() || !context.GetProject() || context.IsPlaying())
		{
			// Nothing to render with, or the game runs (the saved scene is what the picture shows): tried again at the next save.
			Cancel();
			return;
		}

		if (!m_Readback)
		{
			ViewportRenderer* renderer = context.GetViewport().GetCaptureRenderer();
			if (!renderer)
			{
				Cancel();
				return;
			}
			const float aspectRatio = static_cast<float>(c_Width) / static_cast<float>(c_Height);
			const glm::uvec2 size(c_Width * c_RenderScale, c_Height * c_RenderScale);
			std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Scene, aspectRatio);
			if (!view)
				view = ResolveViewportView(context, ViewportCameraSource::Editor, aspectRatio);
			if (!view)
			{
				Cancel();
				return;
			}
			// As the game shows it: no editor overlays, no preview lighting of its own.
			ViewportSettings settings = context.GetViewport().GetSettings();
			settings.DebugView = ViewportDebugView::None;
			renderer->GetSceneRenderer().ResetExposureAdaptation();
			if (!renderer->Render(context, size, *view, settings, false))
			{
				renderer->ReleaseTargets();
				Cancel();
				return;
			}
			// Complete, as captures wait for it: what loads, glyphs still being prepared, and the finer levels of textures the
			// view wants (a texture counts as loaded with its mip tail).
			const SceneRendererStats& stats = renderer->GetStats();
			if ((stats.PendingAssets > 0 || stats.PendingTextGlyphs > 0 || renderer->CountTexturesBelowWanted() > 0) && ++m_Frames < c_MaxFrames)
				return;
			std::string error;
			m_Readback = TextureReadback::Create(renderer->GetOutputTexture(), {}, &error);
			// Like captures, the picture keeps no render targets between saves.
			renderer->ReleaseTargets();
			m_File = GetFile(context.GetProject()->GetProjectDirectory());
			if (!m_Readback)
			{
				ST_WARN("The project's thumbnail could not be read back: {}", error);
				Cancel();
			}
			return;
		}

		if (!m_Readback->IsReady())
			return;
		ReadbackImage image;
		const bool read = m_Readback->GetResult(image);
		m_Readback.reset();
		m_Frames = 0;
		m_Requested = m_RequestedAgain;
		m_RequestedAgain = false;
		if (!read)
			return;
		// Averaged down, encoded and written on an I/O thread: milliseconds of work for a picture of this size.
		m_Write->Running++;
		JobSystem::SubmitIO([state = m_Write, image = std::move(image), file = m_File]()
		{
			std::string error = "the rendered picture has an unexpected format or size";
			const std::optional<ReadbackImage> picture = Downscale(image, c_RenderScale);
			const std::optional<std::vector<uint8_t>> png = picture ? ImageWriter::EncodePNG(*picture, true, &error) : std::nullopt;
			if (png && FileSystem::WriteBytes(file, *png))
				state->Written++;
			else
				ST_WARN("The project's thumbnail could not be written to '{}': {}", FileSystem::ToUTF8(file), png ? std::string("writing failed") : error);
			state->Running--;
		}, JobPriority::Low);
	}

}

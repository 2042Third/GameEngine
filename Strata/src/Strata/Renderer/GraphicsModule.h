#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/EngineModule.h"
#include "Strata/Renderer/TextureReadback.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class GraphicsDevice;
	class Window;
	struct ReadbackImage;

	struct GraphicsModuleSpecification
	{
		// The Khronos validation layer (where installed) and NVRHI's validation; nullopt: on in Debug builds, off otherwise.
		std::optional<bool> Validation;
		// Asset uploads run on the device's dedicated transfer queue where it has one (RendererSpecification).
		bool TransferQueueUploads = false;
	};

	// The engine module of the graphics device and the Renderer: it creates the device (with a swapchain for the program's
	// window, offscreen only without one) and initializes the Renderer at startup, begins every frame (vsync, the wait for
	// the GPU and the swapchain image, Renderer::BeginFrame), presents it (the Present stage) and answers back-buffer
	// captures, and shuts both down last of all. A program without it renders nothing.
	class GraphicsModule final : public EngineModule
	{
	public:
		static constexpr std::string_view c_Name = "Graphics";

		explicit GraphicsModule(GraphicsModuleSpecification specification = {});
		~GraphicsModule() override;

		static EngineModuleFactory Factory(GraphicsModuleSpecification specification = {});

		std::string_view GetName() const override { return c_Name; }
		[[nodiscard]] bool OnInit(EngineContext& context) override;
		EngineModuleStartFailure GetStartFailure() const override { return m_StartFailure; }
		EngineFrameAction OnBeginFrame(EngineFrame& frame) override;
		void OnEndFrame(EngineFrame& frame) override;
		void OnStop() override;
		void OnShutdown() override;

		// Null until the module started and after it shut down.
		GraphicsDevice* GetDevice() const { return m_Device.get(); }

		// Reads back the window's back buffer at the end of the next rendered frame (after the UI is drawn, before
		// presenting) and passes it to the callback on the main thread: screenshots. The image is empty (Width 0) when there
		// is nothing to capture (headless, a read failure, the program ending). Thread-safe.
		void RequestBackBufferCapture(std::function<void(const ReadbackImage&)> callback);
		// What a back-buffer readback gives its callback: the pixels (empty when there are none) and, then, why.
		using BackBufferReadbackCallback = std::function<void(const ReadbackImage& image, const std::string& error)>;
		// Reads `region` of the window's back buffer back at the end of the next rendered frame (after the UI is drawn,
		// before presenting) without waiting for the GPU: the copy is submitted then (TextureReadback) and polled at the start
		// of each later frame, so no frame stalls. The callback runs on the main thread once the pixels arrived, or with an
		// empty image and the reason when there are none: no window (headless), a region outside the back buffer, a copy the
		// GPU did not finish within c_BackBufferReadbackTimeout, the program ending. Main thread only.
		void RequestBackBufferReadback(const TextureReadbackRegion& region, BackBufferReadbackCallback callback);
		// A readback the GPU has not finished after this long fails (a lost or hung device).
		static constexpr std::chrono::seconds c_BackBufferReadbackTimeout { 30 };
	private:
		void ProcessBackBufferCaptures(bool frameRendered);
		// Starts the readbacks asked for (the frame rendered) or fails them; polls the ones under way.
		void StartBackBufferReadbacks(bool frameRendered);
		void PollBackBufferReadbacks();
		void FailBackBufferReadbacks(const std::string& reason);
	private:
		struct BackBufferReadbackRequest
		{
			TextureReadbackRegion Region;
			BackBufferReadbackCallback Callback;
		};
		struct PendingBackBufferReadback
		{
			Scope<TextureReadback> Readback;
			BackBufferReadbackCallback Callback;
			std::chrono::steady_clock::time_point Submitted;
		};
	private:
		GraphicsModuleSpecification m_Specification;
		Window* m_Window = nullptr;
		Scope<GraphicsDevice> m_Device;
		EngineModuleStartFailure m_StartFailure;

		std::mutex m_CaptureMutex;
		std::vector<std::function<void(const ReadbackImage&)>> m_BackBufferCaptures;
		std::vector<BackBufferReadbackRequest> m_BackBufferReadbackRequests;
		std::vector<PendingBackBufferReadback> m_BackBufferReadbacks;
	};

}

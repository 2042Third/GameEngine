#include "stpch.h"
#include "Strata/Renderer/GraphicsModule.h"

#include "Strata/Core/Application.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Core/Timer.h"
#include "Strata/Core/Window.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/TextureReadback.h"

namespace Strata
{

	GraphicsModule::GraphicsModule(GraphicsModuleSpecification specification)
		: m_Specification(specification)
	{
	}

	GraphicsModule::~GraphicsModule() = default;

	EngineModuleFactory GraphicsModule::Factory(GraphicsModuleSpecification specification)
	{
		return [specification]() -> Scope<EngineModule> { return CreateScope<GraphicsModule>(specification); };
	}

	bool GraphicsModule::OnInit(EngineContext& context)
	{
		m_Window = context.MainWindow;
		const std::string& name = context.App.GetSpecification().Name;

		GraphicsDeviceSpecification deviceSpecification;
		deviceSpecification.ApplicationName = name;
		deviceSpecification.Headless = m_Window == nullptr;
		deviceSpecification.PresentationWindow = m_Window;
#if defined(ST_DEBUG)
		deviceSpecification.EnableValidation = m_Specification.Validation.value_or(true);
#else
		deviceSpecification.EnableValidation = m_Specification.Validation.value_or(false);
#endif

		m_Device = GraphicsDevice::Create(deviceSpecification);
		bool started = m_Device != nullptr;
		// A window that cannot present (a test's stand-in) gets a device that renders offscreen, and no swapchain.
		if (started && m_Window && m_Device->CanPresent())
			started = m_Device->CreateSwapchain(*m_Window, m_Window->IsVSync());
		if (started)
		{
			RendererSpecification rendererSpecification;
			rendererSpecification.TransferQueueUploads = m_Specification.TransferQueueUploads;
			started = Renderer::Init(*m_Device, rendererSpecification);
		}
		if (started)
			return true;

		// The device logged why; the person learns what to do about it.
		m_Device.reset();
		m_Window = nullptr;
		m_StartFailure.Problem = fmt::format("{} cannot start: no usable graphics device was found (it needs Vulkan 1.2 or later).", name);
		m_StartFailure.Advice = GraphicsDevice::GetDriverInstallAdvice();
		return false;
	}

	EngineFrameAction GraphicsModule::OnBeginFrame(EngineFrame& frame)
	{
		if (m_Window && m_Window->IsVSync() != m_Device->IsVSync())
			m_Device->SetVSync(m_Window->IsVSync());

		// Waits for the GPU to finish an earlier frame and for the display to hand out an image.
		const double waitStart = Time::GetTime();
		const bool began = m_Device->BeginFrame();
		frame.WaitSeconds += Time::GetTime() - waitStart;
		// A device that cannot render this frame (e.g. a swapchain mid-resize) skips the whole frame.
		if (!began)
			return m_Device->IsDeviceLost() ? EngineFrameAction::Close : EngineFrameAction::Skip;
		Renderer::BeginFrame();
		PollBackBufferReadbacks();
		return EngineFrameAction::Continue;
	}

	void GraphicsModule::OnEndFrame(EngineFrame&)
	{
		// A frame that drew nothing into the back buffer still presents (and captures) a defined image: black.
		Renderer::PrepareBackBuffer();
		ProcessBackBufferCaptures(true);
		StartBackBufferReadbacks(true);
		ST_PROFILE_STAGE("Present");
		m_Device->EndFrame();
	}

	void GraphicsModule::OnStop()
	{
		// No frame will render what is still asked for.
		ProcessBackBufferCaptures(false);
		FailBackBufferReadbacks("the program is closing");
		m_Device->WaitForIdle();
	}

	void GraphicsModule::OnShutdown()
	{
		ProcessBackBufferCaptures(false);
		FailBackBufferReadbacks("the program is closing");
		Renderer::Shutdown();
		m_Device.reset();
		m_Window = nullptr;
	}

	void GraphicsModule::RequestBackBufferCapture(std::function<void(const ReadbackImage&)> callback)
	{
		std::scoped_lock<std::mutex> lock(m_CaptureMutex);
		m_BackBufferCaptures.push_back(std::move(callback));
	}

	void GraphicsModule::RequestBackBufferReadback(const TextureReadbackRegion& region, BackBufferReadbackCallback callback)
	{
		m_BackBufferReadbackRequests.push_back({ region, std::move(callback) });
	}

	void GraphicsModule::StartBackBufferReadbacks(bool frameRendered)
	{
		if (m_BackBufferReadbackRequests.empty())
			return;
		std::vector<BackBufferReadbackRequest> requests;
		requests.swap(m_BackBufferReadbackRequests);
		nvrhi::ITexture* backBuffer = frameRendered && m_Device && m_Device->HasSwapchain() ? m_Device->GetBackBuffer() : nullptr;
		for (BackBufferReadbackRequest& request : requests)
		{
			if (!backBuffer)
			{
				request.Callback(ReadbackImage(), "there is no window to read back (headless)");
				continue;
			}
			std::string error;
			Scope<TextureReadback> readback = TextureReadback::Create(backBuffer, request.Region, &error);
			if (!readback)
			{
				request.Callback(ReadbackImage(), fmt::format("reading the window back failed: {}", error));
				continue;
			}
			m_BackBufferReadbacks.push_back({ std::move(readback), std::move(request.Callback), std::chrono::steady_clock::now() });
		}
	}

	void GraphicsModule::PollBackBufferReadbacks()
	{
		for (size_t index = 0; index < m_BackBufferReadbacks.size();)
		{
			PendingBackBufferReadback& pending = m_BackBufferReadbacks[index];
			ReadbackImage image;
			std::string error;
			if (pending.Readback->IsReady())
			{
				if (!pending.Readback->GetResult(image))
					error = "mapping the window's pixels failed";
			}
			else if (std::chrono::steady_clock::now() - pending.Submitted > c_BackBufferReadbackTimeout)
			{
				error = fmt::format("the GPU did not finish reading the window back within {} seconds", c_BackBufferReadbackTimeout.count());
			}
			else
			{
				index++;
				continue;
			}
			// Taken out first: the callback may ask for another readback.
			BackBufferReadbackCallback callback = std::move(pending.Callback);
			m_BackBufferReadbacks.erase(m_BackBufferReadbacks.begin() + static_cast<std::ptrdiff_t>(index));
			callback(image, error);
		}
	}

	void GraphicsModule::FailBackBufferReadbacks(const std::string& reason)
	{
		std::vector<BackBufferReadbackRequest> requests;
		requests.swap(m_BackBufferReadbackRequests);
		std::vector<PendingBackBufferReadback> pending;
		pending.swap(m_BackBufferReadbacks);
		for (BackBufferReadbackRequest& request : requests)
			request.Callback(ReadbackImage(), reason);
		for (PendingBackBufferReadback& readback : pending)
			readback.Callback(ReadbackImage(), reason);
	}

	void GraphicsModule::ProcessBackBufferCaptures(bool frameRendered)
	{
		std::vector<std::function<void(const ReadbackImage&)>> captures;
		{
			std::scoped_lock<std::mutex> lock(m_CaptureMutex);
			captures.swap(m_BackBufferCaptures);
		}
		if (captures.empty())
			return;

		ReadbackImage image;
		nvrhi::ITexture* backBuffer = frameRendered && m_Device ? m_Device->GetBackBuffer() : nullptr;
		if (backBuffer && !Renderer::ReadTexture(backBuffer, image))
		{
			ST_CORE_ERROR("Back buffer capture failed");
			image = ReadbackImage();
		}
		for (const auto& capture : captures)
			capture(image);
	}

}

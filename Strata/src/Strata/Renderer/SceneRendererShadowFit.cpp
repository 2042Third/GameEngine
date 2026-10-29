#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/Renderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <string>

// The sun's cascades fitted to what the camera sees (Docs/DeveloperGuide.md, "Sun shadows"; SceneRendererSpecification::
// FitShadowsToView): in GPU.ScreenShadows, after the mask, Scene/ShadowFit.comp reduces the frame's prepass depth to the
// receivers (RenderData::ShadowFitResult), copied into a ring of readbacks with the camera and the tracker's epoch and layout
// of the frame; at the start of a later frame's extraction the newest one that arrived goes to ShadowFitTracker, whose
// layout FindShadowCasters applies. Both paths run the same pass on the complete prepass depth, so they fit alike.

namespace Strata
{

	void SceneRenderer::UpdateShadowFit(const RenderScene& renderScene, const SceneCamera& camera, bool cameraCut)
	{
		ST_PROFILE_FUNCTION();
		m_ShadowFitLayout = {};
		m_Stats.ShadowFitChanges = m_ShadowFit.GetChanges();
		if (!m_ShadowLight.Enabled)
		{
			// Nothing to fit: the next shadowed frame starts again.
			if (m_ShadowFitActive)
				m_ShadowFit.Reset();
			m_ShadowFitActive = false;
			return;
		}

		// The light's view: light view space looks along the light (FindShadowCasters casts the cascades in it).
		const glm::vec3 lightDirection = m_ShadowLight.Direction;
		const glm::vec3 up = std::abs(lightDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		m_LightView = glm::lookAt(glm::vec3(0.0f), lightDirection, up);
		const glm::dmat3 lightRotation = glm::dmat3(glm::mat3(m_LightView));
		m_CameraLightPosition = lightRotation * glm::dvec3(camera.Position);

		ShadowFitView view;
		view.CameraPosition = glm::dvec3(camera.Position);
		view.CameraRotation = glm::mat3(glm::inverse(camera.View));
		view.LightRotation = lightRotation;
		view.Near = std::max(camera.Near, 0.01f);
		view.ShadowDistance = std::clamp(m_ShadowLight.Distance, view.Near + 0.1f, std::max(camera.Far, view.Near + 0.1f));
		view.Orthographic = camera.Orthographic;
		view.TanHalfFOV = camera.Orthographic ? 0.0f : std::tan(camera.VerticalFOV * 0.5f);
		view.OrthographicHeight = camera.Orthographic ? camera.VerticalFOV : 0.0f;
		view.Aspect = static_cast<float>(m_ViewportSize.x) / static_cast<float>(m_ViewportSize.y);
		view.CascadeCount = std::clamp(m_Specification.Quality.ShadowCascades, 1u, RenderData::c_MaxCascades);
		view.MapSize = m_ShadowMapSize;
		view.CascadeBlend = m_ShadowLight.CascadeBlend;
		view.Distribution = m_ShadowLight.Distribution;
		view.NearestTransparent = m_NearestTransparent;

		if (!m_Specification.FitShadowsToView)
		{
			if (m_ShadowFitActive)
				m_ShadowFit.Reset();
			m_ShadowFitActive = false;
			m_ShadowFitLayout = ShadowFitTracker::GetUnfittedLayout(view);
			return;
		}

		// A new epoch where the readbacks in flight say nothing about this frame: the fit's first frame, a reset asked for, a
		// camera cut, another render scene (a clone of the previous one, entering Play, keeps it), other cascades.
		const bool sameScene = m_ShadowFitSceneId == renderScene.GetId() || (m_ShadowFitSceneId != 0 && renderScene.GetCloneSource() == m_ShadowFitSceneId);
		if (!m_ShadowFitActive || m_ResetShadowFit || cameraCut || !sameScene || m_ShadowFitMapSize != view.MapSize || m_ShadowFitCascades != view.CascadeCount)
			m_ShadowFit.Reset();
		m_ShadowFitActive = true;
		m_ResetShadowFit = false;
		m_ShadowFitSceneId = renderScene.GetId();
		m_ShadowFitMapSize = view.MapSize;
		m_ShadowFitCascades = view.CascadeCount;

		// The newest readback of the epoch that completed and was not given to the tracker yet (never waited for).
		ShadowFitRecord* newest = nullptr;
		for (ShadowFitRecord& entry : m_ShadowFitReadbacks)
		{
			if (entry.Readback && entry.Readback->IsPending() && entry.Epoch == m_ShadowFit.GetEpoch() && entry.Render > m_ShadowFitSeen
				&& entry.Readback->IsReady() && (!newest || entry.Render > newest->Render))
				newest = &entry;
		}
		const ShadowFitReadback* readback = nullptr;
		if (newest)
		{
			m_ShadowFitSeen = newest->Render;
			m_ShadowFitScratch.Render = newest->Render;
			m_ShadowFitScratch.Epoch = newest->Epoch;
			m_ShadowFitScratch.LayoutVersion = newest->LayoutVersion;
			m_ShadowFitScratch.CameraPosition = newest->CameraPosition;
			m_ShadowFitScratch.CameraRotation = newest->CameraRotation;
			if (newest->Readback->Read(&m_ShadowFitScratch.Result, sizeof(RenderData::ShadowFitResult)))
				readback = &m_ShadowFitScratch;
		}
		m_ShadowFitLayout = m_ShadowFit.Update(view, readback);
		m_Stats.ShadowFit = m_ShadowFitLayout.Fitted;
		m_Stats.ShadowFitSquares = m_ShadowFitLayout.SquaresFitted;
		m_Stats.ShadowFitSettled = m_ShadowFit.IsSettled();
		m_Stats.ShadowFitNear = m_ShadowFitLayout.Near;
		m_Stats.ShadowFitFar = m_ShadowFitLayout.Far;
		m_Stats.ShadowFitChanges = m_ShadowFit.GetChanges();
	}

	bool SceneRenderer::RecordShadowFit(nvrhi::ICommandList* commandList, const SceneCamera& camera)
	{
		if (!m_ShadowFitPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(22), // The prepass's depth
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
				// Slot 1: register b0 belongs to the frame constants.
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::ShadowFitParameters))
			};
			m_ShadowFitLayoutHandle = m_Device->createBindingLayout(layout);
			ST_CORE_VERIFY(m_ShadowFitLayoutHandle, "SceneRenderer: failed to create the shadow fit's binding layout");
			m_ShadowFitPipeline = CreateComputePipeline("Scene/ShadowFit.comp", m_ShadowFitLayoutHandle);
			ST_CORE_VERIFY(m_ShadowFitPipeline, "SceneRenderer: failed to create the shadow fit's pipeline");
		}
		if (!m_ShadowFitBuffer)
		{
			nvrhi::BufferHandle buffer = m_Device->createBuffer(nvrhi::BufferDesc()
				.setByteSize(sizeof(RenderData::ShadowFitResult))
				.setCanHaveUAVs(true)
				.setStructStride(sizeof(uint32_t))
				.setInitialState(nvrhi::ResourceStates::UnorderedAccess)
				.setKeepInitialState(true)
				.setDebugName(m_Specification.DebugName + ".ShadowFit"));
			bool readbacks = true;
			for (ShadowFitRecord& entry : m_ShadowFitReadbacks)
			{
				entry = {};
				entry.Readback = CreateScope<BufferReadback>(m_Device, sizeof(RenderData::ShadowFitResult), m_Specification.DebugName + ".ShadowFitReadback");
				readbacks = readbacks && entry.Readback->IsValid();
			}
			if (!buffer || !readbacks)
			{
				for (ShadowFitRecord& entry : m_ShadowFitReadbacks)
					entry = {};
				m_Errors.Report("failed to create the shadow cascades' fit buffers; the cascades keep Strata 0.6.0's layout");
				return false;
			}
			m_ShadowFitBuffer = std::move(buffer);
			m_ShadowFitSet = nullptr; // Names the buffer
		}
		if (!m_ShadowFitSet)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ShadowFitBuffer),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::ShadowFitParameters))
			};
			m_ShadowFitSet = m_Device->createBindingSet(desc, m_ShadowFitLayoutHandle);
			if (!m_ShadowFitSet)
			{
				m_Errors.Report("failed to create the shadow cascades' fit bindings; the cascades keep Strata 0.6.0's layout");
				return false;
			}
		}

		// A readback whose earlier copy is still under way is left to finish, and this frame is not measured.
		ShadowFitRecord& record = m_ShadowFitReadbacks[m_ShadowFitReadbackIndex];
		if (record.Readback->IsPending() && !record.Readback->IsReady())
			return true;
		commandList->clearBufferUInt(m_ShadowFitBuffer, 0);
		nvrhi::ComputeState state;
		state.pipeline = m_ShadowFitPipeline;
		state.bindings = { m_ShadowFitSet };
		commandList->setComputeState(state);
		RenderData::ShadowFitParameters parameters = {};
		parameters.Width = m_ViewportSize.x;
		parameters.Height = m_ViewportSize.y;
		commandList->setPushConstants(&parameters, sizeof(parameters));
		// A thread reduces 4 x 4 pixels, a group of 16 x 16 threads 64 x 64 (Scene/ShadowFit.comp).
		commandList->dispatch((m_ViewportSize.x + 63) / 64, (m_ViewportSize.y + 63) / 64, 1);
		record.Readback->Record(commandList, m_ShadowFitBuffer, 0, sizeof(RenderData::ShadowFitResult));
		record.Render = m_RenderCount + 1;
		record.Epoch = m_ShadowFit.GetEpoch();
		record.LayoutVersion = m_ShadowFit.GetLayoutVersion();
		record.CameraPosition = glm::dvec3(camera.Position);
		record.CameraRotation = glm::dmat3(glm::mat3(glm::inverse(camera.View)));
		m_ShadowFitRecorded = true;
		return true;
	}

	void SceneRenderer::SubmitShadowFitReadback()
	{
		if (!m_ShadowFitRecorded)
			return;
		m_ShadowFitReadbacks[m_ShadowFitReadbackIndex].Readback->Submit();
		m_ShadowFitReadbackIndex = (m_ShadowFitReadbackIndex + 1) % c_GPURecordCount;
		m_ShadowFitRecorded = false;
	}

}

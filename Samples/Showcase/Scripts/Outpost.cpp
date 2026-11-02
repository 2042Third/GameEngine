#include "Outpost.h"

#include "Flicker.h"

#include <initializer_list>
#include <optional>
#include <string>

using namespace Strata;
using namespace Showcase;

namespace
{

	constexpr std::array<const char*, 6> c_ProbeFields = { "DawnYard", "DawnHut", "GoldenYard", "GoldenHut", "DuskYard", "DuskHut" };
	// A probe's priority while its station is the one switched to (the old one may still be on), and after.
	constexpr int32_t c_YardPriority = 0;
	constexpr int32_t c_HutPriority = 2;

	uint32_t LayerMask(std::initializer_list<const char*> names)
	{
		uint32_t mask = 0;
		for (const char* name : names)
		{
			const int32_t layer = Physics::GetLayerIndex(name);
			if (layer >= 0)
				mask |= 1u << static_cast<uint32_t>(layer);
		}
		return mask != 0 ? mask : Physics::c_AllLayers;
	}

	void SetLightIntensity(Entity light, float candela)
	{
		if (!light.IsValid())
			return;
		if (Flicker* flicker = light.GetScript<Flicker>())
			flicker->Intensity = candela;
		else
			light.SetProperty("PointLight", "Intensity", candela);
	}

}

void Outpost::OnCreate()
{
	m_GroundMask = LayerMask({ "Terrain", "Props" });
	FindMarkers();
	RequestCaptures();
	SetStation(StationId::Golden, true);
	PhotoHour = GetStation(StationId::Golden).Hour;
	SetPhotoOpen(false);
	SetFade(1.0f);
	if (TourOnStart)
	{
		StartTour();
	}
	else
	{
		// Walking from the Player's place, standing still.
		Shot = 0;
		SetFade(0.0f);
		SetMode(1);
	}
}

void Outpost::OnReload()
{
	m_GroundMask = LayerMask({ "Terrain", "Props" });
	FindMarkers();
}

void Outpost::OnUpdate(float)
{
	// Real time: the tour and the probe switch run while the photo mode pauses the game. While the editor steps a paused
	// game it is the fixed step, so a regression's step counts are exact.
	const float realDeltaTime = Time::GetUnscaledDeltaTime();
	m_SinceCreate += realDeltaTime;
	PollCaptures();
	UpdateProbeHold(realDeltaTime);
	HandleInput();
	if (Mode == 0)
		UpdateTour(realDeltaTime);
	else if (Mode == 1)
		UpdateWalk(realDeltaTime);
	else
		UpdateFlight(realDeltaTime);
	if (PhotoOpen && m_Autofocus)
		UpdateAutofocus();
	MeasureGround();
}

void Outpost::HandleInput()
{
	if (Input::IsActionPressed("Showcase/Tour"))
	{
		StartTour();
		return;
	}
	// A bookmark flies there, also from the tour.
	for (int32_t number = 1; number <= 7; number++)
	{
		if (Input::IsActionPressed("Showcase/Bookmark" + std::to_string(number)))
		{
			TakeBookmark(number);
			return;
		}
	}
	if (Mode == 0)
	{
		// Any control hands the tour over to the walk (the press itself does nothing else).
		const bool taken = Input::IsActionPressed("Gameplay/Move") || Input::IsActionPressed("Gameplay/Look") || Input::IsActionPressed("Gameplay/Jump")
			|| Input::IsActionPressed("Showcase/Fly") || Input::IsActionPressed("Showcase/Photo") || Input::IsMouseButtonPressed(Mouse::ButtonLeft)
			|| Input::IsMouseButtonPressed(Mouse::ButtonRight);
		if (taken)
			EndTour("taken over");
		return;
	}
	if (Input::IsActionPressed("Showcase/Fly"))
		SetMode(Mode == 1 ? 2 : 1);
	if (Input::IsActionPressed("Showcase/Photo"))
		SetPhotoOpen(!PhotoOpen);
	if (Input::IsActionPressed("Showcase/HideUI"))
		ToggleHideUI();
}

void Outpost::FindMarkers()
{
	m_MarkersFound = true;
	const auto find = [this](const std::string& name, glm::vec3& position)
	{
		const Entity marker = Scene::FindEntityByName(name);
		if (!marker.IsValid())
		{
			Log::Warn("Showcase: no entity '", name, "'");
			m_MarkersFound = false;
			return;
		}
		position = marker.GetTransform().GetWorldPosition();
	};
	for (int32_t number = 1; number <= c_ShotCount; number++)
	{
		ShotMarkers& markers = m_Shots[static_cast<size_t>(number - 1)];
		const std::string prefix = "Shot" + std::to_string(number) + ".";
		find(prefix + "From", markers.From);
		find(prefix + "Key", markers.Key);
		find(prefix + "To", markers.To);
		find(prefix + "Look", markers.Look);
	}
	// The rock macro, bookmark 7.
	find("Macro.Key", m_Macro.Key);
	find("Macro.Look", m_Macro.Look);
}

void Outpost::RequestCaptures()
{
	CapturesFound = 0;
	CapturesResident = 0;
	for (size_t index = 0; index < c_ProbeFields.size(); index++)
	{
		const Entity probe = GetProbe(static_cast<StationId>(index / 2), index % 2 == 1);
		m_Captures[index] = AssetHandle();
		if (!probe.IsValid())
		{
			Log::Warn("Showcase: no entity '", c_ProbeFields[index], "'");
			continue;
		}
		m_Captures[index] = probe.GetProperty<AssetHandle>("ReflectionProbe", "Capture").value_or(AssetHandle());
		// The first call loads all of the capture and keeps it while the scene plays, so a switch never waits for a load.
		if (m_Captures[index] && Assets::GetLoadProgress(m_Captures[index]) >= 0.0f)
			CapturesFound++;
	}
}

void Outpost::PollCaptures()
{
	if (m_StationsReady)
		return;
	int32_t resident = 0;
	for (const AssetHandle capture : m_Captures)
	{
		if (capture && Assets::GetLoadProgress(capture) >= 1.0f)
			resident++;
	}
	CapturesResident = resident;
	if (resident < CapturesFound && m_SinceCreate < StationWaitSeconds)
		return;
	m_StationsReady = true;
	Log::Info("Showcase: stations ready (found ", CapturesFound, ", resident ", CapturesResident, ")");
}

Entity Outpost::GetProbe(StationId station, bool hut) const
{
	switch (station)
	{
		case StationId::Dawn:
			return hut ? DawnHut : DawnYard;
		case StationId::Golden:
			return hut ? GoldenHut : GoldenYard;
		case StationId::Dusk:
			return hut ? DuskHut : DuskYard;
	}
	return Entity();
}

void Outpost::SetProbes(StationId station, bool active, bool raised)
{
	for (const bool hut : { false, true })
	{
		Entity probe = GetProbe(station, hut);
		if (!probe.IsValid())
			continue;
		probe.SetActive(active);
		// While both stations are on, the new one's captures win where they are converted; until then the old one lights
		// the boxes, so they never fall to the sky.
		const int32_t priority = (hut ? c_HutPriority : c_YardPriority) + (raised ? 1 : 0);
		probe.SetProperty("ReflectionProbe", "Priority", priority);
	}
}

void Outpost::EndProbeHold()
{
	if (m_HoldStation < 0)
		return;
	SetProbes(static_cast<StationId>(m_HoldStation), false, false);
	SetProbes(static_cast<StationId>(Station), true, false);
	m_HoldStation = -1;
	m_HoldLeft = 0.0f;
}

void Outpost::UpdateProbeHold(float realDeltaTime)
{
	if (m_HoldStation < 0)
		return;
	m_HoldLeft -= realDeltaTime;
	if (m_HoldLeft <= 0.0f)
		EndProbeHold();
}

void Outpost::SetStation(StationId station, bool immediately)
{
	if (!immediately && static_cast<int32_t>(station) == Station)
		return;
	ApplyLight(GetStation(station));
	SwitchProbes(station, immediately);
}

void Outpost::SwitchProbes(StationId station, bool immediately)
{
	const int32_t index = static_cast<int32_t>(station);
	if (!immediately && index == Station)
		return;
	// At most two stations are on: a switch while another one's hold runs ends that hold first.
	EndProbeHold();
	if (immediately)
	{
		for (const StationId other : { StationId::Dawn, StationId::Golden, StationId::Dusk })
			SetProbes(other, other == station, false);
	}
	else
	{
		m_HoldStation = Station;
		m_HoldLeft = ProbeHoldSeconds;
		SetProbes(station, true, true);
	}
	Station = index;
	Log::Info("Showcase: station ", GetStationName(station));
}

void Outpost::ApplyLight(const StationValues& values)
{
	SunElevation = values.Elevation;
	SunAzimuth = values.Azimuth;
	if (Sun.IsValid())
	{
		// The light shines along its -Z, away from the sun.
		Sun.GetTransform().SetRotation(glm::quatLookAt(-TowardSun(values.DrawnElevation, values.Azimuth), glm::vec3(0.0f, 1.0f, 0.0f)));
		Sun.SetProperty("DirectionalLight", "Intensity", SunIlluminance * values.LightScale);
		Sun.SetProperty("DirectionalLight", "ShadowDistance", values.ShadowDistance);
	}
	if (Fog.IsValid())
		Fog.SetProperty("HeightFog", "Density", values.FogDensity);
	m_ExposureBase = values.Exposure;
	ApplyExposure();
	SetLightIntensity(FireLight, values.LocalLights ? FireCandela : 0.0f);
	SetLightIntensity(LanternALight, values.LocalLights ? LanternACandela : 0.0f);
	SetLightIntensity(LanternBLight, values.LocalLights ? LanternBCandela : 0.0f);
	for (Entity glow : { Embers, WickA, WickB })
	{
		if (glow.IsValid())
			glow.SetActive(values.LocalLights);
	}
}

void Outpost::MeasureGround()
{
	if (!Eye.IsValid())
		return;
	const glm::vec3 eye = Eye.GetTransform().GetWorldPosition();
	if (const std::optional<RaycastHit> ground = Physics::Raycast(eye + glm::vec3(0.0f, 300.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 900.0f, m_GroundMask))
		HeightAboveGround = eye.y - ground->Point.y;
}

ST_SCRIPT_CLASS(Outpost)
{
	ST_SCRIPT_FIELD(Sun);
	ST_SCRIPT_FIELD(Fog);
	ST_SCRIPT_FIELD(PostProcess);
	ST_SCRIPT_FIELD(Player);
	ST_SCRIPT_FIELD(Eye);
	ST_SCRIPT_FIELD(FadeImage);
	ST_SCRIPT_FIELD(Caption);
	ST_SCRIPT_FIELD(DawnYard);
	ST_SCRIPT_FIELD(DawnHut);
	ST_SCRIPT_FIELD(GoldenYard);
	ST_SCRIPT_FIELD(GoldenHut);
	ST_SCRIPT_FIELD(DuskYard);
	ST_SCRIPT_FIELD(DuskHut);
	ST_SCRIPT_FIELD(FireLight);
	ST_SCRIPT_FIELD(LanternALight);
	ST_SCRIPT_FIELD(LanternBLight);
	ST_SCRIPT_FIELD(Embers);
	ST_SCRIPT_FIELD(WickA);
	ST_SCRIPT_FIELD(WickB);
	ST_SCRIPT_FIELD(PhotoPanel);
	ST_SCRIPT_FIELD(TimeSlider);
	ST_SCRIPT_FIELD(TimeLabel);
	ST_SCRIPT_FIELD(ExposureSlider);
	ST_SCRIPT_FIELD(ExposureLabel);
	ST_SCRIPT_FIELD(FovSlider);
	ST_SCRIPT_FIELD(FovLabel);
	ST_SCRIPT_FIELD(FocusSlider);
	ST_SCRIPT_FIELD(FocusLabel);
	ST_SCRIPT_FIELD(ApertureSlider);
	ST_SCRIPT_FIELD(ApertureLabel);
	ST_SCRIPT_FIELD(DofToggle);
	ST_SCRIPT_FIELD(AutoFocusToggle);
	ST_SCRIPT_FIELD(QualityLabel);
	ST_SCRIPT_FIELD(AntiAliasingLabel);
	ST_SCRIPT_FIELD(TourOnStart);
	ST_SCRIPT_FIELD(StartShot);
	ST_SCRIPT_FIELD(TourSpeed);
	ST_SCRIPT_FIELD(LoopTour);
	ST_SCRIPT_FIELD(FadeSeconds);
	ST_SCRIPT_FIELD(WalkSpeed);
	ST_SCRIPT_FIELD(RunSpeed);
	ST_SCRIPT_FIELD(FlySpeed);
	ST_SCRIPT_FIELD(FastFactor);
	ST_SCRIPT_FIELD(LookSpeed);
	ST_SCRIPT_FIELD(StickTurnSpeed);
	ST_SCRIPT_FIELD(WalkRadius);
	ST_SCRIPT_FIELD(ProbeHoldSeconds);
	ST_SCRIPT_FIELD(StationWaitSeconds);
	ST_SCRIPT_FIELD(PhotoHourMax);
	ST_SCRIPT_FIELD(SunIlluminance);
	ST_SCRIPT_FIELD(FireCandela);
	ST_SCRIPT_FIELD(LanternACandela);
	ST_SCRIPT_FIELD(LanternBCandela);
	ST_SCRIPT_FIELD(Mode);
	ST_SCRIPT_FIELD(Shot);
	ST_SCRIPT_FIELD(Station);
	ST_SCRIPT_FIELD(CapturesFound);
	ST_SCRIPT_FIELD(CapturesResident);
	ST_SCRIPT_FIELD(PhotoOpen);
	ST_SCRIPT_FIELD(PhotoHour);
	ST_SCRIPT_FIELD(SunElevation);
	ST_SCRIPT_FIELD(SunAzimuth);
	ST_SCRIPT_FIELD(Bookmark);
	ST_SCRIPT_FIELD(DistanceWalked);
	ST_SCRIPT_FIELD(Grounded);
	ST_SCRIPT_FIELD(HeightAboveGround);
	ST_SCRIPT_FIELD(ToursCompleted);
}

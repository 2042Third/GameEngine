#pragma once

// The High Desert Outpost's director, on the "Outpost" entity: it sets the time of day (a station: the sun, the fog, the
// exposure, the fire and lanterns, and which of the six baked reflection probes light the yard and the hut), runs the
// camera tour over the six shots of Tools/GenerateShowcaseTerrain.py (the markers Shot<N>.From, .Key, .To and .Look),
// hands over to a first-person walk on any input, flies (Tab) and opens the photo mode (P). It moves the "Player" entity
// (the body's yaw) and its child "Eye" (the camera's pitch). Everything it does is logged with the prefix "Showcase: ",
// which the sample's regression (StrataTests/Editor/ShowcaseCommands.json.in) reads.

#include "Stations.h"
#include "TourPath.h"

#include "StrataScript/StrataScript.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace Showcase
{

	// The photo mode's f-numbers, the aperture slider's whole steps 0 to 8.
	constexpr std::array<float, 9> c_FNumbers = { 1.4f, 2.0f, 2.8f, 4.0f, 5.6f, 8.0f, 11.0f, 16.0f, 22.0f };

}

class Outpost : public Strata::Script
{
public:
	// The scene's entities it drives (the layout script sets them).
	Strata::Entity Sun;
	Strata::Entity Fog;
	Strata::Entity PostProcess;
	Strata::Entity Player;
	Strata::Entity Eye;
	Strata::Entity FadeImage;
	Strata::Entity Caption;
	Strata::Entity DawnYard;
	Strata::Entity DawnHut;
	Strata::Entity GoldenYard;
	Strata::Entity GoldenHut;
	Strata::Entity DuskYard;
	Strata::Entity DuskHut;
	Strata::Entity FireLight;
	Strata::Entity LanternALight;
	Strata::Entity LanternBLight;
	Strata::Entity Embers;
	Strata::Entity WickA;
	Strata::Entity WickB;
	// The photo mode's panel and the widgets whose values it shows (their EventTarget is this entity).
	Strata::Entity PhotoPanel;
	Strata::Entity TimeSlider;
	Strata::Entity TimeLabel;
	Strata::Entity ExposureSlider;
	Strata::Entity ExposureLabel;
	Strata::Entity FovSlider;
	Strata::Entity FovLabel;
	Strata::Entity FocusSlider;
	Strata::Entity FocusLabel;
	Strata::Entity ApertureSlider;
	Strata::Entity ApertureLabel;
	Strata::Entity DofToggle;
	Strata::Entity AutoFocusToggle;
	Strata::Entity QualityLabel;
	Strata::Entity AntiAliasingLabel;

	// Tuning
	bool TourOnStart = true;        // Start with the tour (else standing at the Player's place)
	int32_t StartShot = 1;          // The shot a tour starts with (1-6)
	float TourSpeed = 1.0f;         // Times real time
	bool LoopTour = true;           // After shot 6, shot 1 again
	float FadeSeconds = 0.75f;      // Tour time of the fades at both ends of each shot
	float WalkSpeed = 1.5f;         // m/s
	float RunSpeed = 3.2f;
	float FlySpeed = 6.0f;
	float FastFactor = 4.0f;        // Flying with Run held
	float LookSpeed = 0.12f;        // Degrees per pixel of mouse movement
	float StickTurnSpeed = 120.0f;  // Degrees per second at the right stick's full deflection
	float WalkRadius = 140.0f;      // The walk stays within this distance of the origin
	float ProbeHoldSeconds = 1.0f;  // How long the old station's probes stay on after a switch (seconds of real time)
	float StationWaitSeconds = 5.0f; // The longest the tour waits for the probes' captures to load
	float PhotoHourMax = Showcase::c_LastHour;
	float SunIlluminance = Showcase::c_SunIlluminance;
	float FireCandela = 400.0f;     // The local lights at dusk
	float LanternACandela = 40.0f;
	float LanternBCandela = 25.0f;

	// State (the regression reads these)
	int32_t Mode = 1;               // 0 tour, 1 walk, 2 fly
	int32_t Shot = 0;               // The tour's shot (1-6), 0 outside the tour
	int32_t Station = 1;            // 0 Dawn, 1 Golden, 2 Dusk: whose probes light the scene
	int32_t CapturesFound = 0;      // The probes' captures that exist
	int32_t CapturesResident = 0;   // Those loaded completely
	bool PhotoOpen = false;
	float PhotoHour = 17.75f;       // The photo mode's time of day
	float SunElevation = 0.0f;
	float SunAzimuth = 0.0f;
	int32_t Bookmark = 0;           // The last bookmark taken (1-7), 0 none
	float DistanceWalked = 0.0f;    // Meters walked
	bool Grounded = false;          // Walking with ground under the feet
	float HeightAboveGround = 0.0f; // The eye's height above the ground under it
	int32_t ToursCompleted = 0;

	void OnCreate() override;
	void OnReload() override;
	void OnUpdate(float deltaTime) override;
	void OnUIClick(Strata::Entity widget) override;
	void OnUIValueChanged(Strata::Entity widget, float value) override;

	// Starts the tour at StartShot from black, once the stations' captures are in.
	void StartTour();
private:
	struct ShotMarkers
	{
		glm::vec3 From = glm::vec3(0.0f);
		glm::vec3 Key = glm::vec3(0.0f);
		glm::vec3 To = glm::vec3(0.0f);
		glm::vec3 Look = glm::vec3(0.0f);
	};

	// Stations (Outpost.cpp)
	void FindMarkers();
	void RequestCaptures();
	void PollCaptures();
	bool AreStationsReady() const { return m_StationsReady; }
	// Switches the probes to a station's (the old ones held on for ProbeHoldSeconds) and applies its light.
	void SetStation(Showcase::StationId station, bool immediately = false);
	// Only the probes, for the photo mode's time of day, whose light comes from SampleDay.
	void SwitchProbes(Showcase::StationId station, bool immediately);
	void ApplyLight(const Showcase::StationValues& values);
	void SetProbes(Showcase::StationId station, bool active, bool raised);
	void EndProbeHold();
	void UpdateProbeHold(float realDeltaTime);
	void MeasureGround();
	void HandleInput();
	Strata::Entity GetProbe(Showcase::StationId station, bool hut) const;

	// The tour (OutpostTour.cpp)
	void UpdateTour(float realDeltaTime);
	void EndTour(const char* how);
	void BeginShot(int32_t number);
	void PlaceCamera(const glm::vec3& eye, const glm::vec3& look, float fov);
	void SetFade(float alpha);
	void SetCaption(std::string_view text);

	// The walk and the flight (OutpostWalk.cpp)
	void SetMode(int32_t mode);
	void UpdateLook(float realDeltaTime, bool mouse);
	void ApplyLook();
	void UpdateWalk(float realDeltaTime);
	void UpdateFlight(float realDeltaTime);
	void DropToGround();
	void TakeBookmark(int32_t number);
	glm::vec3 GetEyePosition() const;

	// The photo mode (OutpostPhoto.cpp)
	void SetPhotoOpen(bool open);
	void SetPhotoHour(float hour);
	void ApplyExposure();
	void UpdateAutofocus();
	void RefreshPhotoPanel();
	void CycleAntiAliasing();
	void ToggleHideUI();

	std::array<ShotMarkers, Showcase::c_ShotCount> m_Shots {};
	ShotMarkers m_Macro {};
	bool m_MarkersFound = false;
	std::array<Strata::AssetHandle, 6> m_Captures {};
	bool m_StationsReady = false;
	float m_SinceCreate = 0.0f;
	int32_t m_HoldStation = -1;     // The station whose probes stay on during a switch, -1 none
	float m_HoldLeft = 0.0f;
	bool m_TourWaiting = false;     // Black, waiting for the stations before the first shot
	float m_TourTime = 0.0f;        // Seconds of tour time since shot 1 began
	float m_Fade = -1.0f;
	uint32_t m_GroundMask = 0;
	float m_Yaw = 0.0f;             // Degrees about +Y (0 looks along -Z)
	float m_Pitch = 0.0f;           // Degrees, positive up
	float m_ExposureOffset = 0.0f;  // The photo mode's exposure slider (EV)
	bool m_Autofocus = true;
	bool m_UIHidden = false;
	float m_ExposureBase = 0.0f;    // The time of day's exposure compensation
	bool m_Refreshing = false;      // The panel is being set from the state: its widgets' events are ours
};

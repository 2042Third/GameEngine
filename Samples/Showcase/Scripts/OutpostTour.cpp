// Outpost's camera tour: six shots of about 80 seconds, each a dolly through its key, the time of day changing in the
// black between shots 1 and 2 and between 5 and 6.

#include "Outpost.h"

#include <algorithm>
#include <cmath>

using namespace Strata;
using namespace Showcase;

void Outpost::StartTour()
{
	if (!m_MarkersFound)
		FindMarkers();
	const int32_t start = std::clamp(StartShot, 1, c_ShotCount);
	Mode = 0;
	Shot = 0;
	m_TourWaiting = true;
	m_TourTime = GetShotStart(start);
	SetFade(1.0f);
	SetCaption("");
	// The first shot's time of day comes under the black, while the stations' captures load.
	SetStation(GetShot(start).Station);
	Log::Info("Showcase: tour started");
}

void Outpost::UpdateTour(float realDeltaTime)
{
	if (m_TourWaiting)
	{
		if (!AreStationsReady())
			return;
		m_TourWaiting = false;
	}
	else
	{
		m_TourTime += realDeltaTime * std::max(TourSpeed, 0.0f);
	}
	if (m_TourTime >= GetTourSeconds())
	{
		ToursCompleted++;
		if (!LoopTour)
		{
			Mode = 1;
			Shot = 0;
			SetFade(0.0f);
			SetCaption("");
			Log::Info("Showcase: tour finished");
			return;
		}
		m_TourTime = std::fmod(m_TourTime, GetTourSeconds());
		Log::Info("Showcase: tour looped");
	}
	const TourMoment moment = GetTourMoment(m_TourTime, FadeSeconds);
	if (moment.Shot != Shot)
		BeginShot(moment.Shot);
	const ShotMarkers& markers = m_Shots[static_cast<size_t>(moment.Shot - 1)];
	PlaceCamera(DollyPosition(markers.From, markers.Key, markers.To, moment.Progress), markers.Look, GetShot(moment.Shot).Fov);
	SetFade(moment.Fade);
}

void Outpost::BeginShot(int32_t number)
{
	const TourShot& shot = GetShot(number);
	Shot = number;
	// A cut: the station changes in the black (shots 1-2 and 5-6), which the fade hides.
	SetStation(shot.Station);
	SetCaption(shot.Caption);
	Log::Info("Showcase: shot ", number, " (", shot.Caption, ")");
}

void Outpost::PlaceCamera(const glm::vec3& eye, const glm::vec3& look, float fov)
{
	if (!Player.IsValid() || !Eye.IsValid())
		return;
	const glm::vec3 direction = look - eye;
	const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
	if (horizontal < 1e-4f && std::abs(direction.y) < 1e-4f)
		return;
	// The body turns with the yaw, the eye with the pitch (as the walk does).
	const float yaw = std::atan2(-direction.x, -direction.z);
	const float pitch = std::atan2(direction.y, horizontal);
	const glm::quat body = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::vec3 eyeOffset = Eye.GetTransform().GetTranslation();
	Player.GetTransform().SetWorldPosition(eye - body * eyeOffset);
	Player.GetTransform().SetWorldRotation(body);
	Eye.GetTransform().SetRotation(glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f)));
	if (Eye.GetProperty<float>("Camera", "PerspectiveFOV").value_or(0.0f) != fov)
		Eye.SetProperty("Camera", "PerspectiveFOV", fov);
}

void Outpost::SetFade(float alpha)
{
	if (alpha == m_Fade || !FadeImage.IsValid())
		return;
	m_Fade = alpha;
	FadeImage.SetActive(alpha > 0.0f);
	FadeImage.GetUI().SetColor(glm::vec4(0.0f, 0.0f, 0.0f, alpha));
}

void Outpost::SetCaption(std::string_view text)
{
	if (!Caption.IsValid())
		return;
	Caption.SetActive(!text.empty());
	Caption.GetUI().SetText(text);
}

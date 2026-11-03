// Outpost's photo mode (P): a panel of the time of day, the exposure, the lens (field of view, focus, f-number, depth of
// field, autofocus), the players' graphics options (the quality preset and anti-aliasing, through Graphics) and seven
// bookmarks. The walk or flight goes on meanwhile, with the pointer free; H hides the game's UI for a clean picture.

#include "Outpost.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>

using namespace Strata;
using namespace Showcase;

namespace
{

	constexpr float c_FocusNear = 0.5f;   // The focus slider's 0 (meters)
	constexpr float c_FocusRange = 400.0f; // Its 1 is c_FocusNear times this (200 m)
	constexpr QualityPreset c_Presets[] = { QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra };
	constexpr const char* c_PresetToggles[] = { "PresetLow", "PresetMedium", "PresetHigh", "PresetUltra" };

	std::string Format(const char* format, double value)
	{
		char text[64];
		std::snprintf(text, sizeof(text), format, value);
		return text;
	}

	std::string FormatHour(float hour)
	{
		const int32_t minutes = static_cast<int32_t>(std::lround(hour * 60.0f));
		char text[16];
		std::snprintf(text, sizeof(text), "%02d:%02d", static_cast<int>(minutes / 60), static_cast<int>(minutes % 60));
		return text;
	}

	std::string OptionTitle(std::string_view option, std::string_view fallback)
	{
		const std::optional<GraphicsOption> found = Graphics::FindOption(option);
		return found && !found->Title.empty() ? found->Title : std::string(fallback);
	}

	void SetText(Entity label, const std::string& text)
	{
		if (label.IsValid())
			label.GetUI().SetText(text);
	}

}

void Outpost::SetPhotoOpen(bool open)
{
	const bool changed = open != PhotoOpen;
	PhotoOpen = open;
	if (PhotoPanel.IsValid())
		PhotoPanel.SetActive(open && !m_UIHidden);
	Input::SetCursorMode(Mode == 1 && !open ? CursorMode::Locked : CursorMode::Normal);
	if (open)
		RefreshPhotoPanel();
	if (changed)
		Log::Info("Showcase: photo mode ", open ? "on" : "off");
}

void Outpost::SetPhotoHour(float hour)
{
	PhotoHour = std::clamp(hour, c_FirstHour, std::max(PhotoHourMax, c_FirstHour));
	// The light follows the hour; the reflections snap to the nearest of the three baked stations.
	ApplyLight(SampleDay(PhotoHour));
	SwitchProbes(NearestStation(PhotoHour), false);
	RefreshPhotoPanel();
}

void Outpost::ApplyExposure()
{
	if (PostProcess.IsValid())
		PostProcess.SetProperty("PostProcess", "Exposure", m_ExposureBase + m_ExposureOffset);
}

void Outpost::UpdateAutofocus()
{
	if (!Eye.IsValid())
		return;
	const glm::vec3 eye = GetEyePosition();
	const glm::vec3 forward = Eye.GetTransform().GetWorldRotation() * glm::vec3(0.0f, 0.0f, -1.0f);
	const std::optional<RaycastHit> hit = Physics::Raycast(eye, forward, 1000.0f, m_GroundMask);
	const float distance = hit ? std::max(hit->Distance, c_FocusNear) : c_FocusNear * c_FocusRange;
	const float current = Eye.GetProperty<float>("Camera", "FocusDistance").value_or(0.0f);
	if (std::abs(distance - current) > 0.01f * current)
	{
		Eye.SetProperty("Camera", "FocusDistance", distance);
		if (PhotoOpen)
			RefreshPhotoPanel();
	}
}

void Outpost::RefreshPhotoPanel()
{
	if (!PhotoOpen || !PhotoPanel.IsValid())
		return;
	m_Refreshing = true;
	const float fov = Eye.GetProperty<float>("Camera", "PerspectiveFOV").value_or(50.0f);
	const float focus = Eye.GetProperty<float>("Camera", "FocusDistance").value_or(10.0f);
	const float aperture = Eye.GetProperty<float>("Camera", "Aperture").value_or(2.8f);
	size_t stop = 0;
	for (size_t index = 0; index < c_FNumbers.size(); index++)
	{
		if (std::abs(c_FNumbers[index] - aperture) < std::abs(c_FNumbers[stop] - aperture))
			stop = index;
	}
	if (TimeSlider.IsValid())
		TimeSlider.GetUI().SetValue(PhotoHour);
	SetText(TimeLabel, "Time of day  " + FormatHour(PhotoHour));
	if (ExposureSlider.IsValid())
		ExposureSlider.GetUI().SetValue(m_ExposureOffset);
	SetText(ExposureLabel, Format("Exposure  %+.1f EV", m_ExposureOffset));
	if (FovSlider.IsValid())
		FovSlider.GetUI().SetValue(fov);
	SetText(FovLabel, Format("Field of view  %.0f", fov));
	if (FocusSlider.IsValid())
		FocusSlider.GetUI().SetValue(std::clamp(std::log(focus / c_FocusNear) / std::log(c_FocusRange), 0.0f, 1.0f));
	SetText(FocusLabel, Format(m_Autofocus ? "Focus  %.1f m (auto)" : "Focus  %.1f m", focus));
	if (ApertureSlider.IsValid())
		ApertureSlider.GetUI().SetValue(static_cast<float>(stop));
	SetText(ApertureLabel, Format("Aperture  f/%.1f", c_FNumbers[stop]));
	if (DofToggle.IsValid())
		DofToggle.GetUI().SetValue(Eye.GetProperty<bool>("Camera", "DepthOfField").value_or(false) ? 1.0f : 0.0f);
	if (AutoFocusToggle.IsValid())
		AutoFocusToggle.GetUI().SetValue(m_Autofocus ? 1.0f : 0.0f);
	// The preset in effect, also one the command line chose (--quality).
	const std::optional<QualityPreset> preset = Graphics::GetPreset();
	for (size_t index = 0; index < std::size(c_Presets); index++)
	{
		if (Entity toggle = Scene::FindEntityByName(c_PresetToggles[index]); toggle.IsValid())
			toggle.GetUI().SetValue(preset == c_Presets[index] ? 1.0f : 0.0f);
	}
	SetText(QualityLabel, OptionTitle("Preset", "Quality") + "  " + std::string(preset ? Graphics::GetPresetName(*preset) : "Custom"));
	SetText(AntiAliasingLabel, OptionTitle("AntiAliasing", "Anti-aliasing") + "  " + Graphics::Get<std::string>("AntiAliasing").value_or("?"));
	m_Refreshing = false;
}

void Outpost::CycleAntiAliasing()
{
	const std::optional<GraphicsOption> option = Graphics::FindOption("AntiAliasing");
	if (!option || option->Choices.empty())
		return;
	const std::string current = Graphics::Get<std::string>("AntiAliasing").value_or("");
	const auto found = std::find(option->Choices.begin(), option->Choices.end(), current);
	const size_t next = found == option->Choices.end() ? 0 : (static_cast<size_t>(found - option->Choices.begin()) + 1) % option->Choices.size();
	if (Graphics::Set("AntiAliasing", option->Choices[next]))
		Log::Info("Showcase: anti-aliasing ", option->Choices[next]);
	RefreshPhotoPanel();
}

void Outpost::ToggleHideUI()
{
	m_UIHidden = !m_UIHidden;
	for (const Entity element : { FadeImage, PhotoPanel })
	{
		// Each element's canvas: the overlay (fade and caption) and the photo mode's.
		if (Entity canvas = element.IsValid() ? element.GetParent() : Entity(); canvas.IsValid())
			canvas.SetActive(!m_UIHidden);
	}
	if (PhotoPanel.IsValid())
		PhotoPanel.SetActive(PhotoOpen && !m_UIHidden);
}

void Outpost::OnUIClick(Entity widget)
{
	const std::string name = widget.GetName();
	if (name == "AntiAliasing")
		CycleAntiAliasing();
	else if (name.size() == 9 && name.rfind("Bookmark", 0) == 0 && name[8] >= '1' && name[8] <= '7')
		TakeBookmark(name[8] - '0');
}

void Outpost::OnUIValueChanged(Entity widget, float value)
{
	if (m_Refreshing)
		return;
	if (widget == TimeSlider)
	{
		SetPhotoHour(value);
	}
	else if (widget == ExposureSlider)
	{
		m_ExposureOffset = value;
		ApplyExposure();
		RefreshPhotoPanel();
	}
	else if (widget == FovSlider)
	{
		Eye.SetProperty("Camera", "PerspectiveFOV", value);
		RefreshPhotoPanel();
	}
	else if (widget == FocusSlider)
	{
		// Focusing by hand turns autofocus off.
		m_Autofocus = false;
		Eye.SetProperty("Camera", "FocusDistance", c_FocusNear * std::pow(c_FocusRange, std::clamp(value, 0.0f, 1.0f)));
		RefreshPhotoPanel();
	}
	else if (widget == ApertureSlider)
	{
		const size_t stop = static_cast<size_t>(std::clamp(std::lround(value), 0L, static_cast<long>(c_FNumbers.size()) - 1));
		Eye.SetProperty("Camera", "Aperture", c_FNumbers[stop]);
		RefreshPhotoPanel();
	}
	else if (widget == DofToggle)
	{
		Eye.SetProperty("Camera", "DepthOfField", value > 0.5f);
	}
	else if (widget == AutoFocusToggle)
	{
		m_Autofocus = value > 0.5f;
		RefreshPhotoPanel();
	}
	else
	{
		// The quality toggles: a group, whose toggle turned off reports 0 as another turns on (ignored).
		const std::string name = widget.GetName();
		for (size_t index = 0; index < std::size(c_Presets); index++)
		{
			if (name == c_PresetToggles[index] && value > 0.5f && Graphics::SetPreset(c_Presets[index]))
			{
				Log::Info("Showcase: graphics preset ", std::string(Graphics::GetPresetName(c_Presets[index])));
				RefreshPhotoPanel();
			}
		}
	}
}

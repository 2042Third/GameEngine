#include "Playground.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// The playground's dynamic bodies, by tag.
	constexpr const char* c_BodyTags[] = { "Crate", "Domino", "Ball", "Ghost", "Pin" };

	// The ball prefab's scale (a sphere collider of radius 0.5 at half size: 0.25 m). An instance's root gets the scale it
	// is spawned with instead of the prefab's, so every spawn passes it: at scale 1 the balls of a pour, 0.6 m apart, would
	// start overlapping, and the physics would throw them apart (over the pit's walls).
	constexpr float c_BallScale = 0.5f;

	int32_t CountBodies()
	{
		int32_t count = 0;
		for (const char* tag : c_BodyTags)
			count += static_cast<int32_t>(Scene::FindEntitiesByTag(tag).size());
		return count;
	}

	std::string FormatMilliseconds(float milliseconds)
	{
		const int32_t tenths = static_cast<int32_t>(std::lround(milliseconds * 10.0f));
		return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
	}

}

void Playground::OnCreate()
{
	RequestAssets();
	if (Entity top = Scene::FindEntityByName("TowerTop"); top.IsValid())
		TowerTopStart = top.GetTransform().GetWorldPosition();
	m_Time = 0.0f;
	m_DemoStage = 0;
	Bodies = CountBodies();
	Log::Info("Playground: ", Bodies, " bodies");
}

void Playground::OnReload()
{
	// Fields kept their values; what is not a field starts over.
	Reloads++;
	RequestAssets();
	m_DominoRunLogged = DominoesDown >= 60;
	m_DemoStage = AutoDemo ? 3 : 0; // The demo does not run its stations again after a reload
	Log::Info("Playground: scripts reloaded (", Reloads, ")");
}

void Playground::RequestAssets()
{
	Assets::RequestLoad(BallPrefab);
	Assets::RequestLoad(GoalSound);
	Assets::RequestLoad(ShotSound);
}

void Playground::OnUpdate(float deltaTime)
{
	m_Time += deltaTime;
	if (AutoDemo)
	{
		// The stations one after another, so the game shows something without input.
		if (m_DemoStage == 0 && m_Time >= DemoStart)
		{
			StartDominoes();
			m_DemoStage = 1;
		}
		else if (m_DemoStage == 1 && m_Time >= DemoStart + 4.0f)
		{
			Pour();
			m_DemoStage = 2;
		}
		else if (m_DemoStage == 2 && m_Time >= DemoStart + 8.0f)
		{
			FeedGoal();
			Launch();
			m_DemoStage = 3;
		}
	}

	if (Input::IsKeyPressed(Key::R))
		Reset();
	if (Input::IsMouseButtonPressed(Mouse::ButtonLeft) && !UI::IsPointerOverUI())
	{
		glm::vec3 origin;
		glm::vec3 direction;
		if (GetPointerRay(origin, direction))
			Shoot(origin, direction);
	}
	// Once a frame: a slow frame runs several fixed steps, and counting 600 bodies after each would slow it further.
	UpdateCounts();
	UpdateHud(Time::GetUnscaledDeltaTime());
}

void Playground::OnFixedUpdate(float)
{
	// A spawned ball has its body from the step after it was made: give it its speed then, and measure it a step later.
	if (m_MeasuredShot.IsValid())
	{
		LastShotForwardSpeed = glm::dot(m_MeasuredShot.GetRigidBody().GetLinearVelocity(), m_ShotForward);
		m_MeasuredShot = Entity();
	}
	if (m_PendingShot.IsValid() && m_PendingShot.GetRigidBody().SetLinearVelocity(m_PendingVelocity))
	{
		m_MeasuredShot = m_PendingShot;
		m_PendingShot = Entity();
	}
	UpdateGrab();
}

void Playground::UpdateCounts()
{
	Bodies = CountBodies();

	// Sideways only: the stack settles a little, a toppled tower moves meters.
	if (Entity top = Scene::FindEntityByName("TowerTop"); top.IsValid())
	{
		const glm::vec3 offset = top.GetTransform().GetWorldPosition() - TowerTopStart;
		TowerTopDrift = glm::length(glm::vec2(offset.x, offset.z));
	}

	int32_t down = 0;
	for (const Entity& domino : Scene::FindEntitiesByTag("Domino"))
	{
		const glm::vec3 up = domino.GetTransform().GetWorldRotation() * glm::vec3(0.0f, 1.0f, 0.0f);
		if (up.y < 0.7071f)
			down++;
	}
	DominoesDown = down;
	if (DominoesDown >= 60 && !m_DominoRunLogged)
	{
		m_DominoRunLogged = true;
		Log::Info("Playground: domino run complete (", DominoesDown, " of 64 down)");
	}

	int32_t inPit = 0;
	int32_t escaped = 0;
	for (const Entity& ball : Scene::FindEntitiesByTag("Ball"))
	{
		const glm::vec3 position = ball.GetTransform().GetWorldPosition();
		if (std::abs(position.x) > ArenaHalfSize || std::abs(position.z) > ArenaHalfSize || position.y < -1.0f)
			escaped++;
		else if (std::abs(position.x - PitCenter.x) < PitHalfSize && std::abs(position.z - PitCenter.z) < PitHalfSize && position.y < 4.0f)
			inPit++;
	}
	BallsInPit = inPit;
	BallsEscaped = escaped;

	int32_t onFloor = 0;
	for (const Entity& ghost : Scene::FindEntitiesByTag("Ghost"))
	{
		const glm::vec3 position = ghost.GetTransform().GetWorldPosition();
		const bool inside = std::abs(position.x - PitCenter.x) < PitHalfSize && std::abs(position.z - PitCenter.z) < PitHalfSize;
		const bool resting = glm::length(ghost.GetRigidBody().GetLinearVelocity()) < 0.5f;
		if (inside && position.y < PitCenter.y + 0.6f && resting)
			onFloor++;
	}
	GhostsOnPitFloor = onFloor;
}

void Playground::UpdateHud(float unscaledDeltaTime)
{
	// The frame time, smoothed over about half a second.
	m_FrameMs += (unscaledDeltaTime * 1000.0f - m_FrameMs) * std::min(1.0f, unscaledDeltaTime * 2.0f);
	if (BodiesText.IsValid())
		BodiesText.GetUI().SetText("Bodies: " + std::to_string(Bodies));
	if (ScoreText.IsValid())
		ScoreText.GetUI().SetText("Goal: " + std::to_string(Score) + "   Dominoes: " + std::to_string(DominoesDown) + " / 64");
	if (FrameText.IsValid())
		FrameText.GetUI().SetText("Frame: " + FormatMilliseconds(m_FrameMs) + " ms");
	if (ShotsText.IsValid())
		ShotsText.GetUI().SetText("Shots: " + std::to_string(Shots));
}

bool Playground::GetPointerRay(glm::vec3& outOrigin, glm::vec3& outDirection) const
{
	Entity camera = Scene::GetPrimaryCamera();
	if (!camera.IsValid())
		return false;
	TransformComponent transform = camera.GetTransform();
	outOrigin = transform.GetWorldPosition();
	const glm::vec3 forward = transform.GetForward();
	const glm::vec3 right = transform.GetRight();
	const glm::vec3 up = transform.GetUp();
	const glm::uvec2 window = Game::GetWindowSize();
	const glm::vec2 mouse = Input::GetMousePosition();
	if (window.x == 0 || window.y == 0 || mouse.x < 0.0f || mouse.y < 0.0f)
	{
		outDirection = forward;
		return true;
	}
	// The cursor's place on the camera's image plane (vertical field of view in degrees).
	const float fov = glm::radians(camera.GetProperty<float>("Camera", "PerspectiveFOV").value_or(45.0f));
	const float aspect = static_cast<float>(window.x) / static_cast<float>(window.y);
	const float x = (2.0f * mouse.x / static_cast<float>(window.x) - 1.0f) * std::tan(fov * 0.5f) * aspect;
	const float y = (1.0f - 2.0f * mouse.y / static_cast<float>(window.y)) * std::tan(fov * 0.5f);
	outDirection = glm::normalize(forward + right * x + up * y);
	return true;
}

void Playground::Shoot(const glm::vec3& origin, const glm::vec3& direction)
{
	if (!Assets::IsLoaded(BallPrefab))
		return; // Still loading: requested in OnCreate
	Entity ball = Scene::Instantiate(BallPrefab, origin + direction * 1.5f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(c_BallScale));
	if (!ball.IsValid())
		return;
	Shots++;
	m_PendingShot = ball;
	m_PendingVelocity = direction * (ShotSpeed * std::clamp(ShotPower, 0.25f, 1.0f));
	m_ShotForward = Scene::GetPrimaryCamera().GetTransform().GetForward();
	Audio::PlayOneShot(ShotSound, 0.6f);
}

void Playground::UpdateGrab()
{
	if (!Input::IsKeyDown(Key::E))
	{
		m_Grabbed = Entity();
		return;
	}
	glm::vec3 origin;
	glm::vec3 direction;
	if (!GetPointerRay(origin, direction))
		return;
	if (!m_Grabbed.IsValid())
	{
		// The body under the cursor, if it is one of the playground's.
		const std::optional<RaycastHit> hit = Physics::Raycast(origin, direction, 60.0f);
		if (!hit || !hit->HitEntity.IsValid())
			return;
		const std::string tag = hit->HitEntity.GetTag();
		if (tag != "Crate" && tag != "Domino" && tag != "Ball" && tag != "Ghost" && tag != "Pin")
			return;
		m_Grabbed = hit->HitEntity;
		m_GrabDistance = std::min(hit->Distance, GrabDistance);
	}
	// Pulled toward a point in front of the camera with a speed that closes the gap within a few steps.
	const glm::vec3 target = origin + direction * m_GrabDistance;
	RigidBody body = m_Grabbed.GetRigidBody();
	glm::vec3 velocity = (target - m_Grabbed.GetTransform().GetWorldPosition()) * 10.0f;
	const float speed = glm::length(velocity);
	if (speed > 20.0f)
		velocity *= 20.0f / speed;
	body.SetLinearVelocity(velocity);
	body.SetAngularVelocity(body.GetAngularVelocity() * 0.9f);
}

void Playground::SpawnBalls(const glm::vec3& center, float spread, int32_t count, uint32_t seed)
{
	if (!Assets::IsLoaded(BallPrefab) || count <= 0)
		return;
	Random random(seed);
	std::vector<glm::mat4> transforms;
	transforms.reserve(static_cast<size_t>(count));
	// A loose column of balls, each layer offset at random, 0.6 m apart so none starts touching another.
	const int32_t perRow = std::max(1, static_cast<int32_t>(spread * 2.0f / 0.6f));
	for (int32_t index = 0; index < count; index++)
	{
		const int32_t layer = index / (perRow * perRow);
		const int32_t cell = index % (perRow * perRow);
		const glm::vec3 position = center + glm::vec3(-spread + 0.3f + static_cast<float>(cell % perRow) * 0.6f + random.Range(-0.04f, 0.04f),
			static_cast<float>(layer) * 0.6f, -spread + 0.3f + static_cast<float>(cell / perRow) * 0.6f + random.Range(-0.04f, 0.04f));
		transforms.push_back(glm::scale(glm::translate(glm::mat4(1.0f), position), glm::vec3(c_BallScale)));
	}
	Scene::InstantiateMany(BallPrefab, transforms);
}

void Playground::StartDominoes()
{
	Entity first = Scene::FindEntityByName("Domino 1");
	if (!first.IsValid())
		return;
	TransformComponent transform = first.GetTransform();
	const glm::vec3 top = transform.GetWorldPosition() + transform.GetUp() * 0.45f;
	first.GetRigidBody().AddImpulseAtPosition(transform.GetForward() * DominoImpulse, top);
	m_DominoRunLogged = false;
	Log::Info("Playground: dominoes started");
}

void Playground::Pour()
{
	SpawnBalls(PitCenter + glm::vec3(0.0f, 3.0f, 0.0f), 3.6f, PourCount, m_SpawnSeed++);
	Log::Info("Playground: poured ", PourCount, " balls");
}

void Playground::FeedGoal()
{
	SpawnBalls(FunnelCenter + glm::vec3(0.0f, 6.0f, 0.0f), 1.2f, FeedCount, m_SpawnSeed++);
	Log::Info("Playground: fed the goal ", FeedCount, " balls");
}

void Playground::DropGhosts()
{
	int32_t index = 0;
	for (const Entity& ghost : Scene::FindEntitiesByTag("Ghost"))
	{
		const glm::vec3 position = PitCenter + glm::vec3(-1.5f + static_cast<float>(index % 4), 3.5f + static_cast<float>(index / 4) * 1.0f, -0.5f + static_cast<float>(index / 4));
		RigidBody body = ghost.GetRigidBody();
		body.Teleport(position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		body.SetLinearVelocity(glm::vec3(0.0f));
		body.SetAngularVelocity(glm::vec3(0.0f));
		index++;
	}
	Log::Info("Playground: dropped ", index, " ghosts");
}

void Playground::Launch()
{
	SpawnBalls(LaunchPadCenter + glm::vec3(0.0f, 1.5f, 0.0f), 0.6f, 3, m_SpawnSeed++);
}

void Playground::Reset()
{
	// The scene as it was saved: every body back where it started, spawned balls gone.
	Log::Info("Playground: reset");
	Time::SetTimeScale(1.0f);
	Game::ReloadScene();
}

void Playground::AddScore(const glm::vec3& position)
{
	Score++;
	Audio::PlayOneShotAt(GoalSound, position);
	Log::Info("Playground: goal scored (", Score, ")");
}

void Playground::OnUIClick(Entity widget)
{
	const std::string name = widget.GetName();
	if (name == "StartDominoes")
		StartDominoes();
	else if (name == "Pour")
		Pour();
	else if (name == "FeedGoal")
		FeedGoal();
	else if (name == "DropGhosts")
		DropGhosts();
	else if (name == "Launch")
		Launch();
	else if (name == "Reset")
		Reset();
}

void Playground::OnUIValueChanged(Entity widget, float value)
{
	const std::string name = widget.GetName();
	if (name == "SlowMotion")
	{
		Time::SetTimeScale(value > 0.5f ? SlowMotionScale : 1.0f);
		Log::Info("Playground: slow motion ", value > 0.5f ? "on" : "off");
	}
	else if (name == "ShotPower")
	{
		ShotPower = std::clamp(value, 0.25f, 1.0f);
	}
}

ST_SCRIPT_CLASS(Playground)
{
	ST_SCRIPT_FIELD(AutoDemo);
	ST_SCRIPT_FIELD(DemoStart);
	ST_SCRIPT_FIELD(ShotSpeed);
	ST_SCRIPT_FIELD(ShotPower);
	ST_SCRIPT_FIELD(SlowMotionScale);
	ST_SCRIPT_FIELD(GrabDistance);
	ST_SCRIPT_FIELD(DominoImpulse);
	ST_SCRIPT_FIELD(PourCount);
	ST_SCRIPT_FIELD(FeedCount);
	ST_SCRIPT_FIELD(PitCenter);
	ST_SCRIPT_FIELD(PitHalfSize);
	ST_SCRIPT_FIELD(FunnelCenter);
	ST_SCRIPT_FIELD(LaunchPadCenter);
	ST_SCRIPT_FIELD(ArenaHalfSize);
	ST_SCRIPT_FIELD(BallPrefab);
	ST_SCRIPT_FIELD(GoalSound);
	ST_SCRIPT_FIELD(ShotSound);
	ST_SCRIPT_FIELD(BodiesText);
	ST_SCRIPT_FIELD(ScoreText);
	ST_SCRIPT_FIELD(FrameText);
	ST_SCRIPT_FIELD(ShotsText);
	ST_SCRIPT_FIELD(Bodies);
	ST_SCRIPT_FIELD(TowerTopDrift);
	ST_SCRIPT_FIELD(TowerTopStart);
	ST_SCRIPT_FIELD(DominoesDown);
	ST_SCRIPT_FIELD(BallsInPit);
	ST_SCRIPT_FIELD(BallsEscaped);
	ST_SCRIPT_FIELD(GhostsOnPitFloor);
	ST_SCRIPT_FIELD(Score);
	ST_SCRIPT_FIELD(Shots);
	ST_SCRIPT_FIELD(LastShotForwardSpeed);
	ST_SCRIPT_FIELD(Reloads);
}

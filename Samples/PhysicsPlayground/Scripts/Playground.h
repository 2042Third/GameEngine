#pragma once

#include "StrataScript/StrataScript.h"

#include <cstdint>

// The playground's manager, on the "Game" entity: it counts what the stations do (the fields the HUD shows and tests
// read), runs the tool panel's buttons, the demo that plays the stations by itself, shooting balls from the camera and
// grabbing bodies with the mouse.
//
// The bodies it watches are found by tag: "Crate" (the tower and the pyramid; the tower's top crate is named "TowerTop"),
// "Domino" (named "Domino 1" to "Domino 64" along the run), "Ball" (the pit's balls and every ball spawned), "Ghost" (on the
// Ghost collision layer, which does not collide with the Ball layer) and "Pin".
class Playground : public Strata::Script
{
public:
	// Tuning
	bool AutoDemo = true;           // Pushes the dominoes, pours and feeds the goal by itself (DemoStart seconds after start)
	float DemoStart = 2.0f;
	float ShotSpeed = 24.0f;        // m/s of a shot ball at full power
	float ShotPower = 1.0f;         // The tool panel's slider: 0.25 to 1
	float SlowMotionScale = 0.25f;  // The time scale of the slow-motion toggle
	float GrabDistance = 12.0f;     // How far in front of the camera a grabbed body is held at most
	float DominoImpulse = 1.5f;     // N*s pushing the first domino at its top
	int32_t PourCount = 100;        // Balls a pour drops into the pit
	int32_t FeedCount = 5;          // Balls Feed Goal drops into the funnel
	glm::vec3 PitCenter = glm::vec3(10.0f, 0.0f, -10.0f);
	float PitHalfSize = 4.5f;       // Inner half width of the pit
	glm::vec3 FunnelCenter = glm::vec3(12.0f, 0.0f, 8.0f);
	glm::vec3 LaunchPadCenter = glm::vec3(-14.0f, 3.2f, 15.0f);
	float ArenaHalfSize = 20.0f;
	Strata::AssetHandle BallPrefab; // The ball every pour, feed, launch and shot spawns
	Strata::AssetHandle GoalSound;
	Strata::AssetHandle ShotSound;

	// The HUD
	Strata::Entity BodiesText;
	Strata::Entity ScoreText;
	Strata::Entity FrameText;
	Strata::Entity ShotsText;

	// What the stations did (the HUD, and the sample's regression test)
	int32_t Bodies = 0;             // Dynamic bodies of the playground
	float TowerTopDrift = 0.0f;     // Meters the tower's top crate moved sideways from where it started
	glm::vec3 TowerTopStart = glm::vec3(0.0f);
	int32_t DominoesDown = 0;       // Dominoes tipped more than 45 degrees
	int32_t BallsInPit = 0;
	int32_t BallsEscaped = 0;       // Balls outside the arena or below its floor
	int32_t GhostsOnPitFloor = 0;   // Ghost boxes resting on the pit's floor, under the balls
	int32_t Score = 0;              // Balls the goal's basket caught
	int32_t Shots = 0;
	float LastShotForwardSpeed = 0.0f; // The newest shot's speed along the camera's forward, the step after it was fired
	int32_t Reloads = 0;            // Hot reloads of the script module while playing

	void OnCreate() override;
	void OnReload() override;
	void OnUpdate(float deltaTime) override;
	void OnFixedUpdate(float fixedDeltaTime) override;
	void OnUIClick(Strata::Entity widget) override;
	void OnUIValueChanged(Strata::Entity widget, float value) override;

	// A ball reached the goal's basket (GoalBasket).
	void AddScore(const glm::vec3& position);
	// The stations' actions, from the tool panel, the keys or the demo.
	void StartDominoes();
	void Pour();
	void FeedGoal();
	void DropGhosts();
	void Launch();
	void Reset();
	void Shoot(const glm::vec3& origin, const glm::vec3& direction);
private:
	void RequestAssets();
	void UpdateCounts();
	void UpdateHud(float unscaledDeltaTime);
	void UpdateGrab();
	// The ray from the primary camera through the mouse cursor (the view's center without a cursor position).
	bool GetPointerRay(glm::vec3& outOrigin, glm::vec3& outDirection) const;
	void SpawnBalls(const glm::vec3& center, float spread, int32_t count, uint32_t seed);
private:
	float m_Time = 0.0f;            // Seconds since the demo started (scaled time)
	int32_t m_DemoStage = 0;
	bool m_DominoRunLogged = false;
	float m_FrameMs = 16.7f;
	Strata::Entity m_PendingShot;   // A ball whose velocity is set once its body exists
	glm::vec3 m_PendingVelocity = glm::vec3(0.0f);
	Strata::Entity m_MeasuredShot;  // The shot whose speed the next fixed step measures
	glm::vec3 m_ShotForward = glm::vec3(0.0f, 0.0f, -1.0f);
	Strata::Entity m_Grabbed;
	float m_GrabDistance = 0.0f;
	uint32_t m_SpawnSeed = 1;
};

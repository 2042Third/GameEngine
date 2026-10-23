#pragma once

/*
 * Strata script ABI: the C interface between the engine and game script modules.
 *
 * A script module is a shared library built from game code against StrataScriptCore only; it never links the
 * engine. The engine loads it and calls two exported functions:
 *
 *   uint32_t StrataScript_GetABIVersion(void);
 *   uint32_t StrataScript_Load(const StrataScriptHostAPI* host, uint32_t hostABIVersion, StrataScriptModuleAPI* outModule);
 *
 * The host API table gives scripts access to the engine; the module API describes the script classes (outModule->StructSize
 * tells the module how large the host's struct is, see StrataScriptModuleAPI). Everything
 * crossing this boundary is plain data: no C++ types, no exceptions, strings as (pointer, size) in UTF-8, entities and
 * assets as 64-bit ids, math as float arrays (quaternions are x, y, z, w).
 *
 * Compatibility rules:
 *   - ST_SCRIPT_ABI_VERSION changes on every incompatible change (a changed signature, struct layout or meaning,
 *     removed or reordered members). The engine refuses modules built against another version.
 *   - New host functions, module callbacks and descriptor members are only ever appended to the end of their struct,
 *     which keeps the version. Every extensible struct starts with its StructSize, so either side can tell whether
 *     the other one knows a member (see ST_SCRIPT_HAS_MEMBER).
 *   - Calls into the module return a StrataScriptResult. Modules never let C++ exceptions escape: they report the
 *     message through StrataScriptHostAPI::ReportException and return StrataScriptResult_Exception.
 *   - Everything is single-threaded: the engine calls the module on its main thread, and the module calls the host
 *     only from within those calls.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ST_SCRIPT_ABI_VERSION 1u

#if defined(_WIN32)
	#define ST_SCRIPT_EXPORT __declspec(dllexport)
#else
	#define ST_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

#if defined(__cplusplus)
	#define ST_SCRIPT_EXTERN_C extern "C"
#else
	#define ST_SCRIPT_EXTERN_C
#endif

/* Names of the functions every script module exports. */
#define ST_SCRIPT_GET_ABI_VERSION_SYMBOL "StrataScript_GetABIVersion"
#define ST_SCRIPT_LOAD_SYMBOL "StrataScript_Load"

/*
 * Windows: the structured exception a module raises when its code calls abort() (a failed assert(), std::abort()). The
 * engine reports it as a crash of the current call instead of the C runtime ending the process. Modules built with the
 * SDK raise it from a SIGABRT handler installed in their own, statically linked C runtime (ScriptModuleEntry.cpp).
 */
#define ST_SCRIPT_ABORT_EXCEPTION_CODE 0xE0535441u

/* Layout checks of the ABI's structs, on every compiler that builds the engine or a module. */
#if defined(__cplusplus)
	#define ST_SCRIPT_DETAIL_STATIC_ASSERT(condition, message) static_assert(condition, message)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
	#define ST_SCRIPT_DETAIL_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#else
	#define ST_SCRIPT_DETAIL_STATIC_ASSERT(condition, message)
#endif

/* True when the struct behind `pointer` (which starts with a StructSize member) is large enough to contain `member`. */
#define ST_SCRIPT_HAS_MEMBER(type, pointer, member) \
	((pointer)->StructSize >= offsetof(type, member) + sizeof(((type*)0)->member))

#if defined(__cplusplus)
extern "C"
{
#endif

	/* Opaque per-scene context handed to scripts; every scene-related host function takes it. */
	typedef struct StrataScriptContext StrataScriptContext;

	/* A script instance created by the module (opaque to the engine). */
	typedef void* StrataScriptInstance;

	/* Entity UUID; 0 is the null entity. */
	typedef uint64_t StrataScriptEntityID;
	/* Asset handle (UUID); 0 is the null asset. */
	typedef uint64_t StrataScriptAssetHandle;

	/* UTF-8 string, not necessarily null-terminated. Data may be null when Size is 0. */
	typedef struct StrataScriptString
	{
		const char* Data;
		uint64_t Size;
	} StrataScriptString;

	/* Result of every call into the module. Stored as uint32_t. */
	typedef enum StrataScriptResult
	{
		StrataScriptResult_Ok = 0,
		StrataScriptResult_Exception = 1,       /* A C++ exception was caught; the message went to ReportException */
		StrataScriptResult_ABIMismatch = 2,     /* StrataScript_Load: the host uses another ABI version */
		StrataScriptResult_InvalidArgument = 3  /* E.g. a field index out of range or a value of the wrong type */
	} StrataScriptResult;

	typedef enum StrataScriptLogLevel
	{
		StrataScriptLogLevel_Trace = 0,
		StrataScriptLogLevel_Info = 1,
		StrataScriptLogLevel_Warn = 2,
		StrataScriptLogLevel_Error = 3
	} StrataScriptLogLevel;

	/* Types of script field and component property values. Stored as uint32_t. */
	typedef enum StrataScriptValueType
	{
		StrataScriptValueType_Empty = 0,
		StrataScriptValueType_Bool,
		StrataScriptValueType_Int,    /* Int; component Int, UInt and Enum properties */
		StrataScriptValueType_Float,
		StrataScriptValueType_Vec2,
		StrataScriptValueType_Vec3,   /* Also component Color3 properties */
		StrataScriptValueType_Vec4,   /* Also component Color4 properties */
		StrataScriptValueType_Quat,   /* x, y, z, w */
		StrataScriptValueType_String,
		StrataScriptValueType_Entity,
		StrataScriptValueType_Asset
	} StrataScriptValueType;

	typedef struct StrataScriptValue
	{
		uint32_t Type; /* StrataScriptValueType */
		uint32_t Padding;
		union
		{
			bool Bool;
			int64_t Int;
			float Float;
			float Vector[4];           /* Vec2/Vec3/Vec4 use the first 2/3/4 elements; Quat is x, y, z, w */
			uint64_t ID;               /* Entity or Asset */
			StrataScriptString String;
		} As;
	} StrataScriptValue;

	/* Local or world transform. Rotation is a unit quaternion (x, y, z, w). */
	typedef struct StrataScriptTransform
	{
		float Translation[3];
		float Rotation[4];
		float Scale[3];
	} StrataScriptTransform;

	/* Parts of a transform written by SetTransform/SetWorldTransform. Combine as a bit mask. */
	typedef enum StrataScriptTransformPart
	{
		StrataScriptTransformPart_Translation = 1,
		StrataScriptTransformPart_Rotation = 2,
		StrataScriptTransformPart_Scale = 4,
		StrataScriptTransformPart_All = 7
	} StrataScriptTransformPart;

	/* A hit of a physics ray: the entity owning the body, the world space point and surface normal, and the distance from
	 * the ray's origin. Its layout is frozen for the ABI version: modules pass arrays of it (RaycastAll) that the engine
	 * fills, and they carry no size, so a member appended later would overrun the buffers of older modules. More data
	 * needs a new struct and function. */
	typedef struct StrataScriptRaycastHit
	{
		StrataScriptEntityID Entity;
		float Point[3];
		float Normal[3];
		float Distance;
		uint32_t Padding;
	} StrataScriptRaycastHit;

	ST_SCRIPT_DETAIL_STATIC_ASSERT(sizeof(StrataScriptRaycastHit) == 40 && offsetof(StrataScriptRaycastHit, Entity) == 0
		&& offsetof(StrataScriptRaycastHit, Point) == 8 && offsetof(StrataScriptRaycastHit, Normal) == 20
		&& offsetof(StrataScriptRaycastHit, Distance) == 32 && offsetof(StrataScriptRaycastHit, Padding) == 36,
		"StrataScriptRaycastHit's layout is part of the ABI");

	/* A contact passed to the contact callbacks of StrataScriptClassDesc. Engine memory, valid during the call; read members
	 * appended later only when StructSize covers them. Other is the entity on the other side (it may be destroyed already
	 * when the contact ended because of that); Point is in world space (the last known one when a contact ends) and Normal
	 * points from the script's entity towards the other. */
	typedef struct StrataScriptCollision
	{
		uint32_t StructSize; /* sizeof(StrataScriptCollision) as built into the engine */
		uint32_t Padding;
		StrataScriptEntityID Other;
		float Point[3];
		float Normal[3];
	} StrataScriptCollision;

	ST_SCRIPT_DETAIL_STATIC_ASSERT(sizeof(StrataScriptCollision) == 40 && offsetof(StrataScriptCollision, StructSize) == 0
		&& offsetof(StrataScriptCollision, Other) == 8 && offsetof(StrataScriptCollision, Point) == 16
		&& offsetof(StrataScriptCollision, Normal) == 28, "StrataScriptCollision's layout is part of the ABI");

	/* Where a character controller stands (StrataScriptCharacterState::GroundState). */
	typedef enum StrataScriptGroundState
	{
		StrataScriptGroundState_OnGround = 0,      /* On walkable ground (not steeper than its MaxSlopeDegrees) */
		StrataScriptGroundState_OnSteepGround = 1, /* On ground too steep to walk on: it slides down */
		StrataScriptGroundState_NotSupported = 2,  /* Touching something that does not hold it up (pressed against a wall) */
		StrataScriptGroundState_InAir = 3          /* Touching nothing */
	} StrataScriptGroundState;

	/* A character controller after its last fixed step (CharacterGetState). Its layout is frozen for the ABI version: modules
	 * pass it for the engine to fill without a size, so a member appended later would overrun the structs of older modules.
	 * More data needs a new struct and function. */
	typedef struct StrataScriptCharacterState
	{
		uint32_t GroundState;              /* StrataScriptGroundState */
		uint32_t Padding;
		StrataScriptEntityID GroundEntity; /* The entity owning what it stands on (0 in the air) */
		float GroundNormal[3];             /* The surface normal of what it stands on (zero in the air) */
		float GroundVelocity[3];           /* The velocity of what it stands on (a moving platform) */
		float Velocity[3];                 /* How fast it moved in its last fixed step, after collisions */
		uint32_t Padding2;
	} StrataScriptCharacterState;

	ST_SCRIPT_DETAIL_STATIC_ASSERT(sizeof(StrataScriptCharacterState) == 56 && offsetof(StrataScriptCharacterState, GroundEntity) == 8
		&& offsetof(StrataScriptCharacterState, GroundNormal) == 16 && offsetof(StrataScriptCharacterState, GroundVelocity) == 28
		&& offsetof(StrataScriptCharacterState, Velocity) == 40 && offsetof(StrataScriptCharacterState, Padding2) == 52,
		"StrataScriptCharacterState's layout is part of the ABI");

	/* The cursor over the game (SetCursorMode). */
	typedef enum StrataScriptCursorMode
	{
		StrataScriptCursorMode_Normal = 0,
		StrataScriptCursorMode_Hidden = 1, /* Hidden over the game's window */
		StrataScriptCursorMode_Locked = 2  /* Hidden and kept in the game's window; mouse deltas keep coming (first-person cameras) */
	} StrataScriptCursorMode;

	/* How the game's window shows (SetWindowMode). */
	typedef enum StrataScriptWindowMode
	{
		StrataScriptWindowMode_Windowed = 0,
		StrataScriptWindowMode_Fullscreen = 1, /* Exclusive: the display switches to the game's resolution */
		StrataScriptWindowMode_Borderless = 2  /* A window covering the display at the desktop's resolution */
	} StrataScriptWindowMode;

	/*
	 * The game UI's actions (UIIsActionPressed, UIIsActionDown); the engine's UIAction values. They are the input actions of
	 * the project's map UI (UI/Submit, UI/Cancel, UI/Up...), whose bindings the project and the players may change.
	 */
	typedef enum StrataScriptUIAction
	{
		StrataScriptUIAction_Submit = 0,
		StrataScriptUIAction_Cancel = 1,
		StrataScriptUIAction_Up = 2,
		StrataScriptUIAction_Down = 3,
		StrataScriptUIAction_Left = 4,
		StrataScriptUIAction_Right = 5
	} StrataScriptUIAction;

	/* The devices a binding capture listens to (InputBeginBindingCapture): bits, combined with |. */
	typedef enum StrataScriptInputDevices
	{
		StrataScriptInputDevices_Keyboard = 1,
		StrataScriptInputDevices_Mouse = 2,
		StrataScriptInputDevices_Gamepad = 4
	} StrataScriptInputDevices;

	/*
	 * Engine services for scripts. Unless noted otherwise, functions taking a context only work while the engine is
	 * calling into the module for that context, on the engine's main thread; otherwise they fail (returning false, 0
	 * or an empty result). Failures are logged by the engine.
	 *
	 * Functions returning text or arrays copy into a caller-provided buffer: they write at most `capacity` elements and
	 * return the total number available, so the caller can retry with a larger buffer. Text is not null-terminated.
	 */
	typedef struct StrataScriptHostAPI
	{
		uint32_t StructSize; /* sizeof(StrataScriptHostAPI) as built into the engine */
		uint32_t ABIVersion;

		/* Diagnostics (callable from any thread). */
		void (*Log)(uint32_t level, StrataScriptString message);
		/* Explains the next StrataScriptResult_Exception returned to the engine. */
		void (*ReportException)(StrataScriptString message);

		/* Time. Delta times are scaled by the time scale; the elapsed time is the scene's simulation time in seconds. */
		float (*GetDeltaTime)(StrataScriptContext* context);
		float (*GetFixedDeltaTime)(StrataScriptContext* context);
		double (*GetElapsedTime)(StrataScriptContext* context);
		uint64_t (*GetFrameIndex)(StrataScriptContext* context);
		float (*GetTimeScale)(StrataScriptContext* context);
		void (*SetTimeScale)(StrataScriptContext* context, float timeScale);

		/* Entities. Destruction is deferred to the end of the frame; the entity stays valid until then. */
		StrataScriptEntityID (*CreateEntity)(StrataScriptContext* context, StrataScriptString name, StrataScriptEntityID parent);
		void (*DestroyEntity)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*IsEntityValid)(StrataScriptContext* context, StrataScriptEntityID entity);
		StrataScriptEntityID (*FindEntityByName)(StrataScriptContext* context, StrataScriptString name);
		uint32_t (*FindEntitiesByTag)(StrataScriptContext* context, StrataScriptString tag, StrataScriptEntityID* outEntities, uint32_t capacity);
		uint64_t (*GetEntityName)(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity);
		bool (*SetEntityName)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString name);
		/* An empty tag removes the Tag component. */
		uint64_t (*GetEntityTag)(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity);
		bool (*SetEntityTag)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString tag);
		bool (*IsEntityActive)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*IsEntityActiveInHierarchy)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*SetEntityActive)(StrataScriptContext* context, StrataScriptEntityID entity, bool active);
		StrataScriptEntityID (*GetParent)(StrataScriptContext* context, StrataScriptEntityID entity);
		/* A null parent makes the entity a root. */
		bool (*SetParent)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptEntityID parent, bool keepWorldTransform);
		uint32_t (*GetChildren)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptEntityID* outChildren, uint32_t capacity);

		/*
		 * Components, by registered component name ("Transform", "Camera", ...) and property name, case-insensitive.
		 * For String properties GetProperty copies the text into stringBuffer (at most stringCapacity bytes) and sets
		 * outValue->As.String to { stringBuffer, full size }.
		 */
		bool (*HasComponent)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component);
		bool (*AddComponent)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component);
		bool (*RemoveComponent)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component);
		bool (*GetProperty)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component, StrataScriptString property,
			StrataScriptValue* outValue, char* stringBuffer, uint64_t stringCapacity);
		bool (*SetProperty)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component, StrataScriptString property,
			const StrataScriptValue* value);

		/* Transform fast paths. `parts` selects what Set* writes (StrataScriptTransformPart mask). */
		bool (*GetTransform)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptTransform* outTransform);
		bool (*SetTransform)(StrataScriptContext* context, StrataScriptEntityID entity, const StrataScriptTransform* transform, uint32_t parts);
		bool (*GetWorldTransform)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptTransform* outTransform);
		bool (*SetWorldTransform)(StrataScriptContext* context, StrataScriptEntityID entity, const StrataScriptTransform* transform, uint32_t parts);

		/* Scene queries and prefab/model instantiation. Instantiate returns the root entity of the created hierarchy;
		 * transform (optional) becomes the root's local transform. It never waits for loading: for an asset that is not
		 * loaded yet it starts the load and returns 0 (IsAssetLoaded tells when to try again). */
		StrataScriptEntityID (*GetPrimaryCamera)(StrataScriptContext* context);
		uint32_t (*GetRootEntities)(StrataScriptContext* context, StrataScriptEntityID* outEntities, uint32_t capacity);
		StrataScriptEntityID (*Instantiate)(StrataScriptContext* context, StrataScriptAssetHandle asset, StrataScriptEntityID parent,
			const StrataScriptTransform* transform);

		/* Assets, by path relative to the project's asset directory. RequestAssetLoad starts loading an asset and keeps it
		 * loaded while the scene plays (see ReleaseAsset). */
		StrataScriptAssetHandle (*FindAsset)(StrataScriptContext* context, StrataScriptString path);
		bool (*IsAssetLoaded)(StrataScriptContext* context, StrataScriptAssetHandle asset);
		bool (*RequestAssetLoad)(StrataScriptContext* context, StrataScriptAssetHandle asset);

		/* Scripts on entities, by class name. GetScriptInstance returns an instance created by this module (or null);
		 * AddScript creates the instance immediately, its OnCreate runs before its first update. */
		StrataScriptInstance (*GetScriptInstance)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className);
		bool (*HasScript)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className);
		bool (*AddScript)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className);
		bool (*RemoveScript)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className);

		/* Input. Key and mouse button codes match the engine's (GLFW values). Positions are in viewport pixels. */
		bool (*IsKeyDown)(StrataScriptContext* context, uint32_t key);
		bool (*IsKeyPressed)(StrataScriptContext* context, uint32_t key);
		bool (*IsKeyReleased)(StrataScriptContext* context, uint32_t key);
		bool (*IsMouseButtonDown)(StrataScriptContext* context, uint32_t button);
		bool (*IsMouseButtonPressed)(StrataScriptContext* context, uint32_t button);
		bool (*IsMouseButtonReleased)(StrataScriptContext* context, uint32_t button);
		void (*GetMousePosition)(StrataScriptContext* context, float outPosition[2]);
		void (*GetMouseDelta)(StrataScriptContext* context, float outDelta[2]);
		void (*GetScrollDelta)(StrataScriptContext* context, float outDelta[2]);

		/*
		 * Physics bodies (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). A body is an
		 * active entity with a RigidBody component and colliders; the functions fail for other entities. Vectors are in
		 * world space, angular values in radians. Velocities can be read from any body, but only dynamic bodies accept
		 * velocities, forces and impulses. Forces and torques act during the next fixed step, impulses change the velocity
		 * at once. A transform written through SetTransform/SetWorldTransform moves the body at the next fixed step;
		 * Teleport moves it (and its entity) at once, keeping its velocities, so queries see it there right away.
		 */
		bool (*GetLinearVelocity)(StrataScriptContext* context, StrataScriptEntityID entity, float outVelocity[3]);
		bool (*SetLinearVelocity)(StrataScriptContext* context, StrataScriptEntityID entity, const float velocity[3]);
		bool (*GetAngularVelocity)(StrataScriptContext* context, StrataScriptEntityID entity, float outVelocity[3]);
		bool (*SetAngularVelocity)(StrataScriptContext* context, StrataScriptEntityID entity, const float velocity[3]);
		bool (*AddForce)(StrataScriptContext* context, StrataScriptEntityID entity, const float force[3]);
		bool (*AddForceAtPosition)(StrataScriptContext* context, StrataScriptEntityID entity, const float force[3], const float worldPosition[3]);
		bool (*AddImpulse)(StrataScriptContext* context, StrataScriptEntityID entity, const float impulse[3]);
		bool (*AddImpulseAtPosition)(StrataScriptContext* context, StrataScriptEntityID entity, const float impulse[3], const float worldPosition[3]);
		bool (*AddTorque)(StrataScriptContext* context, StrataScriptEntityID entity, const float torque[3]);
		bool (*AddAngularImpulse)(StrataScriptContext* context, StrataScriptEntityID entity, const float impulse[3]);
		/* rotation: x, y, z, w (normalized by the engine). */
		bool (*Teleport)(StrataScriptContext* context, StrataScriptEntityID entity, const float position[3], const float rotation[4]);

		/*
		 * Physics queries, against the bodies as of the last fixed step (or Teleport). layerMask selects RigidBody layers
		 * (bit n: layer n; 0xFFFFFFFF: every layer). Triggers are skipped unless includeTriggers is set. Rays: the direction
		 * need not be normalized, maxDistance must be positive (infinity is clamped to 1e5), ignoreEntity is skipped (0 or
		 * an entity that does not exist: none), and a ray starting inside a convex collider does not hit it. Results name
		 * the entity owning the body (a RigidBody entity, also for colliders on its descendants).
		 */
		/* The closest hit; false if nothing is hit. outHit may be null. */
		bool (*Raycast)(StrataScriptContext* context, const float origin[3], const float direction[3], float maxDistance, uint32_t layerMask,
			StrataScriptEntityID ignoreEntity, bool includeTriggers, StrataScriptRaycastHit* outHit);
		/* The closest hit on every body along the ray, sorted by distance. */
		uint32_t (*RaycastAll)(StrataScriptContext* context, const float origin[3], const float direction[3], float maxDistance, uint32_t layerMask,
			StrataScriptEntityID ignoreEntity, bool includeTriggers, StrataScriptRaycastHit* outHits, uint32_t capacity);
		/* The entities whose bodies overlap a sphere or an oriented box (rotation x, y, z, w), in a deterministic order. */
		uint32_t (*OverlapSphere)(StrataScriptContext* context, const float center[3], float radius, uint32_t layerMask, bool includeTriggers,
			StrataScriptEntityID* outEntities, uint32_t capacity);
		uint32_t (*OverlapBox)(StrataScriptContext* context, const float center[3], const float halfExtents[3], const float rotation[4], uint32_t layerMask,
			bool includeTriggers, StrataScriptEntityID* outEntities, uint32_t capacity);

		/*
		 * Audio (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). Sources are entities
		 * with an AudioSource component, which holds the clip, volume, pitch, looping and 3D settings. The functions fail
		 * (false, 0) while the scene plays without audio, for entities without an AudioSource component or inactive ones,
		 * and for clips that are not audio clips. AudioPlay starts the clip or resumes it (a clip that is still loading
		 * starts once it is ready); AudioPause keeps the position, AudioStop rewinds; AudioIsPlaying is also true while the
		 * start waits for the clip or the scene is paused. Positions are in seconds.
		 */
		bool (*AudioPlay)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*AudioPause)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*AudioStop)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*AudioIsPlaying)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*AudioSeek)(StrataScriptContext* context, StrataScriptEntityID entity, float seconds);
		float (*AudioGetPlaybackPosition)(StrataScriptContext* context, StrataScriptEntityID entity);
		/* Sounds that belong to no entity: they pause and stop with the scene. Without spatialization, or at a world position.
		 * volume >= 0 and pitch > 0 (1: the clip's own). A clip that takes longer than a quarter second to load is dropped. */
		bool (*AudioPlayOneShot)(StrataScriptContext* context, StrataScriptAssetHandle clip, float volume, float pitch);
		bool (*AudioPlayOneShotAt)(StrataScriptContext* context, StrataScriptAssetHandle clip, const float position[3], float volume, float pitch);
		/* The engine-wide master volume (1: unchanged; negative values count as 0). It outlives the scene, like a game's
		 * sound option; the editor restores its own when play mode stops. */
		void (*AudioSetMasterVolume)(StrataScriptContext* context, float volume);
		float (*AudioGetMasterVolume)(StrataScriptContext* context);

		/*
		 * Game flow (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). Both are requests
		 * honored once the current frame's update is done (scripts keep running until then); quitting wins over loading and
		 * later requests replace earlier ones. QuitGame ends the game with an exit code: an exported game exits with it, the
		 * editor stops play mode. LoadScene replaces the running scene with a scene asset (every entity of the current scene
		 * goes away), or restarts the running scene for the null handle; it fails for assets that are not scenes. The scene
		 * and everything it uses load in the background while the running scene goes on (GetSceneLoadProgress); it replaces
		 * the running scene once all of it is in.
		 */
		void (*QuitGame)(StrataScriptContext* context, int32_t exitCode);
		bool (*LoadScene)(StrataScriptContext* context, StrataScriptAssetHandle scene);

		/*
		 * Asset residency (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). An asset the
		 * playing scene requested with RequestAssetLoad stays loaded (pinned: never evicted to meet memory budgets) until
		 * ReleaseAsset or the end of play; requests do not add up, one release ends the scene's claim. ReleaseAsset returns
		 * false when the scene holds no request for the asset (never requested, or released already).
		 */
		bool (*ReleaseAsset)(StrataScriptContext* context, StrataScriptAssetHandle asset);

		/*
		 * Scene streaming (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use).
		 * LoadSceneAdditive loads a scene asset into the playing scene next to what it holds (a sub-scene) and returns the
		 * sub-scene's ID (0 when it cannot start: not a scene asset). The asset and everything it uses load in the background;
		 * then its entities are created over the following frames (a bounded number per frame), keeping the IDs they have in
		 * the asset (a sub-scene whose IDs the scene has already fails: loading one twice). UnloadSubScene removes a sub-scene:
		 * it cancels its load or destroys its entities with their descendants (at the end of the frame, like DestroyEntity);
		 * false for an unknown ID. IsSubSceneLoaded is true once every entity of the sub-scene exists.
		 */
		uint64_t (*LoadSceneAdditive)(StrataScriptContext* context, StrataScriptAssetHandle scene);
		bool (*UnloadSubScene)(StrataScriptContext* context, uint64_t subScene);
		bool (*IsSubSceneLoaded)(StrataScriptContext* context, uint64_t subScene);
		/* How far the scene LoadScene asked for has loaded, in [0, 1] (a loading bar), or -1 while no switch is under way. */
		float (*GetSceneLoadProgress)(StrataScriptContext* context);
		/* The share of the stored bytes of the asset and everything it uses (its dependency closure) that has loaded, in
		 * [0, 1]: at 1 an instance of a prefab draws complete in its first frame. The first call loads the closure and keeps it
		 * loaded while the scene plays, like RequestAssetLoad (ReleaseAsset ends it). -1 for an asset that does not exist. */
		float (*GetAssetLoadProgress)(StrataScriptContext* context, StrataScriptAssetHandle asset);
		/* Instantiates a prefab or model `count` times, each root placed by a local transform (16 floats per instance, a
		 * column-major 4x4 matrix of a translation, rotation and scale, relative to `parent`, or the world for 0). Writes the
		 * root entities to outEntities (room for `count`) and returns how many it created: `count`, or 0 when it created none
		 * (a degenerate transform, an asset that is not loaded yet - its load starts, as for Instantiate - or a scene too
		 * full). */
		uint32_t (*InstantiateMany)(StrataScriptContext* context, StrataScriptAssetHandle asset, uint32_t count, const float* transforms,
			StrataScriptEntityID parent, StrataScriptEntityID* outEntities);

		/*
		 * Animators (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). An animator is an
		 * entity with an Animator component. The functions fail (false, 0, empty) while the scene plays without animation,
		 * for entities without an Animator component, for states and parameters its controller does not have (or of another
		 * type), and for joints its skeleton does not have. Commands given while the controller still loads apply once it is
		 * loaded. AnimatorPlay jumps to a state, AnimatorCrossFade blends to it over `seconds`; AnimatorPlayClip plays an
		 * animation clip without the controller's transitions until a state is played again. A trigger stays set until a
		 * transition consumes it or AnimatorResetTrigger clears it. The speed is the Animator component's (|speed| <= 100).
		 */
		bool (*AnimatorPlay)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString state);
		bool (*AnimatorPlayClip)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptAssetHandle clip);
		bool (*AnimatorCrossFade)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString state, float seconds);
		bool (*AnimatorSetFloat)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter, float value);
		bool (*AnimatorSetBool)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter, bool value);
		bool (*AnimatorSetTrigger)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter);
		bool (*AnimatorResetTrigger)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter);
		bool (*AnimatorSetSpeed)(StrataScriptContext* context, StrataScriptEntityID entity, float speed);
		/* The name of the state the animator is in (empty while it plays a clip directly or before it plays), copied like
		 * GetEntityName. */
		uint64_t (*AnimatorGetCurrentState)(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity);
		/* The current state's time over its clip's duration, counting loops. */
		float (*AnimatorGetNormalizedTime)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*AnimatorIsInTransition)(StrataScriptContext* context, StrataScriptEntityID entity);
		/* A joint's world transform in the animator's current pose (false before the animator has a pose). */
		bool (*AnimatorGetJointTransform)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString joint,
			StrataScriptTransform* outTransform);

		/*
		 * Character controllers (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). A
		 * controller is an active entity with an enabled CharacterController component and no RigidBody; the functions fail
		 * for other entities, for non-finite values and while the scene plays without physics. What they set takes effect at
		 * the next fixed step. CharacterMove sets the velocity (meters per second, world space) the character walks with
		 * until the next call: the horizontal part moves it along the ground, the vertical part counts only while its
		 * GravityScale is 0. CharacterJump makes it move up at `speed` meters per second at its next fixed step, on the ground
		 * or not. CharacterTeleport moves it and its entity to a world position at once (its feet: the entity's origin),
		 * keeping its walk velocity and dropping its vertical velocity. CharacterGetState reads it after its last fixed step.
		 */
		bool (*CharacterMove)(StrataScriptContext* context, StrataScriptEntityID entity, const float velocity[3]);
		bool (*CharacterJump)(StrataScriptContext* context, StrataScriptEntityID entity, float speed);
		bool (*CharacterTeleport)(StrataScriptContext* context, StrataScriptEntityID entity, const float position[3]);
		bool (*CharacterGetState)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptCharacterState* outState);

		/*
		 * Gamepads (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). Gamepads are numbered
		 * 0 to 3; buttons (0 to 14) and axes (0 to 5) follow the GLFW gamepad mapping, an Xbox-style layout (the SDK's
		 * GamepadButton and GamepadAxis). Sticks range from -1 to 1 with +X right and +Y up, triggers from 0 (released) to 1;
		 * the values are the device's, without a dead zone (the SDK applies one). Like keys, buttons and axes report nothing
		 * from the device while the game has no input focus (simulated input still comes through).
		 */
		bool (*IsGamepadConnected)(StrataScriptContext* context, uint32_t gamepad);
		bool (*IsGamepadButtonDown)(StrataScriptContext* context, uint32_t gamepad, uint32_t button);
		bool (*IsGamepadButtonPressed)(StrataScriptContext* context, uint32_t gamepad, uint32_t button);
		bool (*IsGamepadButtonReleased)(StrataScriptContext* context, uint32_t gamepad, uint32_t button);
		float (*GetGamepadAxis)(StrataScriptContext* context, uint32_t gamepad, uint32_t axis);

		/*
		 * The cursor (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use): a
		 * StrataScriptCursorMode. It applies while the game has the input: in the editor while the game view has the focus
		 * (Shift+F1 frees the cursor, clicking the game view takes it again, stopping play restores Normal).
		 */
		void (*SetCursorMode)(StrataScriptContext* context, uint32_t mode);
		uint32_t (*GetCursorMode)(StrataScriptContext* context);

		/*
		 * The game's window (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). Requests are
		 * checked at once and applied after the frame: a shipped game changes its window, the editor's play mode records them
		 * (editor.status, a notice in the game view) without touching the editor's window, a headless game accepts them.
		 * SetWindowMode takes a StrataScriptWindowMode; SetResolution 1 to 16384 pixels per side. GetWindowSize reports the
		 * pixels the game renders into now (the window's framebuffer, the editor's game view).
		 */
		bool (*SetWindowMode)(StrataScriptContext* context, uint32_t mode);
		bool (*SetResolution)(StrataScriptContext* context, uint32_t width, uint32_t height);
		bool (*SetVSync)(StrataScriptContext* context, bool enabled);
		bool (*GetWindowSize)(StrataScriptContext* context, uint32_t outSize[2]);

		/*
		 * Save files (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use): named byte blobs
		 * in the game's save directory (a shipped game: <user data>/<Game>/Saves; the editor: <project>/.strata/Saves). Names
		 * are 1 to 64 ASCII letters, digits, spaces, '-', '_' and '.' (not at the start or end), without "..": the engine
		 * refuses others, so no name reaches another directory. Writes replace the file atomically (at most 64 MiB). The calls
		 * read and write the disk before they return: keep saves small. ReadSaveFile copies at most `capacity` bytes and
		 * reports the file's size in outSize (false for a file that does not exist); ListSaveFiles copies the names, sorted
		 * and each followed by '\n', following the text buffer protocol.
		 */
		bool (*WriteSaveFile)(StrataScriptContext* context, StrataScriptString name, const void* data, uint64_t size);
		bool (*ReadSaveFile)(StrataScriptContext* context, StrataScriptString name, void* buffer, uint64_t capacity, uint64_t* outSize);
		bool (*DeleteSaveFile)(StrataScriptContext* context, StrataScriptString name);
		uint64_t (*ListSaveFiles)(StrataScriptContext* context, char* buffer, uint64_t capacity);

		/*
		 * Collision layers (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). Bodies and
		 * character controllers are on one of the layers 0 to 31 and collide only when the playing scene's layer matrix (its
		 * copy of the project's collision layers, which the editor's Project Settings edit) lets their layers collide.
		 * PhysicsSetLayersCollide changes the playing scene's matrix (symmetric; a layer may be paired with itself); physics
		 * applies it before its next step and wakes resting bodies. It lasts while the scene plays (the next scene starts with
		 * the project's matrix again) and fails for layers outside 0 to 31. PhysicsGetLayersCollide reads the matrix (false
		 * outside the range). PhysicsGetLayerIndex finds a layer by its name (ignoring case; "Layer <index>" names a layer
		 * without a name of its own): its index, or -1 when the project has no such layer.
		 */
		bool (*PhysicsSetLayersCollide)(StrataScriptContext* context, uint32_t layerA, uint32_t layerB, bool collide);
		bool (*PhysicsGetLayersCollide)(StrataScriptContext* context, uint32_t layerA, uint32_t layerB);
		int32_t (*PhysicsGetLayerIndex)(StrataScriptContext* context, StrataScriptString name);

		/*
		 * The game UI (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). Elements are
		 * entities with UI components; each function fails (false, 0, empty text) for an entity without the component it
		 * needs: text a UIText, the image a UIImage, the color a UIImage (its tint) or else a UIText, interactable a
		 * UISelectable, the value a UISlider (clamped and snapped like input) or a UIToggle (0 or 1; switching one on switches
		 * the others of its group off). Values set here raise no OnUIValueChanged: the callbacks report the player's input.
		 * UIGetText follows the text buffer protocol; colors are display (sRGB) RGBA from 0 to 1. UISetFocus focuses an
		 * interactable selectable shown on a canvas that receives input (0 clears the focus). The pointer and the actions
		 * (StrataScriptUIAction: Submit, Cancel, Up, Down, Left, Right, the input actions of the map UI) are those of the UI's
		 * latest update, which runs before the scripts' OnUpdate.
		 */
		uint64_t (*UIGetText)(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity);
		bool (*UISetText)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString text);
		StrataScriptAssetHandle (*UIGetImage)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*UISetImage)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptAssetHandle texture);
		bool (*UIGetColor)(StrataScriptContext* context, StrataScriptEntityID entity, float outColor[4]);
		bool (*UISetColor)(StrataScriptContext* context, StrataScriptEntityID entity, const float color[4]);
		bool (*UIIsInteractable)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*UISetInteractable)(StrataScriptContext* context, StrataScriptEntityID entity, bool interactable);
		float (*UIGetValue)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*UISetValue)(StrataScriptContext* context, StrataScriptEntityID entity, float value);
		StrataScriptEntityID (*UIGetFocus)(StrataScriptContext* context);
		bool (*UISetFocus)(StrataScriptContext* context, StrataScriptEntityID entity);
		bool (*UIIsPointerOverUI)(StrataScriptContext* context);
		bool (*UIIsActionPressed)(StrataScriptContext* context, uint32_t action);
		bool (*UIIsActionDown)(StrataScriptContext* context, uint32_t action);
		/*
		 * Unscaled time (same block): GetUnscaledDeltaTime is the frame's duration before the time scale (menus and fades
		 * that go on at time scale 0), GetRealTime the sum of those since the scene started playing, in seconds.
		 * GetTypedText copies the text typed during the frame (UTF-8, at most 1,024 bytes, the text buffer protocol).
		 */
		float (*GetUnscaledDeltaTime)(StrataScriptContext* context);
		double (*GetRealTime)(StrataScriptContext* context);
		uint64_t (*GetTypedText)(StrataScriptContext* context, char* buffer, uint64_t capacity);

		/*
		 * World streaming (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use), in a scene
		 * that is a world's persistent scene (its first active World component names a World asset); in other scenes every
		 * call fails and is reported once. Positions are three floats (x, y, z), radii finite and at least 0.
		 * WorldRequestCellsAround streams the cells within the radius of the position on every enabled layer, before the
		 * sources' cells, until WorldReleaseCells or the scene stops: a request ID, 0 on failure (prefetch before a teleport
		 * or a spawn, then poll WorldIsLoadedAround). WorldIsLoadedAround tells whether every existing cell of every enabled
		 * layer within the radius is loaded. WorldSetLayerEnabled turns a layer on or off by name (a disabled layer's cells
		 * unload; false for a name the world does not have); WorldIsLayerEnabled answers 1, 0, or -1 for an unknown layer.
		 */
		uint64_t (*WorldRequestCellsAround)(StrataScriptContext* context, const float* position, float radius);
		bool (*WorldReleaseCells)(StrataScriptContext* context, uint64_t request);
		bool (*WorldIsLoadedAround)(StrataScriptContext* context, const float* position, float radius);
		bool (*WorldSetLayerEnabled)(StrataScriptContext* context, StrataScriptString layer, bool enabled);
		int32_t (*WorldIsLayerEnabled)(StrataScriptContext* context, StrataScriptString layer);

		/*
		 * Input actions (added after the initial set of ABI version 1: check ST_SCRIPT_HAS_MEMBER before use): the project's
		 * named controls, which the players may rebind. An action is named "<Map>/<Action>", or by its bare name when no other
		 * map has an action of that name (ignoring case); unknown and ambiguous names fail and are reported. The state is that
		 * of the current input frame: Down, Pressed (down from this frame on) and Released (up from this frame on).
		 * InputGetActionValue writes (x, y): a Button's x is 1 while down, an Axis' x is its value (-1 to 1), a Vector2 is the
		 * vector (+Y up); zeros on failure. A disabled map's actions read as up from the next input frame, which reports the
		 * release of those held when it was disabled; InputIsActionMapEnabled answers 1, 0, or -1 for an unknown map.
		 * InputGetActionBindings copies the bindings the action uses (the players' when they replaced the
		 * project's), each followed by '\n' (the text buffer protocol); InputSetActionBindings replaces them for the players
		 * from the next input frame on, given as bindings each followed by '\n' (the last one's may be left out; empty text
		 * unbinds the action), and refuses the whole list, changing nothing, when one is no binding, one the action's kind
		 * cannot use, or there are more than 8. InputResetActionBindings restores the project's bindings of an action, or of
		 * every action for an empty name. InputSaveBindings writes the players' bindings into the game's save directory (the
		 * engine's file, outside the save files), from which the program running the game reads them when the game starts.
		 */
		bool (*InputIsActionDown)(StrataScriptContext* context, StrataScriptString action);
		bool (*InputIsActionPressed)(StrataScriptContext* context, StrataScriptString action);
		bool (*InputIsActionReleased)(StrataScriptContext* context, StrataScriptString action);
		bool (*InputGetActionValue)(StrataScriptContext* context, StrataScriptString action, float outValue[2]);
		bool (*InputSetActionMapEnabled)(StrataScriptContext* context, StrataScriptString map, bool enabled);
		int32_t (*InputIsActionMapEnabled)(StrataScriptContext* context, StrataScriptString map);
		uint64_t (*InputGetActionBindings)(StrataScriptContext* context, StrataScriptString action, char* buffer, uint64_t capacity);
		bool (*InputSetActionBindings)(StrataScriptContext* context, StrataScriptString action, StrataScriptString bindings);
		bool (*InputResetActionBindings)(StrataScriptContext* context, StrataScriptString action);
		bool (*InputSaveBindings)(StrataScriptContext* context);

		/*
		 * Binding capture (added after InputSaveBindings, ABI version 1: check ST_SCRIPT_HAS_MEMBER before use), a rebinding
		 * menu's "press a control". InputBeginBindingCapture listens for the next control the player presses on the devices
		 * given (StrataScriptInputDevices bits; none, or bits it does not know, fail and are reported): a key, a mouse button,
		 * a gamepad button, or a stick or trigger pushed beyond half its range. While it listens every action reads as up, and
		 * the control captured reads as up for actions until it is let go, so the press triggers no action. There is one
		 * capture: a later one (the scripts' or the editor's) replaces it. InputPollBindingCapture copies the captured
		 * binding's text (the text buffer protocol), after which the capture is over: 0 while it listens, after it was
		 * cancelled or replaced, or when none runs; a buffer too small for it gets nothing and keeps it for the next call.
		 * InputCancelBindingCapture ends the scripts' capture (false when it was not listening). Play stopping cancels it.
		 */
		bool (*InputBeginBindingCapture)(StrataScriptContext* context, uint32_t devices);
		uint64_t (*InputPollBindingCapture)(StrataScriptContext* context, char* buffer, uint64_t capacity);
		bool (*InputCancelBindingCapture)(StrataScriptContext* context);

		/* New functions are appended here (see the compatibility rules above). */
	} StrataScriptHostAPI;

	typedef struct StrataScriptFieldDesc
	{
		uint32_t StructSize;
		uint32_t Type; /* StrataScriptValueType */
		StrataScriptString Name;
		StrataScriptValue DefaultValue;
	} StrataScriptFieldDesc;

	/*
	 * A script class. Lifetime of an instance: Create, SetField for every serialized override, OnCreate, then the
	 * update callbacks every frame, OnDestroy, Destroy. Callbacks the class does not implement are null.
	 * On hot reload the engine destroys instances without OnDestroy and recreates them in the new module without
	 * OnCreate: fields are restored, then OnReload is called.
	 */
	typedef struct StrataScriptClassDesc
	{
		uint32_t StructSize;
		uint32_t FieldCount;
		StrataScriptString Name;                     /* Unique within the module */
		const StrataScriptFieldDesc* const* Fields;  /* FieldCount pointers */

		uint32_t (*Create)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptInstance* outInstance);
		uint32_t (*Destroy)(StrataScriptInstance instance);
		/* String values returned by GetField stay valid until the next call into the module. */
		uint32_t (*GetField)(StrataScriptInstance instance, uint32_t fieldIndex, StrataScriptValue* outValue);
		uint32_t (*SetField)(StrataScriptInstance instance, uint32_t fieldIndex, const StrataScriptValue* value);

		uint32_t (*OnCreate)(StrataScriptInstance instance);
		uint32_t (*OnUpdate)(StrataScriptInstance instance, float deltaTime);
		uint32_t (*OnFixedUpdate)(StrataScriptInstance instance, float fixedDeltaTime);
		uint32_t (*OnLateUpdate)(StrataScriptInstance instance, float deltaTime);
		uint32_t (*OnDestroy)(StrataScriptInstance instance);
		uint32_t (*OnReload)(StrataScriptInstance instance);

		/*
		 * Contacts of the entity's physics body (added after the initial set of ABI version 1: the engine reads them only when
		 * StructSize covers them). They reach the scripts on the entities owning the two bodies (RigidBody entities, or
		 * colliders without one), after the fixed step that found the change: Enter when the bodies start touching, Exit when
		 * they part or one of them leaves the simulation. The trigger callbacks report contacts where either body is a
		 * trigger, the collision callbacks the others. Contacts begin only for scripts on active entities; every instance
		 * that got an Enter gets its Exit, also when its entity was deactivated meanwhile (deactivation ends its contacts).
		 */
		uint32_t (*OnCollisionEnter)(StrataScriptInstance instance, const StrataScriptCollision* collision);
		uint32_t (*OnCollisionExit)(StrataScriptInstance instance, const StrataScriptCollision* collision);
		uint32_t (*OnTriggerEnter)(StrataScriptInstance instance, const StrataScriptCollision* collision);
		uint32_t (*OnTriggerExit)(StrataScriptInstance instance, const StrataScriptCollision* collision);

		/*
		 * Events of the entity's animator (added after the initial set of ABI version 1: the engine reads it only when
		 * StructSize covers it): the playhead of the animation controller's current state passed an event, named as in the
		 * controller. Called after the animators were evaluated, in the order the events happened; the name is valid during
		 * the call.
		 */
		uint32_t (*OnAnimationEvent)(StrataScriptInstance instance, StrataScriptString name);

		/*
		 * Events of the game UI's widgets (added after the initial set of ABI version 1: the engine reads them only when
		 * StructSize covers them), from the player's input: a button clicked, a slider's or toggle's value changed (the new
		 * value; a toggle's is 0 or 1), a selectable gained or lost the focus. Each reaches the scripts on the widget's entity,
		 * then those on its event target (when it names another entity), once per instance, before the scripts' OnUpdate of
		 * the frame and in the order the inputs happened.
		 */
		uint32_t (*OnUIClick)(StrataScriptInstance instance, StrataScriptEntityID widget);
		uint32_t (*OnUIValueChanged)(StrataScriptInstance instance, StrataScriptEntityID widget, float value);
		uint32_t (*OnUIFocusChanged)(StrataScriptInstance instance, StrataScriptEntityID widget, bool focused);

		/*
		 * An input field's text was submitted (Enter, or a gamepad's Submit, while it was being edited); added after
		 * OnUIFocusChanged and read only when StructSize covers it. It reaches the scripts like the widget events above.
		 */
		uint32_t (*OnUISubmit)(StrataScriptInstance instance, StrataScriptEntityID widget);

		/* New callbacks are appended here. */
	} StrataScriptClassDesc;

	/*
	 * Filled by StrataScript_Load. The host sets StructSize to the size of its StrataScriptModuleAPI (the capacity of the
	 * struct it passes) and zeroes the rest. The module writes at most that many bytes and sets StructSize to the size of
	 * the struct it was built with, so a module built against a newer SDK (same ABI version, members appended) never
	 * writes past the host's struct; either side reads a member only if both sizes cover it. A module refuses a capacity
	 * that cannot hold the members of its ABI version (StrataScriptResult_ABIMismatch). The pointed-to data stays valid
	 * until Unload is called.
	 */
	typedef struct StrataScriptModuleAPI
	{
		uint32_t StructSize;
		uint32_t ABIVersion;
		StrataScriptString Name;
		uint32_t ClassCount;
		uint32_t Padding;
		const StrataScriptClassDesc* const* Classes; /* ClassCount pointers */
		/* Called once before the engine unloads the module, after every instance was destroyed. */
		uint32_t (*Unload)(void);
	} StrataScriptModuleAPI;

	typedef uint32_t (*StrataScriptGetABIVersionFunction)(void);
	typedef uint32_t (*StrataScriptLoadFunction)(const StrataScriptHostAPI* host, uint32_t hostABIVersion, StrataScriptModuleAPI* outModule);

#if defined(__cplusplus)
}
#endif

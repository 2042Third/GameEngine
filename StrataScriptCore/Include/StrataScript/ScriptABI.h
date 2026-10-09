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
	 * the ray's origin. */
	typedef struct StrataScriptRaycastHit
	{
		StrataScriptEntityID Entity;
		float Point[3];
		float Normal[3];
		float Distance;
		uint32_t Padding;
	} StrataScriptRaycastHit;

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

		/* Assets, by path relative to the project's asset directory. */
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
		 * trigger, the collision callbacks the others. Scripts on inactive entities receive none.
		 */
		uint32_t (*OnCollisionEnter)(StrataScriptInstance instance, const StrataScriptCollision* collision);
		uint32_t (*OnCollisionExit)(StrataScriptInstance instance, const StrataScriptCollision* collision);
		uint32_t (*OnTriggerEnter)(StrataScriptInstance instance, const StrataScriptCollision* collision);
		uint32_t (*OnTriggerExit)(StrataScriptInstance instance, const StrataScriptCollision* collision);

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

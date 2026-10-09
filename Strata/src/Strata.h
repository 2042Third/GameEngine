#pragma once

// Umbrella header for Strata applications (editor, runtime, tools).

#include "Strata/Core/Base.h"
#include "Strata/Core/Application.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/CommandLine.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Layer.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Timer.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/UUID.h"
#include "Strata/Core/Version.h"

#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Events/MouseEvent.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"

#include "Strata/Audio/AudioClip.h"
#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Audio/AudioSource.h"

#include "Strata/Input/Input.h"
#include "Strata/Input/KeyCodes.h"
#include "Strata/Input/MouseCodes.h"

#include "Strata/Project/GameManifest.h"
#include "Strata/Project/Project.h"

#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/Texture.h"

#include "Strata/Runtime/GameRuntime.h"

#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

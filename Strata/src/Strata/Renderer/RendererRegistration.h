#pragma once

namespace Strata
{

	// Registers the renderer module with the engine's registries: its components, the loaders of textures, meshes, materials
	// and fonts, the mesh geometry reader, and the objects of the built-in meshes and default material. Called by
	// Engine::RegisterBuiltinModules only.
	void RegisterRendererModule();

	// Registers the renderer's components (MeshRenderer, the lights, SkyLight, PostProcess and Text:
	// RendererComponents.cpp); part of RegisterRendererModule.
	void RegisterRendererComponents();

}

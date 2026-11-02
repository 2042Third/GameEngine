#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Strata
{
	// What scene meshes' vertex streams provide, one input attribute each (Mesh.h): the position stream,
	// MeshVertexAttributes (normal, tangent, UV) and, skinned, MeshSkinVertex (joints, weights).
	enum class SceneVertexAttribute : uint8_t
	{
		Position,
		Normal,
		Tangent,
		TexCoord,
		Joints,
		Weights
	};

	// The input layouts of scene pipelines. Each holds exactly the attributes its vertex shaders read, because the
	// validation layer warns about an attribute no shader consumes: shadow passes (the sun's and the local lights') read
	// the position and the UV (alpha-tested casters), and the skin when skinned.
	enum class SceneVertexLayout : uint8_t
	{
		Mesh,
		SkinnedMesh,
		Shadow,
		SkinnedShadow
	};
	constexpr size_t c_SceneVertexLayoutCount = 4;

	namespace SceneVertexLayoutsDetail
	{
		constexpr SceneVertexAttribute c_Mesh[] = { SceneVertexAttribute::Position, SceneVertexAttribute::Normal,
			SceneVertexAttribute::Tangent, SceneVertexAttribute::TexCoord };
		constexpr SceneVertexAttribute c_SkinnedMesh[] = { SceneVertexAttribute::Position, SceneVertexAttribute::Normal,
			SceneVertexAttribute::Tangent, SceneVertexAttribute::TexCoord, SceneVertexAttribute::Joints, SceneVertexAttribute::Weights };
		constexpr SceneVertexAttribute c_Shadow[] = { SceneVertexAttribute::Position, SceneVertexAttribute::TexCoord };
		constexpr SceneVertexAttribute c_SkinnedShadow[] = { SceneVertexAttribute::Position, SceneVertexAttribute::TexCoord,
			SceneVertexAttribute::Joints, SceneVertexAttribute::Weights };
	}

	// The layout's attributes in order. NVRHI numbers a layout's attributes from 0 in order, so attribute i is read at
	// location i.
	constexpr std::span<const SceneVertexAttribute> GetSceneVertexAttributes(SceneVertexLayout layout)
	{
		switch (layout)
		{
			case SceneVertexLayout::Mesh:          return SceneVertexLayoutsDetail::c_Mesh;
			case SceneVertexLayout::SkinnedMesh:   return SceneVertexLayoutsDetail::c_SkinnedMesh;
			case SceneVertexLayout::Shadow:        return SceneVertexLayoutsDetail::c_Shadow;
			case SceneVertexLayout::SkinnedShadow: return SceneVertexLayoutsDetail::c_SkinnedShadow;
		}
		return {};
	}

	struct SceneVertexEntry
	{
		std::string_view Shader; // The embedded entry's name
		SceneVertexLayout Layout;
	};

	// Every vertex shader scene pipelines draw meshes with, and its layout. SceneRenderer takes a pipeline's layout from
	// here by the pipeline's vertex shader, and Renderer.ShaderModules checks that each entry reads exactly its layout's
	// locations, with the types the attributes' formats give.
	constexpr SceneVertexEntry c_SceneVertexEntries[] = {
		{ "Scene/Mesh.vert", SceneVertexLayout::Mesh },
		{ "Scene/MeshVelocity.vert", SceneVertexLayout::Mesh },
		{ "Scene/MeshSkinned.vert", SceneVertexLayout::SkinnedMesh },
		{ "Scene/MeshSkinnedVelocity.vert", SceneVertexLayout::SkinnedMesh },
		{ "Scene/ShadowMesh.vert", SceneVertexLayout::Shadow },
		{ "Scene/LocalShadowMesh.vert", SceneVertexLayout::Shadow },
		{ "Scene/ShadowMeshSkinned.vert", SceneVertexLayout::SkinnedShadow },
		{ "Scene/LocalShadowMeshSkinned.vert", SceneVertexLayout::SkinnedShadow },
	};

	// The layout scene pipelines with this vertex shader use, or nullopt for a shader c_SceneVertexEntries does not list.
	constexpr std::optional<SceneVertexLayout> FindSceneVertexLayout(std::string_view vertexShader)
	{
		for (const SceneVertexEntry& entry : c_SceneVertexEntries)
		{
			if (entry.Shader == vertexShader)
				return entry.Layout;
		}
		return std::nullopt;
	}
}

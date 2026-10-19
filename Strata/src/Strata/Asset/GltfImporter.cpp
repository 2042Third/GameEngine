#include "stpch.h"
#include "Strata/Asset/GltfImporter.h"

#include "Strata/Animation/AnimationTypes.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/TextureImporter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#if ST_WITH_ANIMATION
	#include "Strata/Animation/AnimationClip.h"
	#include "Strata/Animation/AnimationComponents.h"
	#include "Strata/Animation/Skeleton.h"
#endif

#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		// Extensions the importer reads. EXT_meshopt_compression's buffer views are decoded with meshoptimizer's decoders before
		// anything reads them (GltfImport::DecodeCompressedViews); KHR_mesh_quantization's normalized and integer accessors
		// are read by cgltf's unpacking.
		constexpr const char* c_SupportedExtensions[] = {
			"KHR_lights_punctual", "KHR_materials_emissive_strength", "KHR_materials_unlit", "KHR_texture_transform", "KHR_mesh_quantization",
			"EXT_meshopt_compression"
		};

		// What a model's compressed buffer views may decode to, in all: hostile files must not exhaust memory.
		constexpr uint64_t c_MaxDecodedBytes = 1ull << 30;

		// Limits that keep hostile or broken files from exhausting memory, time or the stack. The geometry an import
		// expands (vertices plus triangle corners over all primitives) is limited relative to the loaded buffers:
		// accessors can describe far more data than a file holds (zero-filled accessors, accessors shared by many
		// primitives).
		constexpr uint64_t c_MaxAccessorElements = 1ull << 30;
		constexpr uint64_t c_MinGeometryBudget = 1ull << 22;  // Elements every file may expand to
		constexpr uint64_t c_GeometryBudgetPerBufferByte = 4; // Elements per loaded buffer byte beyond that
		constexpr uint64_t c_MaxGeometryBudget = 1ull << 28;
		constexpr uint32_t c_MaxHierarchyDepth = 1024;

		// The range of the Scale setting, which editors offer and asset.setImportSettings accepts (a .meta written by hand may
		// hold any positive scale, which imports).
		constexpr float c_MinImportScale = 0.0001f;
		constexpr float c_MaxImportScale = 10000.0f;
		constexpr uint32_t c_MaxLODCount = 8;

		struct ImportSettings
		{
			float Scale = 1.0f;
			bool GenerateLODs = true;
			uint32_t LODCount = 3;
			bool OptimizeMeshes = true;
			bool ImportMaterials = true;
			bool ImportCameras = true;
			bool ImportLights = true;
			uint32_t MaxTextureSize = 0;
			TextureCompression Compression = TextureCompression::Auto; // Of the model's textures
			TextureQuality Quality = TextureQuality::High;
		};

		ImportSettings ReadSettings(const nlohmann::json& json, std::vector<std::string>& warnings)
		{
			ImportSettings settings;
			const float scale = JsonUtils::GetFloat(json, "Scale", settings.Scale);
			if (std::isfinite(scale) && scale > 0.0f)
				settings.Scale = scale;
			else
				warnings.push_back("Scale must be a positive number; using 1");
			settings.GenerateLODs = JsonUtils::GetBool(json, "GenerateLODs", settings.GenerateLODs);
			settings.LODCount = static_cast<uint32_t>(std::min<uint64_t>(JsonUtils::GetUInt(json, "LODCount", settings.LODCount), c_MaxLODCount));
			settings.OptimizeMeshes = JsonUtils::GetBool(json, "OptimizeMeshes", settings.OptimizeMeshes);
			settings.ImportMaterials = JsonUtils::GetBool(json, "ImportMaterials", settings.ImportMaterials);
			settings.ImportCameras = JsonUtils::GetBool(json, "ImportCameras", settings.ImportCameras);
			settings.ImportLights = JsonUtils::GetBool(json, "ImportLights", settings.ImportLights);
			settings.MaxTextureSize = static_cast<uint32_t>(std::min<uint64_t>(JsonUtils::GetUInt(json, "MaxTextureSize", 0), TextureImporter::c_MaxImageDimension));
			if (const nlohmann::json* compression = JsonUtils::Find(json, "TextureCompression"))
			{
				const std::optional<TextureCompression> value = compression->is_string() ? TextureCompressionFromString(compression->get<std::string>()) : std::nullopt;
				if (value)
					settings.Compression = *value;
				else
					warnings.push_back("TextureCompression must be Auto or Uncompressed; using Auto");
			}
			if (const nlohmann::json* quality = JsonUtils::Find(json, "TextureQuality"))
			{
				const std::optional<TextureQuality> value = quality->is_string() ? TextureQualityFromString(quality->get<std::string>()) : std::nullopt;
				if (value)
					settings.Quality = *value;
				else
					warnings.push_back("TextureQuality must be High or Fast; using High");
			}
			return settings;
		}

		const char* ResultToString(cgltf_result result)
		{
			switch (result)
			{
				case cgltf_result_success:         return "success";
				case cgltf_result_data_too_short:  return "data too short";
				case cgltf_result_unknown_format:  return "unknown format";
				case cgltf_result_invalid_json:    return "invalid JSON";
				case cgltf_result_invalid_gltf:    return "invalid glTF";
				case cgltf_result_invalid_options: return "invalid options";
				case cgltf_result_file_not_found:  return "file not found";
				case cgltf_result_io_error:        return "I/O error";
				case cgltf_result_out_of_memory:   return "out of memory";
				case cgltf_result_legacy_gltf:     return "glTF 1.0 is not supported";
				default:                           return "unknown error";
			}
		}

		// Checked arithmetic for sizes declared by the file.
		bool CheckedMultiplyAdd(uint64_t a, uint64_t b, uint64_t c, uint64_t& outResult)
		{
			if (b != 0 && a > (std::numeric_limits<uint64_t>::max() - c) / b)
				return false;
			outResult = a * b + c;
			return true;
		}

		bool IsSupportedImageData(const std::vector<uint8_t>& data)
		{
			// glTF images are PNG or JPEG; other decoders are not exposed to model files.
			static constexpr uint8_t c_PNGSignature[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
			const bool png = data.size() >= sizeof(c_PNGSignature) && std::memcmp(data.data(), c_PNGSignature, sizeof(c_PNGSignature)) == 0;
			const bool jpeg = data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
			return png || jpeg;
		}

		// cgltf reads external buffers through these callbacks: paths are UTF-8 and must stay inside the asset
		// directory (also through symbolic links), so a model cannot pull arbitrary files into the project.
		struct FileReadContext
		{
			const ImportContext* Context = nullptr;
			ImportResult* Result = nullptr;
		};

		cgltf_result ReadFile(const cgltf_memory_options*, const cgltf_file_options* fileOptions, const char* path, cgltf_size* size, void** outData)
		{
			// No exception may cross cgltf's C frames.
			try
			{
				const auto* context = static_cast<const FileReadContext*>(fileOptions->user_data);
				std::optional<std::vector<uint8_t>> bytes = ReadImportDependency(*context->Context, *context->Result, FileSystem::FromUTF8(path));
				if (!bytes)
					return cgltf_result_file_not_found;

				// cgltf asks for the declared byte length (0 or no size = whole file) and trusts the result: a shorter
				// file fails.
				const size_t requested = size ? *size : 0;
				if (requested > bytes->size())
					return cgltf_result_data_too_short;
				const size_t count = requested != 0 ? requested : bytes->size();
				void* data = std::malloc(std::max<size_t>(count, 1));
				if (!data)
					return cgltf_result_out_of_memory;
				if (count != 0)
					std::memcpy(data, bytes->data(), count);
				if (size)
					*size = count;
				*outData = data;
				return cgltf_result_success;
			}
			catch (...)
			{
				return cgltf_result_io_error;
			}
		}

		void ReleaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data)
		{
			std::free(data);
		}

		struct CgltfDataDeleter
		{
			void operator()(cgltf_data* data) const { cgltf_free(data); }
		};

		TextureWrap ToTextureWrap(cgltf_wrap_mode mode)
		{
			switch (mode)
			{
				case cgltf_wrap_mode_clamp_to_edge:   return TextureWrap::Clamp;
				case cgltf_wrap_mode_mirrored_repeat: return TextureWrap::Mirror;
				default:                              return TextureWrap::Repeat;
			}
		}

		class GltfImport
		{
			// An imported mesh: its handle, and whether it was imported with a skin.
			struct MeshImport
			{
				AssetHandle Handle = UUID::Null();
				bool Skinned = false;
			};
		public:
			GltfImport(const ImportContext& context, ImportResult& result)
				: m_Context(context), m_Result(result)
			{
			}

			bool Run(std::string* outError)
			{
				m_Settings = ReadSettings(m_Context.Settings, m_Result.Warnings);
				if (!Load(outError))
					return false;

				// A cancelled import stops between a model's materials (whose textures take the longest) and meshes.
				if (m_Settings.ImportMaterials)
				{
					for (cgltf_size index = 0; index < m_Data->materials_count; index++)
					{
						if (m_Context.IsCancelled())
							return Fail(outError, "cancelled");
						ImportMaterial(index);
					}
				}

#if ST_WITH_ANIMATION
				// The skeleton first: skinned meshes bind to its joints.
				ImportSkeleton();
#endif

				for (cgltf_size index = 0; index < m_Data->meshes_count; index++)
				{
					if (m_Context.IsCancelled())
						return Fail(outError, "cancelled");
					// One mesh per skin it is drawn with (glTF lets nodes draw a mesh with different skins, or without one).
					const std::vector<const cgltf_skin*>& skins = m_MeshSkins[index];
					for (size_t use = 0; use < skins.size(); use++)
					{
						const std::string key = use == 0 ? fmt::format("Mesh/{}", index)
							: fmt::format("Mesh/{}/{}", index, skins[use] ? fmt::format("Skin/{}", cgltf_skin_index(m_Data.get(), skins[use])) : std::string("Rigid"));
						ImportMesh(index, skins[use], key);
					}
				}

#if ST_WITH_ANIMATION
				if (m_Context.IsCancelled())
					return Fail(outError, "cancelled");
				ImportAnimations();
#endif

				if (m_Context.IsCancelled())
					return Fail(outError, "cancelled");
				if (!BuildModel(outError))
					return false;

				for (const std::string& feature : m_SkippedFeatures)
					m_Result.Warnings.push_back(fmt::format("Not imported: {}", feature));
				return true;
			}
		private:
			bool Load(std::string* outError)
			{
				std::optional<std::vector<uint8_t>> fileData = ReadImportSource(m_Context);
				if (!fileData)
					return Fail(outError, "cannot read the file");
				// cgltf references the binary chunk of .glb files in place: the bytes must outlive the parsed data.
				m_FileData = std::move(*fileData);

				m_FileContext.Context = &m_Context;
				m_FileContext.Result = &m_Result;
				cgltf_options options = {};
				options.file.read = ReadFile;
				options.file.release = ReleaseFile;
				options.file.user_data = &m_FileContext;

				cgltf_data* data = nullptr;
				cgltf_result result = cgltf_parse(&options, m_FileData.data(), m_FileData.size(), &data);
				m_Data.reset(data);
				if (result != cgltf_result_success)
					return Fail(outError, fmt::format("parsing failed ({})", ResultToString(result)));

				for (cgltf_size index = 0; index < m_Data->extensions_required_count; index++)
				{
					const std::string_view extension = m_Data->extensions_required[index];
					if (std::find(std::begin(c_SupportedExtensions), std::end(c_SupportedExtensions), extension) == std::end(c_SupportedExtensions))
						return Fail(outError, fmt::format("requires the unsupported extension {}", extension));
				}
				for (cgltf_size index = 0; index < m_Data->extensions_used_count; index++)
				{
					const std::string_view extension = m_Data->extensions_used[index];
					if (std::find(std::begin(c_SupportedExtensions), std::end(c_SupportedExtensions), extension) == std::end(c_SupportedExtensions))
						m_SkippedFeatures.insert(fmt::format("extension {}", extension));
				}

				const std::string sourcePath = FileSystem::ToUTF8(m_Context.SourcePath);
				result = cgltf_load_buffers(&options, m_Data.get(), sourcePath.c_str());
				if (result != cgltf_result_success)
					return Fail(outError, fmt::format("loading buffers failed ({}); referenced files must be inside the asset directory", ResultToString(result)));

				// cgltf_validate trusts declared sizes in its own (unchecked) arithmetic and walks every node's parent
				// chain: check the layout and bound the hierarchy first. Compressed views are decoded before anything reads them.
				std::string structureError;
				if (!DecodeCompressedViews(structureError) || !ValidateLayout(structureError))
					return Fail(outError, structureError);
				CollectMeshSkins();
				if (!ValidateGeometryBudget(structureError) || !ValidateHierarchy(structureError))
					return Fail(outError, structureError);

				result = cgltf_validate(m_Data.get());
				if (result != cgltf_result_success)
					return Fail(outError, fmt::format("validation failed ({})", ResultToString(result)));

				m_Options = options;
				return true;
			}

			// EXT_meshopt_compression: a view's data is stored compressed in another buffer (the view's own buffer is then only
			// a fallback, often without data). Each is checked against what the extension allows (meshoptimizer's decoders
			// assert on anything else) and decoded with meshoptimizer's decoders and filters into data of the view's own
			// (cgltf_buffer_view::data, which cgltf frees with the document and its accessors read instead of the buffer's).
			bool DecodeCompressedViews(std::string& outError)
			{
				uint64_t decodedBytes = 0;
				for (cgltf_size index = 0; index < m_Data->buffer_views_count; index++)
				{
					cgltf_buffer_view& view = m_Data->buffer_views[index];
					if (!view.has_meshopt_compression)
						continue;
					const cgltf_meshopt_compression& compression = view.meshopt_compression;
					uint64_t end = 0;
					if (!compression.buffer || !compression.buffer->data || !CheckedMultiplyAdd(compression.offset, 1, compression.size, end)
						|| end > compression.buffer->size)
					{
						outError = fmt::format("buffer view {} is compressed into data outside its buffer", index);
						return false;
					}
					uint64_t size = 0;
					if (!CheckedMultiplyAdd(compression.count, compression.stride, 0, size) || size != view.size || size == 0)
					{
						outError = fmt::format("buffer view {}: its compressed elements ({} of {} bytes) do not fill its {} bytes", index, compression.count,
							compression.stride, view.size);
						return false;
					}
					const bool indices = compression.mode == cgltf_meshopt_compression_mode_triangles || compression.mode == cgltf_meshopt_compression_mode_indices;
					bool valid = compression.mode == cgltf_meshopt_compression_mode_attributes || indices;
					if (compression.mode == cgltf_meshopt_compression_mode_attributes)
						valid = compression.stride % 4 == 0 && compression.stride <= 256;
					if (indices)
						valid = (compression.stride == 2 || compression.stride == 4) && compression.filter == cgltf_meshopt_compression_filter_none;
					if (compression.mode == cgltf_meshopt_compression_mode_triangles)
						valid = valid && compression.count % 3 == 0;
					if (compression.filter == cgltf_meshopt_compression_filter_octahedral)
						valid = valid && (compression.stride == 4 || compression.stride == 8);
					else if (compression.filter == cgltf_meshopt_compression_filter_quaternion)
						valid = valid && compression.stride == 8;
					else if (compression.filter == cgltf_meshopt_compression_filter_exponential)
						valid = valid && compression.stride % 4 == 0;
					else if (compression.filter != cgltf_meshopt_compression_filter_none)
						valid = false;
					if (!valid)
					{
						outError = fmt::format("buffer view {} is compressed with a mode, filter or element size EXT_meshopt_compression does not allow", index);
						return false;
					}
					decodedBytes += size;
					if (decodedBytes > c_MaxDecodedBytes)
					{
						outError = fmt::format("the compressed buffer views decode to more than {} MiB", c_MaxDecodedBytes >> 20);
						return false;
					}

					void* decoded = std::malloc(static_cast<size_t>(size));
					if (!decoded)
					{
						outError = fmt::format("out of memory decoding buffer view {}", index);
						return false;
					}
					view.data = decoded; // Freed by cgltf_free with the document (its default free function)
					const auto* source = static_cast<const unsigned char*>(compression.buffer->data) + compression.offset;
					const size_t count = static_cast<size_t>(compression.count);
					const size_t stride = static_cast<size_t>(compression.stride);
					const size_t sourceSize = static_cast<size_t>(compression.size);
					int status = -1;
					switch (compression.mode)
					{
						case cgltf_meshopt_compression_mode_attributes: status = meshopt_decodeVertexBuffer(decoded, count, stride, source, sourceSize); break;
						case cgltf_meshopt_compression_mode_triangles:  status = meshopt_decodeIndexBuffer(decoded, count, stride, source, sourceSize); break;
						case cgltf_meshopt_compression_mode_indices:    status = meshopt_decodeIndexSequence(decoded, count, stride, source, sourceSize); break;
						default: break;
					}
					if (status != 0)
					{
						outError = fmt::format("buffer view {} could not be decoded (EXT_meshopt_compression data is corrupt or of an unknown version)", index);
						return false;
					}
					switch (compression.filter)
					{
						case cgltf_meshopt_compression_filter_octahedral:  meshopt_decodeFilterOct(decoded, count, stride); break;
						case cgltf_meshopt_compression_filter_quaternion:  meshopt_decodeFilterQuat(decoded, count, stride); break;
						case cgltf_meshopt_compression_filter_exponential: meshopt_decodeFilterExp(decoded, count, stride); break;
						default: break;
					}
				}
				return true;
			}

			// Overflow-safe bounds checks of every buffer view and accessor against the loaded data.
			bool ValidateLayout(std::string& outError) const
			{
				// A buffer without data is only a compressed view's fallback (EXT_meshopt_compression) when every view on it was
				// decoded into data of its own.
				std::vector<bool> readFromBuffer(m_Data->buffers_count, false);
				for (cgltf_size index = 0; index < m_Data->buffer_views_count; index++)
				{
					const cgltf_buffer_view& view = m_Data->buffer_views[index];
					if (view.buffer && !view.data)
						readFromBuffer[cgltf_buffer_index(m_Data.get(), view.buffer)] = true;
				}
				for (cgltf_size index = 0; index < m_Data->buffers_count; index++)
				{
					const cgltf_buffer& buffer = m_Data->buffers[index];
					if (buffer.size > 0 && !buffer.data && readFromBuffer[index])
					{
						outError = fmt::format("buffer {} has no data", index);
						return false;
					}
				}

				for (cgltf_size index = 0; index < m_Data->buffer_views_count; index++)
				{
					const cgltf_buffer_view& view = m_Data->buffer_views[index];
					uint64_t end = 0;
					if (!view.buffer || !CheckedMultiplyAdd(view.offset, 1, view.size, end) || end > view.buffer->size)
					{
						outError = fmt::format("buffer view {} lies outside its buffer", index);
						return false;
					}
					if (view.stride != 0 && (view.stride < 4 || view.stride > 252 || view.stride % 4 != 0))
					{
						outError = fmt::format("buffer view {} has an invalid byte stride {}", index, view.stride);
						return false;
					}
				}

				auto fits = [](const cgltf_buffer_view* view, uint64_t offset, uint64_t stride, uint64_t count, uint64_t elementSize)
				{
					if (!view)
						return true;
					if (count == 0)
						return offset <= view->size;
					uint64_t extent = 0;
					return CheckedMultiplyAdd(stride, count - 1, elementSize, extent) && extent <= view->size && offset <= view->size - extent;
				};

				for (cgltf_size index = 0; index < m_Data->accessors_count; index++)
				{
					const cgltf_accessor& accessor = m_Data->accessors[index];
					const uint64_t elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
					if (elementSize == 0 || accessor.count > c_MaxAccessorElements)
					{
						outError = fmt::format("accessor {} has an invalid type or too many elements ({})", index, accessor.count);
						return false;
					}

					const uint64_t stride = accessor.buffer_view && accessor.buffer_view->stride != 0 ? accessor.buffer_view->stride : elementSize;
					if (!fits(accessor.buffer_view, accessor.offset, stride, accessor.count, elementSize))
					{
						outError = fmt::format("accessor {} reads past the end of its buffer view", index);
						return false;
					}

					if (accessor.is_sparse)
					{
						// cgltf steps through the sparse values with the stride of the base view, while they are packed.
						if (stride != elementSize)
						{
							outError = fmt::format("sparse accessor {} on an interleaved buffer view is not supported", index);
							return false;
						}
						const cgltf_accessor_sparse& sparse = accessor.sparse;
						const uint64_t indexSize = cgltf_component_size(sparse.indices_component_type);
						if (sparse.count > accessor.count || indexSize == 0 || !sparse.indices_buffer_view || !sparse.values_buffer_view
							|| !fits(sparse.indices_buffer_view, sparse.indices_byte_offset, indexSize, sparse.count, indexSize)
							|| !fits(sparse.values_buffer_view, sparse.values_byte_offset, elementSize, sparse.count, elementSize))
						{
							outError = fmt::format("sparse accessor {} is out of range", index);
							return false;
						}
					}
				}
				return true;
			}

			// Limits the geometry the import expands: each primitive unpacks its accessors again (shared ones included)
			// and may unweld its triangles, so the vertices and triangle corners of all primitives are counted, once per skin a
			// mesh is drawn with (each is a mesh of its own).
			bool ValidateGeometryBudget(std::string& outError) const
			{
				uint64_t bufferBytes = 0;
				for (cgltf_size index = 0; index < m_Data->buffers_count; index++)
					bufferBytes += m_Data->buffers[index].size; // Loaded in memory, so the sum cannot overflow
				const uint64_t budget = std::min(c_MaxGeometryBudget, std::max(c_MinGeometryBudget, bufferBytes * c_GeometryBudgetPerBufferByte));

				uint64_t elements = 0;
				for (cgltf_size meshIndex = 0; meshIndex < m_Data->meshes_count; meshIndex++)
				{
					const cgltf_mesh& mesh = m_Data->meshes[meshIndex];
					// At most one per skin plus one without: bounded by the file's skins, so the product cannot overflow.
					const uint64_t uses = m_MeshSkins[meshIndex].size();
					if (uses > budget)
					{
						outError = fmt::format("a mesh is drawn with more skins than the file's data supports (limit {})", budget);
						return false;
					}
					for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; primitiveIndex++)
					{
						const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
						uint64_t vertexCount = 0;
						for (cgltf_size attribute = 0; attribute < primitive.attributes_count; attribute++)
						{
							if (primitive.attributes[attribute].type == cgltf_attribute_type_position && primitive.attributes[attribute].data)
								vertexCount = primitive.attributes[attribute].data->count;
						}
						const uint64_t indexCount = primitive.indices ? primitive.indices->count : vertexCount;
						// Strips and fans become lists: up to three corners per index.
						const uint64_t corners = primitive.type == cgltf_primitive_type_triangles ? indexCount : indexCount * 3;
						elements += (vertexCount + corners) * uses; // Each term is below 2^32 times the skins: no overflow before the check
						if (elements > budget)
						{
							outError = fmt::format("the meshes expand to more geometry than the file's data supports (limit {} vertices and corners)", budget);
							return false;
						}
					}
				}

				// Animation channels unpack their sampler's keys again (samplers may be shared), and skins their joints.
				for (cgltf_size animationIndex = 0; animationIndex < m_Data->animations_count; animationIndex++)
				{
					const cgltf_animation& animation = m_Data->animations[animationIndex];
					for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; channelIndex++)
					{
						const cgltf_animation_sampler* sampler = animation.channels[channelIndex].sampler;
						if (!sampler || !sampler->input || !sampler->output)
							continue;
						elements += sampler->input->count + sampler->output->count;
						if (elements > budget)
						{
							outError = fmt::format("the animations expand to more keys than the file's data supports (limit {} vertices, corners and keys)", budget);
							return false;
						}
					}
				}
				for (cgltf_size skinIndex = 0; skinIndex < m_Data->skins_count; skinIndex++)
				{
					elements += m_Data->skins[skinIndex].joints_count;
					if (elements > budget)
					{
						outError = fmt::format("the skins list more joints than the file's data supports (limit {})", budget);
						return false;
					}
				}
				return true;
			}

			// Rejects more nodes than the model's scene can hold (an entity per node below the model's root) and parent chains
			// deeper than the hierarchy limit, which includes cycles (depths are memoized: linear).
			bool ValidateHierarchy(std::string& outError) const
			{
				if (m_Data->nodes_count >= Scene::c_MaxEntities)
				{
					outError = fmt::format("the file has {} nodes; a model holds at most {}", m_Data->nodes_count, Scene::c_MaxEntities - 1);
					return false;
				}

				std::vector<uint32_t> depths(m_Data->nodes_count, 0); // 0 = not known yet
				std::vector<cgltf_size> chain;
				for (cgltf_size index = 0; index < m_Data->nodes_count; index++)
				{
					chain.clear();
					uint32_t depth = 0;
					for (const cgltf_node* node = &m_Data->nodes[index]; node; node = node->parent)
					{
						const cgltf_size nodeIndex = cgltf_node_index(m_Data.get(), node);
						if (depths[nodeIndex] != 0)
						{
							depth = depths[nodeIndex];
							break;
						}
						chain.push_back(nodeIndex);
						if (chain.size() > c_MaxHierarchyDepth)
						{
							outError = fmt::format("the node hierarchy is cyclic or deeper than {} levels", c_MaxHierarchyDepth);
							return false;
						}
					}

					for (auto it = chain.rbegin(); it != chain.rend(); ++it)
						depths[*it] = ++depth;
					if (depth > c_MaxHierarchyDepth)
					{
						outError = fmt::format("the node hierarchy is cyclic or deeper than {} levels", c_MaxHierarchyDepth);
						return false;
					}
				}
				return true;
			}

			bool Fail(std::string* outError, const std::string& message) const
			{
				if (outError)
					*outError = fmt::format("{}: {}", FileSystem::ToUTF8(m_Context.SourcePath.filename()), message);
				return false;
			}

			void Warn(const std::string& message)
			{
				m_Result.Warnings.push_back(message);
			}

			std::string GetName(const char* name, std::string_view kind, cgltf_size index) const
			{
				return name && name[0] ? std::string(name) : fmt::format("{}{}", kind, index);
			}

			////////////////////////////////////////////////////////////////////////////////
			// Textures and materials
			////////////////////////////////////////////////////////////////////////////////

			bool ReadImage(const cgltf_image& image, std::vector<uint8_t>& outData, std::string& outError)
			{
				if (image.buffer_view)
				{
					const cgltf_buffer_view& view = *image.buffer_view;
					const uint8_t* begin = cgltf_buffer_view_data(&view); // Range validated in ValidateLayout
					if (!begin)
					{
						outError = "image's buffer view has no data";
						return false;
					}
					outData.assign(begin, begin + view.size);
				}
				else if (!image.uri)
				{
					outError = "image has neither a URI nor a buffer view";
					return false;
				}
				else if (std::string_view uri = image.uri; uri.rfind("data:", 0) == 0)
				{
					const size_t comma = uri.find(',');
					if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
					{
						outError = "only base64 data URIs are supported";
						return false;
					}
					const std::string_view base64 = uri.substr(comma + 1);
					size_t padding = 0;
					if (!base64.empty() && base64.back() == '=')
						padding = base64.size() >= 2 && base64[base64.size() - 2] == '=' ? 2 : 1;
					const size_t size = base64.size() / 4 * 3 - std::min(padding, base64.size() / 4 * 3);
					void* decoded = nullptr;
					if (size == 0 || cgltf_load_buffer_base64(&m_Options, size, base64.data(), &decoded) != cgltf_result_success)
					{
						outError = "invalid base64 image data";
						return false;
					}
					const uint8_t* begin = static_cast<const uint8_t*>(decoded);
					outData.assign(begin, begin + size);
					std::free(decoded);
				}
				else
				{
					if (uri.find("://") != std::string_view::npos)
					{
						outError = fmt::format("image URI '{}' is not a relative file path", uri);
						return false;
					}

					std::string decodedUri(uri);
					decodedUri.resize(cgltf_decode_uri(decodedUri.data()));
					const std::filesystem::path path = m_Context.SourcePath.parent_path() / FileSystem::FromUTF8(decodedUri);
					std::optional<std::vector<uint8_t>> bytes = ReadImportDependency(m_Context, m_Result, path);
					if (!bytes)
					{
						outError = fmt::format("image '{}' is missing, unreadable or outside the asset directory", decodedUri);
						return false;
					}
					outData = std::move(*bytes);
				}

				if (!IsSupportedImageData(outData))
				{
					outError = "image is not PNG or JPEG";
					return false;
				}
				return true;
			}

			// Decodes the texture of a material slot (once per image, usage and sampler) and returns its handle.
			AssetHandle ImportTexture(const cgltf_texture_view& view, TextureUsage usage, std::string_view slot, const std::string& materialName)
			{
				if (!view.texture)
					return UUID::Null();
				const cgltf_texture& texture = *view.texture;
				// KHR_texture_transform may select another coordinate set than the texture view itself.
				const cgltf_int texCoordSet = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
				if (texCoordSet != 0)
				{
					// Only TEXCOORD_0 is imported: sampling with the wrong coordinates would look broken.
					Warn(fmt::format("Material '{}': {} texture uses TEXCOORD_{}, which is not imported; texture skipped", materialName, slot, texCoordSet));
					return UUID::Null();
				}
				if (!texture.image)
				{
					Warn(fmt::format("Material '{}': {} texture has no image (KHR_texture_basisu and EXT_texture_webp images are not supported)", materialName, slot));
					return UUID::Null();
				}

				const cgltf_size imageIndex = cgltf_image_index(m_Data.get(), texture.image);
				TextureImportSettings settings;
				settings.Usage = usage;
				settings.MaxSize = m_Settings.MaxTextureSize;
				settings.Compression = m_Settings.Compression;
				settings.Quality = m_Settings.Quality;
				if (texture.sampler)
				{
					settings.Wrap = ToTextureWrap(texture.sampler->wrap_s);
					if (texture.sampler->wrap_t != texture.sampler->wrap_s)
						m_SkippedFeatures.insert("different horizontal and vertical texture wrap modes");
					settings.Filter = texture.sampler->mag_filter == cgltf_filter_type_nearest ? TextureFilter::Nearest : TextureFilter::Linear;
				}

				const std::string key = fmt::format("Texture/{}/{}/{}{}", imageIndex, TextureUsageToString(usage), static_cast<int>(settings.Wrap), static_cast<int>(settings.Filter));
				auto existing = m_TextureHandles.find(key);
				if (existing != m_TextureHandles.end())
					return existing->second;

				std::vector<uint8_t> encoded;
				std::string error;
				Ref<Texture> decoded;
				const std::string imageName = GetName(texture.image->name, "Image", imageIndex);
				if (ReadImage(*texture.image, encoded, error))
					decoded = TextureImporter::Decode(encoded, settings, imageName, &error, m_Context.Cancel);
				if (!decoded && m_Context.IsCancelled())
					return UUID::Null(); // Run fails the import before anything uses the slot
				if (!decoded)
				{
					Warn(fmt::format("Material '{}': {} texture '{}' skipped: {}", materialName, slot, imageName, error));
					m_TextureHandles[key] = UUID::Null();
					return UUID::Null();
				}

				ImportedSubAsset subAsset;
				subAsset.Key = key;
				subAsset.Name = fmt::format("{} ({})", imageName, TextureUsageToString(usage));
				subAsset.Type = AssetType::Texture;
				subAsset.Data = decoded->Serialize();
				m_Result.SubAssets.push_back(std::move(subAsset));

				const AssetHandle handle = DeriveSubAssetHandle(m_Context.Handle, key);
				m_TextureHandles[key] = handle;
				return handle;
			}

			void ImportMaterial(cgltf_size index)
			{
				const cgltf_material& source = m_Data->materials[index];
				const std::string name = GetName(source.name, "Material", index);
				MaterialProperties properties;

				if (source.has_pbr_specular_glossiness && !source.has_pbr_metallic_roughness)
					Warn(fmt::format("Material '{}' uses the specular-glossiness workflow, which is not supported; using metallic-roughness defaults", name));

				const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
				properties.BaseColor = glm::make_vec4(pbr.base_color_factor);
				properties.Metallic = std::clamp(pbr.metallic_factor, 0.0f, 1.0f);
				properties.Roughness = std::clamp(pbr.roughness_factor, 0.0f, 1.0f);
				properties.EmissiveColor = glm::make_vec3(source.emissive_factor);
				properties.EmissiveIntensity = source.has_emissive_strength ? source.emissive_strength.emissive_strength : 1.0f;
				properties.DoubleSided = source.double_sided;
				properties.Unlit = source.unlit;
				switch (source.alpha_mode)
				{
					case cgltf_alpha_mode_mask:  properties.AlphaMode = MaterialAlphaMode::Mask; break;
					case cgltf_alpha_mode_blend: properties.AlphaMode = MaterialAlphaMode::Blend; break;
					default:                     properties.AlphaMode = MaterialAlphaMode::Opaque; break;
				}
				properties.AlphaCutoff = std::clamp(source.alpha_cutoff, 0.0f, 1.0f);

				properties.BaseColorMap = ImportTexture(pbr.base_color_texture, TextureUsage::Color, "base color", name);
				properties.MetallicRoughnessMap = ImportTexture(pbr.metallic_roughness_texture, TextureUsage::Data, "metallic-roughness", name);
				properties.NormalMap = ImportTexture(source.normal_texture, TextureUsage::NormalMap, "normal", name);
				if (source.normal_texture.texture)
					properties.NormalScale = source.normal_texture.scale;
				properties.OcclusionMap = ImportTexture(source.occlusion_texture, TextureUsage::Data, "occlusion", name);
				if (source.occlusion_texture.texture)
					properties.OcclusionStrength = std::clamp(source.occlusion_texture.scale, 0.0f, 1.0f);
				properties.EmissiveMap = ImportTexture(source.emissive_texture, TextureUsage::Color, "emissive", name);

				// Strata materials have one UV transform; it comes from the base color texture (the common case).
				const cgltf_texture_view& transformSource = pbr.base_color_texture.texture ? pbr.base_color_texture : source.normal_texture;
				if (transformSource.has_transform)
				{
					properties.UVTiling = glm::make_vec2(transformSource.transform.scale);
					properties.UVOffset = glm::make_vec2(transformSource.transform.offset);
					if (transformSource.transform.rotation != 0.0f)
						m_SkippedFeatures.insert("texture coordinate rotation (KHR_texture_transform)");
				}

				const std::string key = fmt::format("Material/{}", index);
				ImportedSubAsset subAsset;
				subAsset.Key = key;
				subAsset.Name = name;
				subAsset.Type = AssetType::Material;
				const std::string document = JsonUtils::Dump(Material::Create(properties)->Serialize(), 1, '\t');
				subAsset.Data.assign(document.begin(), document.end());
				m_Result.SubAssets.push_back(std::move(subAsset));
				m_MaterialHandles[&source] = DeriveSubAssetHandle(m_Context.Handle, key);
			}

			AssetHandle GetMaterialHandle(const cgltf_material* material) const
			{
				if (!material)
					return BuiltinAssets::DefaultMaterial;
				auto it = m_MaterialHandles.find(material);
				return it != m_MaterialHandles.end() ? it->second : BuiltinAssets::DefaultMaterial;
			}

			////////////////////////////////////////////////////////////////////////////////
			// Skeleton and skins
			////////////////////////////////////////////////////////////////////////////////

			// The nodes the model's hierarchy starts from: the scene's (or, without one, every node without a parent).
			std::vector<const cgltf_node*> GetRootNodes() const
			{
				std::vector<const cgltf_node*> roots;
				const cgltf_scene* gltfScene = m_Data->scene ? m_Data->scene : (m_Data->scenes_count > 0 ? &m_Data->scenes[0] : nullptr);
				if (gltfScene)
				{
					for (cgltf_size index = 0; index < gltfScene->nodes_count; index++)
						roots.push_back(gltfScene->nodes[index]);
				}
				else
				{
					for (cgltf_size index = 0; index < m_Data->nodes_count; index++)
					{
						if (!m_Data->nodes[index].parent)
							roots.push_back(&m_Data->nodes[index]);
					}
				}
				return roots;
			}

			static bool HasSkinAttributes(const cgltf_mesh& mesh)
			{
				for (cgltf_size primitive = 0; primitive < mesh.primitives_count; primitive++)
				{
					for (cgltf_size attribute = 0; attribute < mesh.primitives[primitive].attributes_count; attribute++)
					{
						if (mesh.primitives[primitive].attributes[attribute].type == cgltf_attribute_type_joints)
							return true;
					}
				}
				return false;
			}

			// Per mesh, the skins nodes draw it with, in node order (null: without a skin); one null entry for a mesh no node
			// draws. Meshes without joint attributes are always drawn without one. One pass over the nodes.
			void CollectMeshSkins()
			{
				m_MeshSkins.assign(m_Data->meshes_count, {});
				std::vector<int8_t> skinnable(m_Data->meshes_count, -1); // -1: not known yet
				for (cgltf_size index = 0; index < m_Data->nodes_count; index++)
				{
					const cgltf_node& node = m_Data->nodes[index];
					if (!node.mesh)
						continue;
					const cgltf_size meshIndex = cgltf_mesh_index(m_Data.get(), node.mesh);
					if (skinnable[meshIndex] < 0)
						skinnable[meshIndex] = HasSkinAttributes(*node.mesh) ? 1 : 0;
#if ST_WITH_ANIMATION
					const cgltf_skin* skin = skinnable[meshIndex] ? node.skin : nullptr;
#else
					// Without the animation module skinned meshes are imported rigid, at their bind pose.
					if (skinnable[meshIndex] && node.skin)
						m_SkippedFeatures.insert("skins (the engine has no animation module)");
					const cgltf_skin* skin = nullptr;
#endif
					std::vector<const cgltf_skin*>& skins = m_MeshSkins[meshIndex];
					if (std::find(skins.begin(), skins.end(), skin) == skins.end())
						skins.push_back(skin);
				}
				for (std::vector<const cgltf_skin*>& skins : m_MeshSkins)
				{
					if (skins.empty())
						skins.push_back(nullptr);
				}
			}

			JointPose GetNodePose(const cgltf_node& node, cgltf_size nodeIndex)
			{
				JointPose pose;
				if (node.has_matrix)
				{
					if (!Math::DecomposeTransform(glm::make_mat4(node.matrix), pose.Translation, pose.Rotation, pose.Scale))
					{
						Warn(fmt::format("Node {} has a transform matrix that cannot be decomposed; its joint uses identity", nodeIndex));
						return JointPose();
					}
					return pose;
				}
				if (node.has_translation)
					pose.Translation = glm::make_vec3(node.translation);
				if (node.has_rotation)
					pose.Rotation = AnimationMath::Normalize(glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]));
				if (node.has_scale)
					pose.Scale = glm::make_vec3(node.scale);
				return pose;
			}

#if ST_WITH_ANIMATION
			// The model's skeleton ("Skeleton"): the joints of every skin and the nodes the animations move, with their ancestors
			// (whose transforms carry them), parents first in the order of the node hierarchy. Then each skin's joints in it and
			// its inverse bind matrices.
			void ImportSkeleton()
			{
				std::vector<uint8_t> inSkeleton(m_Data->nodes_count, 0);
				bool any = false;
				for (cgltf_size skinIndex = 0; skinIndex < m_Data->skins_count; skinIndex++)
				{
					const cgltf_skin& skin = m_Data->skins[skinIndex];
					for (cgltf_size joint = 0; joint < skin.joints_count; joint++)
					{
						inSkeleton[cgltf_node_index(m_Data.get(), skin.joints[joint])] = 1;
						any = true;
					}
				}
				for (cgltf_size animationIndex = 0; animationIndex < m_Data->animations_count; animationIndex++)
				{
					const cgltf_animation& animation = m_Data->animations[animationIndex];
					for (cgltf_size channel = 0; channel < animation.channels_count; channel++)
					{
						const cgltf_animation_channel& source = animation.channels[channel];
						if (source.target_node && source.target_path != cgltf_animation_path_type_weights && source.target_path != cgltf_animation_path_type_invalid)
						{
							inSkeleton[cgltf_node_index(m_Data.get(), source.target_node)] = 1;
							any = true;
						}
					}
				}
				if (!any)
					return;
				// Ancestors carry their descendants' transforms: they are joints too (ValidateHierarchy bounded the chains).
				for (cgltf_size index = 0; index < m_Data->nodes_count; index++)
				{
					if (!inSkeleton[index])
						continue;
					for (const cgltf_node* parent = m_Data->nodes[index].parent; parent; parent = parent->parent)
					{
						uint8_t& marked = inSkeleton[cgltf_node_index(m_Data.get(), parent)];
						if (marked)
							break;
						marked = 1;
					}
				}

				// Parents first: a walk of the hierarchy from the model's roots, then from the other parentless nodes (skins may
				// name nodes outside the scene).
				m_NodeJoints.assign(m_Data->nodes_count, c_NoJoint);
				std::vector<std::string> names;
				std::vector<JointIndex> parents;
				std::vector<JointPose> restPose;
				std::unordered_set<std::string> usedNames;
				std::vector<uint8_t> visited(m_Data->nodes_count, 0);
				std::vector<const cgltf_node*> starts = GetRootNodes();
				for (cgltf_size index = 0; index < m_Data->nodes_count; index++)
				{
					if (!m_Data->nodes[index].parent)
						starts.push_back(&m_Data->nodes[index]);
				}
				std::vector<const cgltf_node*> stack;
				for (const cgltf_node* start : starts)
				{
					stack.push_back(start);
					while (!stack.empty())
					{
						const cgltf_node* node = stack.back();
						stack.pop_back();
						const cgltf_size nodeIndex = cgltf_node_index(m_Data.get(), node);
						if (visited[nodeIndex])
							continue;
						visited[nodeIndex] = 1;
						if (!inSkeleton[nodeIndex])
							continue; // Below a node outside the skeleton nothing is in it (ancestors of joints are joints)
						if (names.size() >= Skeleton::c_MaxJoints)
						{
							Warn(fmt::format("The model's skeleton has more than {} joints; skins and animations are not imported", Skeleton::c_MaxJoints));
							m_NodeJoints.clear();
							return;
						}
						// Joint names are unique: clips bind to joints by name.
						std::string name = GetName(node->name, "Node", nodeIndex);
						if (!usedNames.insert(name).second)
						{
							name = fmt::format("{}_{}", name, nodeIndex);
							while (!usedNames.insert(name).second)
								name += "_";
						}
						m_NodeJoints[nodeIndex] = static_cast<JointIndex>(names.size());
						names.push_back(std::move(name));
						parents.push_back(node->parent ? m_NodeJoints[cgltf_node_index(m_Data.get(), node->parent)] : c_NoJoint);
						restPose.push_back(GetNodePose(*node, nodeIndex));
						for (cgltf_size child = node->children_count; child > 0; child--)
							stack.push_back(node->children[child - 1]);
					}
				}

				// Root motion moves the entity by the topmost joint of a skin (a character's hips or root bone).
				JointIndex rootMotionJoint = c_NoJoint;
				for (cgltf_size skinIndex = 0; skinIndex < m_Data->skins_count; skinIndex++)
				{
					const cgltf_skin& skin = m_Data->skins[skinIndex];
					for (cgltf_size joint = 0; joint < skin.joints_count; joint++)
						rootMotionJoint = std::min(rootMotionJoint, m_NodeJoints[cgltf_node_index(m_Data.get(), skin.joints[joint])]);
				}

				std::string error;
				m_JointNames = names; // Channels name their joints
				// Where each joint rests relative to the model's root: entities attached to joints start there.
				m_JointRestModel.assign(names.size(), glm::mat4(1.0f));
				for (size_t joint = 0; joint < names.size(); joint++)
				{
					const glm::mat4 local = AnimationMath::ToMatrix(restPose[joint]);
					m_JointRestModel[joint] = parents[joint] == c_NoJoint ? local : AnimationMath::MultiplyAffine(m_JointRestModel[parents[joint]], local);
				}
				Ref<Skeleton> skeleton = Skeleton::Create(std::move(names), std::move(parents), std::move(restPose), rootMotionJoint, &error);
				if (!skeleton)
				{
					Warn(fmt::format("The model's skeleton was not imported ({}); skins and animations are not imported", error));
					m_NodeJoints.clear();
					m_JointRestModel.clear();
					return;
				}
				ImportedSubAsset subAsset;
				subAsset.Key = "Skeleton";
				subAsset.Name = "Skeleton";
				subAsset.Type = AssetType::Skeleton;
				subAsset.Data = skeleton->Serialize();
				m_Result.SubAssets.push_back(std::move(subAsset));
				m_SkeletonHandle = DeriveSubAssetHandle(m_Context.Handle, "Skeleton");

				for (cgltf_size skinIndex = 0; skinIndex < m_Data->skins_count; skinIndex++)
					m_Skins[&m_Data->skins[skinIndex]] = ReadSkin(m_Data->skins[skinIndex], skinIndex);
			}
#endif

			// A skin's joints in the skeleton and its inverse bind matrices; empty joints when it cannot be used.
			MeshSkinBinding ReadSkin(const cgltf_skin& skin, cgltf_size skinIndex)
			{
				MeshSkinBinding binding;
				const std::string name = GetName(skin.name, "Skin", skinIndex);
				if (skin.joints_count == 0 || skin.joints_count > MeshSkinBinding::c_MaxJoints)
				{
					Warn(fmt::format("Skin '{}' has {} joints (1 to {} are supported); its meshes are imported without skinning", name, skin.joints_count, MeshSkinBinding::c_MaxJoints));
					return binding;
				}
				std::vector<glm::mat4> matrices(skin.joints_count, glm::mat4(1.0f));
				if (skin.inverse_bind_matrices && !ReadFloats(skin.inverse_bind_matrices, cgltf_type_mat4, skin.joints_count, matrices))
				{
					Warn(fmt::format("Skin '{}' has unreadable inverse bind matrices; its meshes are imported without skinning", name));
					return binding;
				}
				for (glm::mat4& matrix : matrices)
				{
					// Affine transforms only; a fourth row a writer rounded is set to exactly (0, 0, 0, 1).
					bool finite = true;
					for (int column = 0; column < 4; column++)
					{
						for (int row = 0; row < 4; row++)
							finite = finite && std::isfinite(matrix[column][row]);
					}
					const bool affine = std::abs(matrix[0][3]) < 1e-4f && std::abs(matrix[1][3]) < 1e-4f && std::abs(matrix[2][3]) < 1e-4f && std::abs(matrix[3][3] - 1.0f) < 1e-4f;
					if (!finite || !affine)
					{
						Warn(fmt::format("Skin '{}' has an inverse bind matrix that is not a finite affine transform; its meshes are imported without skinning", name));
						return binding;
					}
					matrix[0][3] = 0.0f;
					matrix[1][3] = 0.0f;
					matrix[2][3] = 0.0f;
					matrix[3][3] = 1.0f;
				}
				binding.Joints.reserve(skin.joints_count);
				for (cgltf_size joint = 0; joint < skin.joints_count; joint++)
					binding.Joints.push_back(m_NodeJoints[cgltf_node_index(m_Data.get(), skin.joints[joint])]);
				binding.InverseBindMatrices = std::move(matrices);
				return binding;
			}

			// The skin vertices of a primitive: the four strongest of up to eight influences (JOINTS_0/1, WEIGHTS_0/1),
			// normalized and quantized to sixteen bits that add up to 65535. False when the primitive has no usable skinning.
			bool ReadSkinVertices(const cgltf_primitive& primitive, size_t vertexCount, uint32_t jointCount, const std::string& primitiveName,
				std::vector<MeshSkinVertex>& outVertices)
			{
				std::array<const cgltf_accessor*, 2> jointAccessors = {};
				std::array<const cgltf_accessor*, 2> weightAccessors = {};
				for (cgltf_size attributeIndex = 0; attributeIndex < primitive.attributes_count; attributeIndex++)
				{
					const cgltf_attribute& attribute = primitive.attributes[attributeIndex];
					if (attribute.index < 0 || attribute.index > 1)
					{
						if (attribute.type == cgltf_attribute_type_joints || attribute.type == cgltf_attribute_type_weights)
							m_SkippedFeatures.insert("skin influences beyond eight per vertex");
						continue;
					}
					if (attribute.type == cgltf_attribute_type_joints)
						jointAccessors[static_cast<size_t>(attribute.index)] = attribute.data;
					else if (attribute.type == cgltf_attribute_type_weights)
						weightAccessors[static_cast<size_t>(attribute.index)] = attribute.data;
				}

				struct Influence
				{
					uint32_t Joint = 0;
					float Weight = 0.0f;
				};
				std::vector<std::array<Influence, 8>> influences(vertexCount);
				uint32_t sets = 0;
				for (size_t set = 0; set < 2; set++)
				{
					const cgltf_accessor* joints = jointAccessors[set];
					const cgltf_accessor* weights = weightAccessors[set];
					if (!joints && !weights)
						continue;
					std::vector<glm::vec4> weightValues;
					if (!joints || !weights || joints->type != cgltf_type_vec4 || joints->count != vertexCount
						|| !ReadFloats(weights, cgltf_type_vec4, vertexCount, weightValues))
					{
						Warn(fmt::format("Primitive '{}' has unreadable skin influences (JOINTS_{} / WEIGHTS_{})", primitiveName, set, set));
						return false;
					}
					for (size_t vertex = 0; vertex < vertexCount; vertex++)
					{
						cgltf_uint indices[4] = {};
						if (!cgltf_accessor_read_uint(joints, vertex, indices, 4))
						{
							Warn(fmt::format("Primitive '{}' has unreadable joint indices", primitiveName));
							return false;
						}
						for (size_t influence = 0; influence < 4; influence++)
						{
							const float weight = weightValues[vertex][static_cast<int>(influence)];
							// A weight for a joint the skin does not have, or a negative or non-finite one, counts as none.
							const bool valid = std::isfinite(weight) && weight > 0.0f && indices[influence] < jointCount;
							influences[vertex][set * 4 + influence] = Influence { valid ? indices[influence] : 0u, valid ? weight : 0.0f };
						}
					}
					sets++;
				}
				if (sets == 0)
					return false;
				if (sets > 1)
					m_SkippedFeatures.insert("skin influences beyond the four strongest of a vertex");

				outVertices.resize(vertexCount);
				bool unweighted = false;
				for (size_t vertex = 0; vertex < vertexCount; vertex++)
				{
					std::array<Influence, 8>& candidates = influences[vertex];
					std::stable_sort(candidates.begin(), candidates.end(), [](const Influence& a, const Influence& b) { return a.Weight > b.Weight; });
					const float total = candidates[0].Weight + candidates[1].Weight + candidates[2].Weight + candidates[3].Weight;
					MeshSkinVertex& skin = outVertices[vertex];
					if (!(total > 0.0f))
					{
						// No influence at all: the vertex follows the skin's first joint.
						unweighted = true;
						skin = MeshSkinVertex();
						continue;
					}
					uint32_t sum = 0;
					for (int influence = 0; influence < 4; influence++)
					{
						skin.Joints[influence] = static_cast<uint16_t>(candidates[static_cast<size_t>(influence)].Joint);
						skin.Weights[influence] = static_cast<uint16_t>(std::lround(candidates[static_cast<size_t>(influence)].Weight / total * 65535.0f));
						sum += skin.Weights[influence];
					}
					// Rounding leaves the sum a few units off: the strongest influence takes the difference.
					skin.Weights[0] = static_cast<uint16_t>(static_cast<int64_t>(skin.Weights[0]) + 65535 - static_cast<int64_t>(sum));
					for (int influence = 0; influence < 4; influence++)
					{
						if (skin.Weights[influence] == 0)
							skin.Joints[influence] = 0;
					}
				}
				if (unweighted)
					Warn(fmt::format("Primitive '{}' has vertices without skin weights; they follow the skin's first joint", primitiveName));
				return true;
			}

			////////////////////////////////////////////////////////////////////////////////
			// Meshes
			////////////////////////////////////////////////////////////////////////////////

			// Triangle list indices of a primitive (strips and fans are converted), or false when it has no triangles.
			bool ReadTriangleIndices(const cgltf_primitive& primitive, size_t vertexCount, std::vector<uint32_t>& outIndices)
			{
				std::vector<uint32_t> indices;
				if (primitive.indices)
				{
					indices.resize(primitive.indices->count);
					if (cgltf_accessor_unpack_indices(primitive.indices, indices.data(), sizeof(uint32_t), indices.size()) != indices.size())
						return false;
				}
				else
				{
					indices.resize(vertexCount);
					for (size_t index = 0; index < vertexCount; index++)
						indices[index] = static_cast<uint32_t>(index);
				}

				outIndices.clear();
				switch (primitive.type)
				{
					case cgltf_primitive_type_triangles:
						outIndices.assign(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(indices.size() / 3 * 3));
						break;
					case cgltf_primitive_type_triangle_strip:
						for (size_t index = 2; index < indices.size(); index++)
						{
							// Every other triangle of a strip is wound in reverse.
							const bool even = index % 2 == 0;
							outIndices.insert(outIndices.end(), { indices[index - 2], even ? indices[index - 1] : indices[index], even ? indices[index] : indices[index - 1] });
						}
						break;
					case cgltf_primitive_type_triangle_fan:
						for (size_t index = 2; index < indices.size(); index++)
							outIndices.insert(outIndices.end(), { indices[0], indices[index - 1], indices[index] });
						break;
					default:
						return false;
				}

				for (uint32_t index : outIndices)
				{
					if (index >= vertexCount)
						return false;
				}
				return !outIndices.empty();
			}

			// Reads a float accessor fully; false when it has no data or the wrong shape.
			template<typename T>
			static bool ReadFloats(const cgltf_accessor* accessor, cgltf_type type, size_t expectedCount, std::vector<T>& outValues)
			{
				constexpr size_t components = sizeof(T) / sizeof(float);
				if (!accessor || accessor->type != type || accessor->count != expectedCount)
					return false;
				outValues.resize(expectedCount);
				return cgltf_accessor_unpack_floats(accessor, reinterpret_cast<float*>(outValues.data()), expectedCount * components) == expectedCount * components;
			}

			// Imports a mesh as drawn with `skin` (null: without one) under `key`.
			void ImportMesh(cgltf_size meshIndex, const cgltf_skin* skin, const std::string& key)
			{
				const cgltf_mesh& source = m_Data->meshes[meshIndex];
				const std::string meshName = GetName(source.name, "Mesh", meshIndex);
				// A skin that could not be read (or a model whose skeleton could not be) leaves the mesh rigid in its bind pose.
				const MeshSkinBinding* binding = nullptr;
				if (skin)
				{
					auto found = m_Skins.find(skin);
					if (found != m_Skins.end() && !found->second.Joints.empty())
						binding = &found->second;
				}

				std::vector<glm::vec3> positions;
				std::vector<MeshVertexAttributes> attributes;
				std::vector<MeshSkinVertex> skinVertices;
				std::vector<uint32_t> indices;
				std::vector<Submesh> submeshes;

				for (cgltf_size primitiveIndex = 0; primitiveIndex < source.primitives_count; primitiveIndex++)
				{
					// A cancelled import stops here; Run then fails it.
					if (m_Context.IsCancelled())
						return;
					const cgltf_primitive& primitive = source.primitives[primitiveIndex];
					const std::string primitiveName = source.primitives_count > 1 ? fmt::format("{}/{}", meshName, primitiveIndex) : meshName;
					if (primitive.targets_count > 0)
						m_SkippedFeatures.insert("morph targets");

					const cgltf_accessor* positionAccessor = nullptr;
					const cgltf_accessor* normalAccessor = nullptr;
					const cgltf_accessor* tangentAccessor = nullptr;
					const cgltf_accessor* texCoordAccessor = nullptr;
					for (cgltf_size attributeIndex = 0; attributeIndex < primitive.attributes_count; attributeIndex++)
					{
						const cgltf_attribute& attribute = primitive.attributes[attributeIndex];
						switch (attribute.type)
						{
							case cgltf_attribute_type_position: positionAccessor = attribute.data; break;
							case cgltf_attribute_type_normal:   normalAccessor = attribute.data; break;
							case cgltf_attribute_type_tangent:  tangentAccessor = attribute.data; break;
							case cgltf_attribute_type_texcoord:
								if (attribute.index == 0)
									texCoordAccessor = attribute.data;
								break;
							case cgltf_attribute_type_color: m_SkippedFeatures.insert("vertex colors"); break;
							default: break; // Joints and weights: ReadSkinVertices, for meshes drawn with a skin
						}
					}

					// An optional Draco extension comes with uncompressed fallback data, which is what gets imported.
					if (primitive.has_draco_mesh_compression && (!positionAccessor || !positionAccessor->buffer_view))
					{
						Warn(fmt::format("Primitive '{}' only has Draco-compressed data, which is not supported; skipped", primitiveName));
						continue;
					}
					if (primitive.type != cgltf_primitive_type_triangles && primitive.type != cgltf_primitive_type_triangle_strip
						&& primitive.type != cgltf_primitive_type_triangle_fan)
					{
						Warn(fmt::format("Primitive '{}' is not made of triangles (points or lines); skipped", primitiveName));
						continue;
					}

					const size_t vertexCount = positionAccessor ? positionAccessor->count : 0;
					std::vector<glm::vec3> primitivePositions;
					if (vertexCount == 0 || !ReadFloats(positionAccessor, cgltf_type_vec3, vertexCount, primitivePositions))
					{
						Warn(fmt::format("Primitive '{}' has no readable positions; skipped", primitiveName));
						continue;
					}
					if (std::any_of(primitivePositions.begin(), primitivePositions.end(), [](const glm::vec3& p) { return !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z); }))
					{
						Warn(fmt::format("Primitive '{}' has non-finite positions; skipped", primitiveName));
						continue;
					}

					std::vector<uint32_t> primitiveIndices;
					if (!ReadTriangleIndices(primitive, vertexCount, primitiveIndices))
					{
						Warn(fmt::format("Primitive '{}' has invalid indices; skipped", primitiveName));
						continue;
					}

					// Every primitive of a skinned mesh is skinned; one without influences follows the skin's first joint.
					std::vector<MeshSkinVertex> primitiveSkin;
					if (binding && !ReadSkinVertices(primitive, vertexCount, static_cast<uint32_t>(binding->Joints.size()), primitiveName, primitiveSkin))
					{
						Warn(fmt::format("Primitive '{}' of a skinned mesh has no skin influences; it follows the skin's first joint", primitiveName));
						primitiveSkin.assign(vertexCount, MeshSkinVertex());
					}

					std::vector<MeshVertexAttributes> primitiveAttributes(vertexCount);
					std::vector<glm::vec2> texCoords;
					const bool hasTexCoords = ReadFloats(texCoordAccessor, cgltf_type_vec2, vertexCount, texCoords);
					if (hasTexCoords)
					{
						for (size_t vertex = 0; vertex < vertexCount; vertex++)
							primitiveAttributes[vertex].TexCoord = texCoords[vertex];
					}

					std::vector<glm::vec3> normals;
					bool hasNormals = ReadFloats(normalAccessor, cgltf_type_vec3, vertexCount, normals);
					for (size_t vertex = 0; hasNormals && vertex < vertexCount; vertex++)
					{
						const float length = glm::length(normals[vertex]);
						if (!std::isfinite(length) || length < 1e-6f)
							hasNormals = false;
						else
							primitiveAttributes[vertex].Normal = normals[vertex] / length;
					}
					if (normalAccessor && !hasNormals)
						Warn(fmt::format("Primitive '{}' has unusable normals; flat normals are computed", primitiveName));

					// glTF: without normals, flat normals are computed and provided tangents are ignored.
					std::vector<glm::vec4> tangents;
					bool hasTangents = hasNormals && ReadFloats(tangentAccessor, cgltf_type_vec4, vertexCount, tangents);
					for (size_t vertex = 0; hasTangents && vertex < vertexCount; vertex++)
					{
						const glm::vec3 tangent = glm::vec3(tangents[vertex]);
						const float length = glm::length(tangent);
						if (!std::isfinite(length) || length < 1e-6f)
							hasTangents = false;
						else
							primitiveAttributes[vertex].Tangent = glm::vec4(tangent / length, tangents[vertex].w < 0.0f ? -1.0f : 1.0f);
					}

					if (!hasTangents && hasTexCoords)
					{
						// Exact per-corner MikkTSpace tangents (also creates the flat normals when needed).
						if (!MeshUtils::GenerateSeamTangents(primitivePositions, primitiveAttributes, primitiveIndices, !hasNormals, binding ? &primitiveSkin : nullptr))
						{
							Warn(fmt::format("Primitive '{}': tangent generation failed", primitiveName));
							continue;
						}
					}
					else if (!hasTangents)
					{
						if (!hasNormals)
						{
							// Flat normals without texture coordinates: unwelded triangles with their face normals.
							std::vector<glm::vec3> flatPositions;
							std::vector<MeshVertexAttributes> flatAttributes;
							std::vector<MeshSkinVertex> flatSkin;
							for (size_t corner = 0; corner < primitiveIndices.size(); corner += 3)
							{
								const glm::vec3& a = primitivePositions[primitiveIndices[corner]];
								const glm::vec3& b = primitivePositions[primitiveIndices[corner + 1]];
								const glm::vec3& c = primitivePositions[primitiveIndices[corner + 2]];
								const glm::vec3 faceNormal = glm::cross(b - a, c - a);
								const float length = glm::length(faceNormal);
								MeshVertexAttributes attribute;
								attribute.Normal = length > 1e-12f ? faceNormal / length : glm::vec3(0.0f, 1.0f, 0.0f);
								for (size_t vertex = 0; vertex < 3; vertex++)
								{
									flatPositions.push_back(primitivePositions[primitiveIndices[corner + vertex]]);
									flatAttributes.push_back(attribute);
									if (binding)
										flatSkin.push_back(primitiveSkin[primitiveIndices[corner + vertex]]);
								}
							}
							primitivePositions = std::move(flatPositions);
							primitiveAttributes = std::move(flatAttributes);
							primitiveSkin = std::move(flatSkin);
							for (size_t corner = 0; corner < primitiveIndices.size(); corner++)
								primitiveIndices[corner] = static_cast<uint32_t>(corner);
						}
						// Without texture coordinates any tangent perpendicular to the normal is as good as another.
						for (MeshVertexAttributes& attribute : primitiveAttributes)
						{
							const glm::vec3 reference = std::abs(attribute.Normal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
							attribute.Tangent = glm::vec4(glm::normalize(glm::cross(reference, attribute.Normal)), 1.0f);
						}
					}

					const size_t primitiveVertexCount = primitivePositions.size();
					Submesh submesh;
					submesh.Name = primitiveName;
					submesh.BaseVertex = static_cast<uint32_t>(positions.size());
					submesh.VertexCount = static_cast<uint32_t>(primitiveVertexCount);
					submesh.Material = GetMaterialHandle(primitive.material);
					submesh.Bounds = MeshUtils::ComputeBounds(primitivePositions);

					if (m_Settings.OptimizeMeshes)
						MeshUtils::OptimizeIndices(primitiveIndices, primitiveVertexCount);
					std::vector<std::vector<uint32_t>> levels = { primitiveIndices };
					// Simplifying a large primitive into its levels of detail takes the longest of a mesh.
					if (m_Context.IsCancelled())
						return;
					if (m_Settings.GenerateLODs && m_Settings.LODCount > 0)
					{
						for (std::vector<uint32_t>& level : MeshUtils::GenerateLODs(primitivePositions, primitiveIndices, m_Settings.LODCount))
							levels.push_back(std::move(level));
					}
					for (const std::vector<uint32_t>& level : levels)
					{
						submesh.LODs.push_back(MeshLOD { static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(level.size()) });
						indices.insert(indices.end(), level.begin(), level.end());
					}

					positions.insert(positions.end(), primitivePositions.begin(), primitivePositions.end());
					attributes.insert(attributes.end(), primitiveAttributes.begin(), primitiveAttributes.end());
					skinVertices.insert(skinVertices.end(), primitiveSkin.begin(), primitiveSkin.end());
					submeshes.push_back(std::move(submesh));
				}

				if (submeshes.empty())
				{
					Warn(fmt::format("Mesh '{}' has no importable primitives", meshName));
					return;
				}

				std::string error;
				Ref<Mesh> mesh = binding
					? Mesh::CreateSkinned(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), std::move(skinVertices), *binding, &error)
					: Mesh::Create(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), &error);
				if (!mesh)
				{
					Warn(fmt::format("Mesh '{}' skipped: {}", meshName, error));
					return;
				}

				ImportedSubAsset subAsset;
				subAsset.Key = key;
				subAsset.Name = meshName;
				subAsset.Type = AssetType::Mesh;
				subAsset.Data = mesh->Serialize();
				// What the cooked mesh does not show as a document: its submeshes' default materials, and its bounds, so that
				// packs can prefetch the materials with the mesh and culling can place the mesh before it loads.
				for (const Submesh& submesh : mesh->GetSubmeshes())
				{
					if (submesh.Material.IsValid() && std::find(subAsset.References.begin(), subAsset.References.end(), submesh.Material) == subAsset.References.end())
						subAsset.References.push_back(submesh.Material);
				}
				subAsset.Bounds = mesh->GetBounds();
				m_Result.SubAssets.push_back(std::move(subAsset));
				m_MeshHandles[{ &source, skin }] = MeshImport { DeriveSubAssetHandle(m_Context.Handle, key), binding != nullptr };
			}

#if ST_WITH_ANIMATION
			////////////////////////////////////////////////////////////////////////////////
			// Animations
			////////////////////////////////////////////////////////////////////////////////

			// One clip per animation ("Animation/<name>", or "Animation/<index>" for animations without a unique name), of the
			// channels that move nodes of the skeleton; keys interpolation reproduces are reduced.
			void ImportAnimations()
			{
				if (m_NodeJoints.empty())
					return; // No skeleton: nothing an animation could move was imported
				std::map<std::string, uint32_t> nameCounts;
				for (cgltf_size index = 0; index < m_Data->animations_count; index++)
				{
					if (m_Data->animations[index].name && m_Data->animations[index].name[0])
						nameCounts[m_Data->animations[index].name]++;
				}

				for (cgltf_size animationIndex = 0; animationIndex < m_Data->animations_count; animationIndex++)
				{
					if (m_Context.IsCancelled())
						return; // Run fails the import
					const cgltf_animation& animation = m_Data->animations[animationIndex];
					const std::string name = GetName(animation.name, "Animation", animationIndex);
					const bool uniqueName = animation.name && animation.name[0] && nameCounts[animation.name] == 1;
					const std::string key = uniqueName ? fmt::format("Animation/{}", name) : fmt::format("Animation/{}", animationIndex);

					std::vector<AnimationChannelData> channels;
					std::set<std::pair<JointIndex, int>> animated;
					for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; channelIndex++)
					{
						// A long baked animation is seconds of unpacking and key reduction: checked per channel, and within the
						// reduction.
						if (m_Context.IsCancelled())
							return; // Run fails the import
						const cgltf_animation_channel& source = animation.channels[channelIndex];
						if (source.target_path == cgltf_animation_path_type_weights)
						{
							m_SkippedFeatures.insert("morph target animation");
							continue;
						}
						std::optional<AnimationChannelData> channel = ReadChannel(source, name);
						if (!channel)
							continue;
						const JointIndex joint = m_NodeJoints[cgltf_node_index(m_Data.get(), source.target_node)];
						if (!animated.insert({ joint, static_cast<int>(channel->Path) }).second)
						{
							Warn(fmt::format("Animation '{}' animates the {} of '{}' twice; the first channel is kept", name, AnimationPathToString(channel->Path), channel->Target));
							continue;
						}
						if (!AnimationClip::ReduceKeys(*channel, [this]() { return m_Context.IsCancelled(); }))
							return; // Cancelled: Run fails the import
						channels.push_back(std::move(*channel));
					}
					if (channels.empty())
					{
						Warn(fmt::format("Animation '{}' has no channel that moves a node; skipped", name));
						continue;
					}

					std::string error;
					Ref<AnimationClip> clip = AnimationClip::Create(name, std::move(channels), &error);
					if (!clip)
					{
						Warn(fmt::format("Animation '{}' skipped: {}", name, error));
						continue;
					}
					ImportedSubAsset subAsset;
					subAsset.Key = key;
					subAsset.Name = name;
					subAsset.Type = AssetType::AnimationClip;
					subAsset.Data = clip->Serialize();
					m_Result.SubAssets.push_back(std::move(subAsset));
					m_ClipHandles.push_back(DeriveSubAssetHandle(m_Context.Handle, key));
				}
			}

			// A channel of a node of the skeleton: its sampler's times and values (cubic splines: three per key).
			std::optional<AnimationChannelData> ReadChannel(const cgltf_animation_channel& source, const std::string& animationName)
			{
				const cgltf_animation_sampler* sampler = source.sampler;
				if (!source.target_node || !sampler || !sampler->input || !sampler->output
					|| m_NodeJoints[cgltf_node_index(m_Data.get(), source.target_node)] == c_NoJoint)
					return std::nullopt;
				AnimationChannelData channel;
				const JointIndex joint = m_NodeJoints[cgltf_node_index(m_Data.get(), source.target_node)];
				channel.Target = m_JointNames[joint];
				cgltf_type valueType = cgltf_type_vec3;
				switch (source.target_path)
				{
					case cgltf_animation_path_type_translation: channel.Path = AnimationPath::Translation; break;
					case cgltf_animation_path_type_rotation:    channel.Path = AnimationPath::Rotation; valueType = cgltf_type_vec4; break;
					case cgltf_animation_path_type_scale:       channel.Path = AnimationPath::Scale; break;
					default: return std::nullopt;
				}
				switch (sampler->interpolation)
				{
					case cgltf_interpolation_type_step:         channel.Interpolation = AnimationInterpolation::Step; break;
					case cgltf_interpolation_type_cubic_spline: channel.Interpolation = AnimationInterpolation::CubicSpline; break;
					default:                                    channel.Interpolation = AnimationInterpolation::Linear; break;
				}
				const size_t keys = sampler->input->count;
				const size_t valuesPerKey = channel.Interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
				const size_t components = valueType == cgltf_type_vec4 ? 4 : 3;
				if (keys == 0 || sampler->input->type != cgltf_type_scalar || sampler->output->type != valueType || sampler->output->count != keys * valuesPerKey)
				{
					Warn(fmt::format("Animation '{}': the {} channel of '{}' has unreadable keys; skipped", animationName, AnimationPathToString(channel.Path), channel.Target));
					return std::nullopt;
				}
				channel.Times.resize(keys);
				channel.Values.resize(keys * valuesPerKey * components);
				if (cgltf_accessor_unpack_floats(sampler->input, channel.Times.data(), keys) != keys
					|| cgltf_accessor_unpack_floats(sampler->output, channel.Values.data(), channel.Values.size()) != channel.Values.size())
				{
					Warn(fmt::format("Animation '{}': the {} channel of '{}' has unreadable keys; skipped", animationName, AnimationPathToString(channel.Path), channel.Target));
					return std::nullopt;
				}
				// glTF: key times are not negative and strictly increasing; values are finite.
				for (size_t key = 0; key < keys; key++)
				{
					if (!std::isfinite(channel.Times[key]) || channel.Times[key] < 0.0f || (key > 0 && !(channel.Times[key] > channel.Times[key - 1])))
					{
						Warn(fmt::format("Animation '{}': the {} channel of '{}' has key times that are negative or not increasing; skipped", animationName,
							AnimationPathToString(channel.Path), channel.Target));
						return std::nullopt;
					}
				}
				if (std::any_of(channel.Values.begin(), channel.Values.end(), [](float value) { return !std::isfinite(value); }))
				{
					Warn(fmt::format("Animation '{}': the {} channel of '{}' has non-finite values; skipped", animationName, AnimationPathToString(channel.Path), channel.Target));
					return std::nullopt;
				}
				return channel;
			}
#endif

			////////////////////////////////////////////////////////////////////////////////
			// Hierarchy
			////////////////////////////////////////////////////////////////////////////////

			// The mesh a node draws, as imported for the skin it draws it with; null when the mesh was not imported.
			const MeshImport* FindMeshImport(const cgltf_node& node) const
			{
				if (!node.mesh)
					return nullptr;
				auto mesh = m_MeshHandles.find({ node.mesh, HasSkinAttributes(*node.mesh) ? node.skin : nullptr });
				return mesh != m_MeshHandles.end() ? &mesh->second : nullptr;
			}

			void SetupEntity(Entity entity, const cgltf_node& node)
			{
				const MeshImport* mesh = FindMeshImport(node);
				TransformComponent& transform = entity.GetComponent<TransformComponent>();
				if (mesh && mesh->Skinned)
				{
					// glTF ignores the transform of a node that draws a skinned mesh: its joints place the vertices, relative to the
					// model's root (AddHierarchy puts the entity there).
				}
				else if (node.has_matrix)
				{
					glm::vec3 translation;
					glm::quat rotation;
					glm::vec3 scale;
					if (Math::DecomposeTransform(glm::make_mat4(node.matrix), translation, rotation, scale))
					{
						transform.Translation = translation;
						transform.Rotation = rotation;
						transform.Scale = scale;
					}
					else
					{
						Warn(fmt::format("Node '{}' has a transform matrix that cannot be decomposed; using identity", entity.GetName()));
					}
				}
				else
				{
					if (node.has_translation)
						transform.Translation = glm::make_vec3(node.translation);
					if (node.has_rotation)
						transform.Rotation = glm::normalize(glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]));
					if (node.has_scale)
						transform.Scale = glm::make_vec3(node.scale);
				}

				if (mesh)
					entity.AddComponent<MeshRendererComponent>().Mesh = mesh->Handle;
				if (node.has_mesh_gpu_instancing)
					m_SkippedFeatures.insert("GPU instancing (EXT_mesh_gpu_instancing)");

				// Distances in components are world units; the import scale applies to them like to the geometry.
				const float scale = m_Settings.Scale;
				if (node.camera && m_Settings.ImportCameras)
				{
					CameraComponent& camera = entity.AddComponent<CameraComponent>();
					camera.Primary = false; // The scene's camera stays in charge
					if (node.camera->type == cgltf_camera_type_perspective)
					{
						const cgltf_camera_perspective& perspective = node.camera->data.perspective;
						camera.Projection = ProjectionType::Perspective;
						camera.PerspectiveFOV = glm::clamp(glm::degrees(perspective.yfov), 1.0f, 179.0f);
						camera.PerspectiveNear = std::max(perspective.znear * scale, 0.001f);
						const float farClip = perspective.zfar * scale;
						camera.PerspectiveFar = perspective.has_zfar && farClip > camera.PerspectiveNear ? farClip
							: std::max(camera.PerspectiveFar, camera.PerspectiveNear * 1000.0f);
					}
					else if (node.camera->type == cgltf_camera_type_orthographic)
					{
						const cgltf_camera_orthographic& orthographic = node.camera->data.orthographic;
						camera.Projection = ProjectionType::Orthographic;
						camera.OrthographicSize = std::max(orthographic.ymag * 2.0f * scale, 0.001f);
						camera.OrthographicNear = orthographic.znear * scale;
						if (orthographic.zfar > orthographic.znear)
							camera.OrthographicFar = orthographic.zfar * scale;
					}
				}

				if (node.light && m_Settings.ImportLights)
					AddLight(entity, *node.light);
			}

			// Light intensities are imported 1:1: Strata interprets point and spot intensity like glTF candela
			// (inverse-square falloff) and directional intensity like lux, both relative to camera exposure.
			void AddLight(Entity entity, const cgltf_light& light)
			{
				const glm::vec3 color = glm::make_vec3(light.color);
				const float intensity = std::max(light.intensity, 0.0f);
				// glTF ranges are model distances and scale with the model. Without one (infinite), the light is cut off
				// where it drops below 1% of unit intensity; that distance is in world units, as intensities are not scaled.
				const float range = light.range > 0.0f ? light.range * m_Settings.Scale : std::max(10.0f * std::sqrt(intensity), 0.1f);
				switch (light.type)
				{
					case cgltf_light_type_directional:
					{
						DirectionalLightComponent& component = entity.AddComponent<DirectionalLightComponent>();
						component.Color = color;
						component.Intensity = intensity;
						break;
					}
					case cgltf_light_type_point:
					{
						PointLightComponent& component = entity.AddComponent<PointLightComponent>();
						component.Color = color;
						component.Intensity = intensity;
						component.Range = range;
						break;
					}
					case cgltf_light_type_spot:
					{
						SpotLightComponent& component = entity.AddComponent<SpotLightComponent>();
						component.Color = color;
						component.Intensity = intensity;
						component.Range = range;
						component.OuterConeAngle = glm::clamp(glm::degrees(light.spot_outer_cone_angle), 1.0f, 89.0f);
						component.InnerConeAngle = glm::clamp(glm::degrees(light.spot_inner_cone_angle), 0.0f, component.OuterConeAngle);
						break;
					}
					default:
						break;
				}
			}

			// The joint of the skeleton a node is (c_NoJoint for nodes outside it).
			JointIndex GetNodeJoint(const cgltf_node& node) const
			{
				return m_NodeJoints.empty() ? c_NoJoint : m_NodeJoints[cgltf_node_index(m_Data.get(), &node)];
			}

			// Whether a joint has something the scene needs as an entity: a rigid mesh, a camera or light that is imported, or a
			// child outside the skeleton (whose transform is relative to it). Joints without any are only part of the pose.
			bool HasEntityContent(const cgltf_node& node) const
			{
				if (node.mesh || (node.camera && m_Settings.ImportCameras) || (node.light && m_Settings.ImportLights))
					return true;
				for (cgltf_size child = 0; child < node.children_count; child++)
				{
					if (GetNodeJoint(*node.children[child]) == c_NoJoint)
						return true;
				}
				return false;
			}

			// Builds the entity hierarchy iteratively (no recursion on deep files); fails above the depth limit. With a skeleton
			// the model's root is its animator: skinned meshes are placed under it with a Skin naming it, joints with content
			// become entities under it that follow their joint (AttachToJoint), and other joints are no entities at all (the
			// animator's pose holds them).
			bool AddHierarchy(Scene& scene, const std::vector<const cgltf_node*>& roots, Entity parent, std::string& outError)
			{
				struct PendingNode
				{
					const cgltf_node* Node;
					Entity Parent;
					uint32_t Depth;
				};
				std::vector<PendingNode> stack;
				for (auto it = roots.rbegin(); it != roots.rend(); ++it)
					stack.push_back(PendingNode { *it, parent, 1 });

				std::unordered_set<const cgltf_node*> visited;
				while (!stack.empty())
				{
					const PendingNode pending = stack.back();
					stack.pop_back();
					if (!visited.insert(pending.Node).second)
					{
						Warn(fmt::format("Node {} is listed more than once; the duplicate is ignored", cgltf_node_index(m_Data.get(), pending.Node)));
						continue;
					}
					if (pending.Depth > c_MaxHierarchyDepth)
					{
						outError = fmt::format("the node hierarchy is deeper than {} levels", c_MaxHierarchyDepth);
						return false;
					}

					const cgltf_node& node = *pending.Node;
					const cgltf_size nodeIndex = cgltf_node_index(m_Data.get(), &node);
					const MeshImport* mesh = FindMeshImport(node);
					const bool skinned = mesh && mesh->Skinned;
					const JointIndex joint = skinned ? c_NoJoint : GetNodeJoint(node);
					if (joint != c_NoJoint && !HasEntityContent(node))
					{
						// Only a joint: its children are joints too, each placed on its own.
						for (cgltf_size child = node.children_count; child > 0; child--)
							stack.push_back(PendingNode { node.children[child - 1], parent, pending.Depth + 1 });
						continue;
					}
					// Entity ids derive from the model handle, so re-imports produce the same ids.
					Entity entity = scene.CreateEntityWithUUID(DeriveSubAssetHandle(m_Context.Handle, fmt::format("Node/{}", nodeIndex)), GetName(node.name, "Node", nodeIndex));
					if (!entity)
					{
						outError = "the model has more nodes than a scene can hold";
						return false;
					}
					// Skinned meshes are placed by their joints and joints by the pose, both relative to the model's root (the
					// animator), whatever their place in the file's hierarchy.
					scene.SetParent(entity, skinned || joint != c_NoJoint ? parent : pending.Parent, false);
					SetupEntity(entity, node);
#if ST_WITH_ANIMATION
					if (skinned)
						entity.AddComponent<SkinComponent>().Animator = parent.GetUUID();
					if (joint != c_NoJoint)
					{
						// At the joint's rest pose until the animator poses it.
						AttachToJointComponent& attachment = entity.AddComponent<AttachToJointComponent>();
						attachment.Animator = parent.GetUUID();
						attachment.Joint = m_JointNames[joint];
						TransformComponent& transform = entity.GetComponent<TransformComponent>();
						glm::vec3 translation;
						glm::quat rotation;
						glm::vec3 scale;
						if (Math::DecomposeTransform(m_JointRestModel[joint], translation, rotation, scale))
						{
							transform.Translation = translation;
							transform.Rotation = rotation;
							transform.Scale = scale;
						}
					}
#endif

					for (cgltf_size child = node.children_count; child > 0; child--)
						stack.push_back(PendingNode { node.children[child - 1], entity, pending.Depth + 1 });
				}
				return true;
			}

			bool BuildModel(std::string* outError)
			{
				Scene scene;
				const std::string modelName = FileSystem::ToUTF8(m_Context.SourcePath.stem());
				Entity root = scene.CreateEntityWithUUID(DeriveSubAssetHandle(m_Context.Handle, "Node/Root"), modelName);
				root.GetComponent<TransformComponent>().Scale = glm::vec3(m_Settings.Scale);

				// Without a scene every root node is part of the model.
				const std::vector<const cgltf_node*> roots = GetRootNodes();
				if (m_Data->scenes_count > 1)
					m_SkippedFeatures.insert(fmt::format("{} additional scenes", m_Data->scenes_count - 1));

				std::string error;
				if (!AddHierarchy(scene, roots, root, error))
					return Fail(outError, error);
#if ST_WITH_ANIMATION
				if (m_SkeletonHandle.IsValid())
				{
					// The model's animator: its skeleton, playing the first animation (a controller can take over).
					AnimatorComponent& animator = root.AddComponent<AnimatorComponent>();
					animator.Skeleton = m_SkeletonHandle;
					animator.Clip = m_ClipHandles.empty() ? UUID::Null() : m_ClipHandles.front();
				}
#else
				if (m_Data->animations_count > 0)
					m_SkippedFeatures.insert("animations (the engine has no animation module)");
#endif

				Ref<Model> model = Model::CreateFromSnapshot(SceneSerializer::SerializeEntities(scene, { root }));
				if (!model)
					return Fail(outError, "building the model failed");
				const std::string document = JsonUtils::Dump(model->Serialize());
				m_Result.Data.assign(document.begin(), document.end());
				return true;
			}
		private:
			const ImportContext& m_Context;
			ImportResult& m_Result;
			ImportSettings m_Settings;
			FileReadContext m_FileContext;
			cgltf_options m_Options = {};
			std::vector<uint8_t> m_FileData; // Declared before m_Data: destroyed after it
			std::unique_ptr<cgltf_data, CgltfDataDeleter> m_Data;

			std::map<std::string, AssetHandle> m_TextureHandles;
			std::map<const cgltf_material*, AssetHandle> m_MaterialHandles;
			std::map<std::pair<const cgltf_mesh*, const cgltf_skin*>, MeshImport> m_MeshHandles; // By mesh and the skin it is drawn with
			std::vector<std::vector<const cgltf_skin*>> m_MeshSkins; // CollectMeshSkins
			std::set<std::string> m_SkippedFeatures;

			// The skeleton: each node's joint (c_NoJoint for nodes outside it; empty without a skeleton) and the joints' names.
			std::vector<JointIndex> m_NodeJoints;
			std::vector<std::string> m_JointNames;
			std::vector<glm::mat4> m_JointRestModel; // Per joint, its rest transform relative to the model's root
			AssetHandle m_SkeletonHandle = UUID::Null();
			std::map<const cgltf_skin*, MeshSkinBinding> m_Skins; // Empty joints: the skin cannot be used
			std::vector<AssetHandle> m_ClipHandles;
		};

	}

	nlohmann::json GltfImporter::GetDefaultSettings(const std::filesystem::path&) const
	{
		return {
			{ "Scale", 1.0f },
			{ "GenerateLODs", true },
			{ "LODCount", 3 },
			{ "OptimizeMeshes", true },
			{ "ImportMaterials", true },
			{ "ImportCameras", true },
			{ "ImportLights", true },
			{ "MaxTextureSize", 0 },
			{ "TextureCompression", TextureCompressionToString(TextureCompression::Auto) },
			{ "TextureQuality", TextureQualityToString(TextureQuality::High) }
		};
	}

	std::string GltfImporter::GetInactiveReason(std::string_view setting, const nlohmann::json& settings) const
	{
		std::vector<std::string> warnings;
		const ImportSettings values = ReadSettings(settings, warnings);
		if (setting == "LODCount" && !values.GenerateLODs)
			return "Simplified versions are made only with Generate LODs";
		const bool textureSetting = setting == "MaxTextureSize" || setting == "TextureCompression" || setting == "TextureQuality";
		if (textureSetting && !values.ImportMaterials)
			return "The model's textures are imported with its materials: Import Materials is off";
		if (setting == "TextureQuality" && values.Compression == TextureCompression::Uncompressed)
			return "Quality only chooses how block compression encodes; with Texture Compression Uncompressed every texel is kept exact";
		return {};
	}

	std::vector<PropertyInfo> GltfImporter::GetSettingsProperties() const
	{
		const ImportSettings defaults;
		std::vector<PropertyInfo> properties;
		ImportSettingsBuilder(properties)
			.Float("Scale", defaults.Scale, c_MinImportScale, c_MaxImportScale).Speed(0.01f)
				.Tooltip("Uniform scale of the model's root (glTF uses meters, like Strata); also scales imported cameras' and lights' ranges")
			.Bool("GenerateLODs", defaults.GenerateLODs).DisplayName("Generate LODs")
				.Tooltip("Generate simplified versions of every mesh, drawn when it covers less of the screen")
			.UInt("LODCount", defaults.LODCount, c_MaxLODCount).DisplayName("LOD Count")
				.Tooltip("Simplified versions per mesh besides the original")
			.UInt("MaxTextureSize", defaults.MaxTextureSize, TextureImporter::c_MaxImageDimension).PowerOfTwoLimit()
				.Tooltip("Longest side of the model's textures in pixels: larger images are scaled down. No limit (0): only the engine's limit")
			.Enum("TextureCompression", { "Auto", "Uncompressed" }, TextureCompressionToString(defaults.Compression))
				.Tooltip("The model's textures, as a texture's Compression: Auto block-compresses each in the format its use in the "
					"materials needs; Uncompressed keeps every texel exact")
			.Enum("TextureQuality", { "High", "Fast" }, TextureQualityToString(defaults.Quality))
				.Tooltip("The model's textures, as a texture's Quality: High searches every BC7 mode; Fast stores opaque color as BC1 and "
					"imports large textures several times faster")
			.Bool("OptimizeMeshes", defaults.OptimizeMeshes)
				.Tooltip("Reorder vertices and triangles for the GPU's caches")
			.Bool("ImportMaterials", defaults.ImportMaterials)
				.Tooltip("Import the model's materials and textures; without them, meshes use the default material")
			.Bool("ImportCameras", defaults.ImportCameras)
				.Tooltip("Import the model's cameras as camera components")
			.Bool("ImportLights", defaults.ImportLights)
				.Tooltip("Import the model's punctual lights (KHR_lights_punctual) as light components");
		return properties;
	}

	bool GltfImporter::Import(const ImportContext& context, ImportResult& result, std::string* outError) const
	{
		GltfImport import(context, result);
		return import.Run(outError);
	}

	uint64_t GltfImporter::EstimateImportMemory(const ImportEstimateSource& source, const nlohmann::json& settings) const
	{
		const uint64_t fallback = AssetImporter::EstimateImportMemory(source, settings);
		// The document: a .glb's JSON chunk (after 12 bytes of header and the chunk's length and type; its binary chunk
		// follows), or all of a .gltf, read beyond the head when it is longer.
		const std::span<const uint8_t> head = source.Head;
		std::span<const uint8_t> document = head;
		std::optional<std::vector<uint8_t>> read;
		const auto readDocument = [&](uint64_t offset, uint64_t length)
		{
			if (length > c_MaxEstimatedDocumentBytes)
				return false;
			read = ReadEstimateBytes(source, source.Path, offset, length);
			if (!read || read->size() != length)
				return false;
			document = *read;
			return true;
		};
		std::optional<uint64_t> binaryChunk; // Where a .glb's binary chunk's bytes start
		if (head.size() >= 20 && std::memcmp(head.data(), "glTF", 4) == 0)
		{
			uint32_t jsonLength = 0;
			std::memcpy(&jsonLength, head.data() + 12, sizeof(jsonLength));
			if (std::memcmp(head.data() + 16, "JSON", 4) != 0)
				return fallback;
			binaryChunk = 20ull + jsonLength + 8;
			if (jsonLength <= head.size() - 20)
				document = head.subspan(20, jsonLength);
			else if (!readDocument(20, jsonLength))
				return fallback;
		}
		else if (head.size() < source.Size && !readDocument(0, source.Size))
		{
			return fallback;
		}
		const std::optional<nlohmann::json> json = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(document.data()), document.size()));
		if (!json || !json->is_object())
			return fallback;
		// The document is the file's word: lengths a crafted file makes huge saturate instead of wrapping around.
		uint64_t estimate = 0;
		const auto add = [&estimate](uint64_t length, uint64_t factor)
		{
			const uint64_t bytes = length > UINT64_MAX / factor ? UINT64_MAX : length * factor;
			estimate = bytes > UINT64_MAX - estimate ? UINT64_MAX : estimate + bytes;
		};
		const nlohmann::json* buffers = JsonUtils::Find(*json, "buffers");
		if (buffers && buffers->is_array())
		{
			for (const nlohmann::json& buffer : *buffers)
				add(JsonUtils::GetUInt(buffer, "byteLength", 0), 2);
		}

		// Images by their headers (TextureImporter::EstimateImageMemory: they are cooked like textures), wherever they are:
		// files beside the model, or buffer views of the .glb's binary chunk or of a buffer file. Images in data URIs and
		// those whose bytes cannot be read count by their size (their import reads them, or fails).
		const std::filesystem::path folder = source.Path.parent_path();
		// The file a relative URI names; null for data and other URIs.
		const auto findFile = [&folder](const nlohmann::json& object) -> std::optional<std::filesystem::path>
		{
			const nlohmann::json* uri = JsonUtils::Find(object, "uri");
			if (!uri || !uri->is_string())
				return std::nullopt;
			std::string decoded = uri->get<std::string>();
			if (decoded.starts_with("data:") || decoded.find("://") != std::string::npos)
				return std::nullopt;
			decoded.resize(cgltf_decode_uri(decoded.data()));
			return folder / FileSystem::FromUTF8(decoded);
		};
		const nlohmann::json* views = JsonUtils::Find(*json, "bufferViews");
		const nlohmann::json* images = JsonUtils::Find(*json, "images");
		if (!images || !images->is_array())
			return std::max(estimate, fallback);
		for (const nlohmann::json& image : *images)
		{
			if (!image.is_object())
				continue;
			const uint64_t viewIndex = JsonUtils::GetUInt(image, "bufferView", UINT64_MAX);
			if (views && views->is_array() && viewIndex < views->size())
			{
				const nlohmann::json& view = (*views)[static_cast<size_t>(viewIndex)];
				const uint64_t length = JsonUtils::GetUInt(view, "byteLength", 0);
				const uint64_t offset = JsonUtils::GetUInt(view, "byteOffset", 0);
				const uint64_t bufferIndex = JsonUtils::GetUInt(view, "buffer", UINT64_MAX);
				std::optional<std::filesystem::path> file;
				uint64_t start = 0;
				if (buffers && buffers->is_array() && bufferIndex < buffers->size())
				{
					const nlohmann::json& buffer = (*buffers)[static_cast<size_t>(bufferIndex)];
					if (buffer.is_object() && !buffer.contains("uri") && bufferIndex == 0 && binaryChunk)
					{
						file = source.Path;
						start = *binaryChunk;
					}
					else if (buffer.is_object())
					{
						file = findFile(buffer);
					}
				}
				if (file && offset <= UINT64_MAX - start)
					add(TextureImporter::EstimateImageMemory(source, *file, start + offset, length), 1);
				else
					add(length, TextureImporter::c_UnknownImageFactor);
				continue;
			}
			if (const std::optional<std::filesystem::path> file = findFile(image))
			{
				if (const std::optional<uint64_t> size = GetEstimateFileSize(source, *file))
					add(TextureImporter::EstimateImageMemory(source, *file, 0, *size), 1);
				continue; // A missing image fails the import
			}
			// A data URI holds its bytes in base64: three for every four characters.
			if (const nlohmann::json* uri = JsonUtils::Find(image, "uri"); uri && uri->is_string())
				add(uri->get_ref<const std::string&>().size() / 4 * 3, TextureImporter::c_UnknownImageFactor);
		}
		return std::max(estimate, fallback);
	}

	bool GltfImporter::FindExternalFiles(std::span<const uint8_t> data, std::vector<std::string>& outRelativePaths, std::string* outError) const
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		// Parsing reads no other file (cgltf_load_buffers would): the document only names them.
		cgltf_options options = {};
		cgltf_data* parsed = nullptr;
		const cgltf_result result = cgltf_parse(&options, data.data(), data.size(), &parsed);
		const std::unique_ptr<cgltf_data, CgltfDataDeleter> document(parsed);
		if (result != cgltf_result_success)
			return fail(fmt::format("parsing failed ({})", ResultToString(result)));

		std::set<std::string> listed;
		auto add = [&](const char* uri, std::string_view kind) -> bool
		{
			if (!uri)
				return true;
			const std::string_view text = uri;
			if (text.rfind("data:", 0) == 0)
				return true; // Inside the file
			if (text.find("://") != std::string_view::npos)
				return fail(fmt::format("the {} '{}' is a URL, not a file beside the model", kind, text));
			std::string decoded(text);
			decoded.resize(cgltf_decode_uri(decoded.data()));
			std::replace(decoded.begin(), decoded.end(), '\\', '/');
			if (decoded.empty() || decoded.front() == '/' || decoded.find(':') != std::string::npos)
				return fail(fmt::format("the {} '{}' is no relative file path", kind, text));
			if (listed.insert(decoded).second)
				outRelativePaths.push_back(std::move(decoded));
			return true;
		};
		for (cgltf_size index = 0; index < document->buffers_count; index++)
		{
			if (!add(document->buffers[index].uri, "buffer"))
				return false;
		}
		for (cgltf_size index = 0; index < document->images_count; index++)
		{
			if (!add(document->images[index].uri, "image"))
				return false;
		}
		return true;
	}

}

#include "stpch.h"
#include "Strata/Asset/GltfImporter.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/TextureImporter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		// Extensions that only change how existing data is interpreted; a model using them imports fine without them.
		constexpr const char* c_SupportedExtensions[] = {
			"KHR_lights_punctual", "KHR_materials_emissive_strength", "KHR_materials_unlit", "KHR_texture_transform", "KHR_mesh_quantization"
		};

		// Limits that keep hostile or broken files from exhausting memory, time or the stack. The geometry an import
		// expands (vertices plus triangle corners over all primitives) is limited relative to the loaded buffers:
		// accessors can describe far more data than a file holds (zero-filled accessors, accessors shared by many
		// primitives).
		constexpr uint64_t c_MaxAccessorElements = 1ull << 30;
		constexpr uint64_t c_MinGeometryBudget = 1ull << 22;  // Elements every file may expand to
		constexpr uint64_t c_GeometryBudgetPerBufferByte = 4; // Elements per loaded buffer byte beyond that
		constexpr uint64_t c_MaxGeometryBudget = 1ull << 28;
		constexpr uint32_t c_MaxHierarchyDepth = 1024;

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
			settings.LODCount = static_cast<uint32_t>(std::min<uint64_t>(JsonUtils::GetUInt(json, "LODCount", settings.LODCount), 8));
			settings.OptimizeMeshes = JsonUtils::GetBool(json, "OptimizeMeshes", settings.OptimizeMeshes);
			settings.ImportMaterials = JsonUtils::GetBool(json, "ImportMaterials", settings.ImportMaterials);
			settings.ImportCameras = JsonUtils::GetBool(json, "ImportCameras", settings.ImportCameras);
			settings.ImportLights = JsonUtils::GetBool(json, "ImportLights", settings.ImportLights);
			settings.MaxTextureSize = static_cast<uint32_t>(std::min<uint64_t>(JsonUtils::GetUInt(json, "MaxTextureSize", 0), TextureImporter::c_MaxImageDimension));
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

				if (m_Settings.ImportMaterials)
				{
					for (cgltf_size index = 0; index < m_Data->materials_count; index++)
						ImportMaterial(index);
				}

				for (cgltf_size index = 0; index < m_Data->meshes_count; index++)
					ImportMesh(index);

				if (!BuildModel(outError))
					return false;

				for (const std::string& feature : m_SkippedFeatures)
					m_Result.Warnings.push_back(fmt::format("Not imported: {}", feature));
				return true;
			}
		private:
			bool Load(std::string* outError)
			{
				std::optional<std::vector<uint8_t>> fileData = FileSystem::ReadBytes(m_Context.SourcePath);
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
				// chain: check the layout and bound the hierarchy first.
				std::string structureError;
				if (!ValidateLayout(structureError) || !ValidateGeometryBudget(structureError) || !ValidateHierarchy(structureError))
					return Fail(outError, structureError);

				result = cgltf_validate(m_Data.get());
				if (result != cgltf_result_success)
					return Fail(outError, fmt::format("validation failed ({})", ResultToString(result)));

				m_Options = options;
				if (m_Data->skins_count > 0)
					m_SkippedFeatures.insert("skins");
				if (m_Data->animations_count > 0)
					m_SkippedFeatures.insert("animations");
				return true;
			}

			// Overflow-safe bounds checks of every buffer view and accessor against the loaded data.
			bool ValidateLayout(std::string& outError) const
			{
				for (cgltf_size index = 0; index < m_Data->buffers_count; index++)
				{
					const cgltf_buffer& buffer = m_Data->buffers[index];
					if (buffer.size > 0 && !buffer.data)
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
			// and may unweld its triangles, so the vertices and triangle corners of all primitives are counted.
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
						elements += vertexCount + corners; // Each term is below 2^32: no overflow before the check
						if (elements > budget)
						{
							outError = fmt::format("the meshes expand to more geometry than the file's data supports (limit {} vertices and corners)", budget);
							return false;
						}
					}
				}
				return true;
			}

			// Rejects parent chains deeper than the hierarchy limit, which includes cycles (depths are memoized: linear).
			bool ValidateHierarchy(std::string& outError) const
			{
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
					const uint8_t* begin = static_cast<const uint8_t*>(view.buffer->data) + view.offset; // Range validated in ValidateLayout
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
					decoded = TextureImporter::Decode(encoded, settings, imageName, &error);
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

			void ImportMesh(cgltf_size meshIndex)
			{
				const cgltf_mesh& source = m_Data->meshes[meshIndex];
				const std::string meshName = GetName(source.name, "Mesh", meshIndex);

				std::vector<glm::vec3> positions;
				std::vector<MeshVertexAttributes> attributes;
				std::vector<uint32_t> indices;
				std::vector<Submesh> submeshes;

				for (cgltf_size primitiveIndex = 0; primitiveIndex < source.primitives_count; primitiveIndex++)
				{
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
							case cgltf_attribute_type_color:  m_SkippedFeatures.insert("vertex colors"); break;
							case cgltf_attribute_type_joints:
							case cgltf_attribute_type_weights: m_SkippedFeatures.insert("skinning"); break;
							default: break;
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
						if (!MeshUtils::GenerateSeamTangents(primitivePositions, primitiveAttributes, primitiveIndices, !hasNormals))
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
							for (size_t corner = 0; corner < primitiveIndices.size(); corner += 3)
							{
								const glm::vec3& a = primitivePositions[primitiveIndices[corner]];
								const glm::vec3& b = primitivePositions[primitiveIndices[corner + 1]];
								const glm::vec3& c = primitivePositions[primitiveIndices[corner + 2]];
								const glm::vec3 faceNormal = glm::cross(b - a, c - a);
								const float length = glm::length(faceNormal);
								MeshVertexAttributes attribute;
								attribute.Normal = length > 1e-12f ? faceNormal / length : glm::vec3(0.0f, 1.0f, 0.0f);
								for (const glm::vec3* position : { &a, &b, &c })
								{
									flatPositions.push_back(*position);
									flatAttributes.push_back(attribute);
								}
							}
							primitivePositions = std::move(flatPositions);
							primitiveAttributes = std::move(flatAttributes);
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
					submeshes.push_back(std::move(submesh));
				}

				if (submeshes.empty())
				{
					Warn(fmt::format("Mesh '{}' has no importable primitives", meshName));
					return;
				}

				std::string error;
				Ref<Mesh> mesh = Mesh::Create(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), &error);
				if (!mesh)
				{
					Warn(fmt::format("Mesh '{}' skipped: {}", meshName, error));
					return;
				}

				const std::string key = fmt::format("Mesh/{}", meshIndex);
				ImportedSubAsset subAsset;
				subAsset.Key = key;
				subAsset.Name = meshName;
				subAsset.Type = AssetType::Mesh;
				subAsset.Data = mesh->Serialize();
				m_Result.SubAssets.push_back(std::move(subAsset));
				m_MeshHandles[&source] = DeriveSubAssetHandle(m_Context.Handle, key);
			}

			////////////////////////////////////////////////////////////////////////////////
			// Hierarchy
			////////////////////////////////////////////////////////////////////////////////

			void SetupEntity(Entity entity, const cgltf_node& node)
			{
				TransformComponent& transform = entity.GetComponent<TransformComponent>();
				if (node.has_matrix)
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

				if (node.mesh)
				{
					auto mesh = m_MeshHandles.find(node.mesh);
					if (mesh != m_MeshHandles.end())
						entity.AddComponent<MeshRendererComponent>().Mesh = mesh->second;
				}
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

			// Builds the entity hierarchy iteratively (no recursion on deep files); fails above the depth limit.
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
					// Entity ids derive from the model handle, so re-imports produce the same ids.
					Entity entity = scene.CreateEntityWithUUID(DeriveSubAssetHandle(m_Context.Handle, fmt::format("Node/{}", nodeIndex)), GetName(node.name, "Node", nodeIndex));
					scene.SetParent(entity, pending.Parent, false);
					SetupEntity(entity, node);

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

				std::vector<const cgltf_node*> roots;
				const cgltf_scene* gltfScene = m_Data->scene ? m_Data->scene : (m_Data->scenes_count > 0 ? &m_Data->scenes[0] : nullptr);
				if (gltfScene)
				{
					for (cgltf_size index = 0; index < gltfScene->nodes_count; index++)
						roots.push_back(gltfScene->nodes[index]);
				}
				else
				{
					// No scene: every root node is part of the model.
					for (cgltf_size index = 0; index < m_Data->nodes_count; index++)
					{
						if (!m_Data->nodes[index].parent)
							roots.push_back(&m_Data->nodes[index]);
					}
				}
				if (m_Data->scenes_count > 1)
					m_SkippedFeatures.insert(fmt::format("{} additional scenes", m_Data->scenes_count - 1));

				std::string error;
				if (!AddHierarchy(scene, roots, root, error))
					return Fail(outError, error);

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
			std::map<const cgltf_mesh*, AssetHandle> m_MeshHandles;
			std::set<std::string> m_SkippedFeatures;
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
			{ "MaxTextureSize", 0 }
		};
	}

	bool GltfImporter::Import(const ImportContext& context, ImportResult& result, std::string* outError) const
	{
		GltfImport import(context, result);
		return import.Run(outError);
	}

}

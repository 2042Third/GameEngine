# Strata third-party dependencies.
#
# Every dependency is a pinned, shallow git submodule (see /.gitmodules). Vendor code compiles without
# warnings and is consumed by Strata targets as SYSTEM includes, so it never trips first-party /WX.
# To upgrade a dependency: check out the new tag inside its submodule, rebuild, run all tests, and
# update ThirdPartyNotices.md.

set(CMAKE_FOLDER "Vendor")
set(BUILD_SHARED_LIBS OFF)

if(MSVC)
	string(REGEX REPLACE "/W[0-4]" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
	string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(APPEND CMAKE_C_FLAGS " /W0 /utf-8")
	string(APPEND CMAKE_CXX_FLAGS " /W0 /utf-8")
else()
	string(APPEND CMAKE_C_FLAGS " -w")
	string(APPEND CMAKE_CXX_FLAGS " -w")
endif()

macro(strata_vendor_option name value)
	set(${name} ${value} CACHE INTERNAL "Set by Strata/vendor/CMakeLists.txt")
endmacro()

set(STRATA_VENDOR_IMPLEMENTATIONS "${CMAKE_CURRENT_SOURCE_DIR}/Implementations")

# --- Vulkan-Headers (Vulkan::Headers) -------------------------------------------------------------
strata_vendor_option(VULKAN_HEADERS_ENABLE_TESTS OFF)
strata_vendor_option(VULKAN_HEADERS_ENABLE_INSTALL OFF)
strata_vendor_option(VULKAN_HEADERS_ENABLE_MODULE OFF)
add_subdirectory(Vulkan-Headers SYSTEM)

# --- NVRHI (nvrhi, nvrhi_vk) ----------------------------------------------------------------------
strata_vendor_option(NVRHI_WITH_VULKAN ON)
strata_vendor_option(NVRHI_WITH_DX11 OFF)
strata_vendor_option(NVRHI_WITH_DX12 OFF)
strata_vendor_option(NVRHI_WITH_VALIDATION ON)
strata_vendor_option(NVRHI_WITH_RTXMU OFF)
strata_vendor_option(NVRHI_WITH_NVAPI OFF)
strata_vendor_option(NVRHI_WITH_AFTERMATH OFF)
strata_vendor_option(NVRHI_BUILD_SHARED OFF)
strata_vendor_option(NVRHI_BUILD_TESTS OFF)
strata_vendor_option(NVRHI_INSTALL OFF)
strata_vendor_option(NVRHI_FETCH_VULKAN_HEADERS OFF)
add_subdirectory(NVRHI SYSTEM)
# The Vulkan backend uses the common library (validation, state tracking), which NVRHI does not declare. Linkers that
# resolve static libraries in command-line order (GNU ld) need nvrhi after nvrhi_vk.
target_link_libraries(nvrhi_vk PUBLIC nvrhi)

# --- GLFW (glfw) ----------------------------------------------------------------------------------
strata_vendor_option(GLFW_BUILD_DOCS OFF)
strata_vendor_option(GLFW_BUILD_TESTS OFF)
strata_vendor_option(GLFW_BUILD_EXAMPLES OFF)
strata_vendor_option(GLFW_INSTALL OFF)
add_subdirectory(glfw SYSTEM)

# --- glm (header-only) ----------------------------------------------------------------------------
# No layout-changing GLM_FORCE_* defines: glm types cross the script C ABI and must stay plain.
add_library(glm INTERFACE)
add_library(glm::glm ALIAS glm)
target_include_directories(glm SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/glm")
target_compile_definitions(glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL GLM_FORCE_SILENT_WARNINGS)

# --- EnTT (header-only) ---------------------------------------------------------------------------
add_library(EnTT INTERFACE)
add_library(EnTT::EnTT ALIAS EnTT)
target_include_directories(EnTT SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/entt/src")

# --- spdlog (spdlog::spdlog, bundled fmt) ---------------------------------------------------------
strata_vendor_option(SPDLOG_BUILD_EXAMPLE OFF)
strata_vendor_option(SPDLOG_BUILD_TESTS OFF)
strata_vendor_option(SPDLOG_BUILD_BENCH OFF)
strata_vendor_option(SPDLOG_INSTALL OFF)
strata_vendor_option(SPDLOG_USE_STD_FORMAT OFF)
strata_vendor_option(SPDLOG_FMT_EXTERNAL OFF)
add_subdirectory(spdlog SYSTEM)

# --- nlohmann/json (nlohmann_json::nlohmann_json) -------------------------------------------------
strata_vendor_option(JSON_BuildTests OFF)
strata_vendor_option(JSON_Install OFF)
strata_vendor_option(JSON_ImplicitConversions OFF)
add_subdirectory(json SYSTEM)

# Libraries only tools use (the editor, the tests) are excluded from all: a build of the runtime alone never compiles them.

# --- Dear ImGui, docking branch (imgui) -----------------------------------------------------------
# The core only: its platform backend (Strata/ImGui/ImGuiPlatform) and renderer backend (ImGuiRenderer) are the engine's.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
add_library(imgui STATIC EXCLUDE_FROM_ALL
	"${IMGUI_DIR}/imgui.cpp"
	"${IMGUI_DIR}/imgui_demo.cpp"
	"${IMGUI_DIR}/imgui_draw.cpp"
	"${IMGUI_DIR}/imgui_tables.cpp"
	"${IMGUI_DIR}/imgui_widgets.cpp"
	"${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp")
target_include_directories(imgui SYSTEM PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/misc/cpp")
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# --- ImGuizmo -------------------------------------------------------------------------------------
add_library(ImGuizmo STATIC EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src/ImGuizmo.cpp")
target_include_directories(ImGuizmo SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src")
target_link_libraries(ImGuizmo PUBLIC imgui)

# --- stb (image, image_write, image_resize2, dxt, truetype, rect_pack) -----------------------------
# One object per header, so that the linker leaves out what a program never calls (games: the DXT encoder and the
# resizer). rect_pack and truetype share one: stb_truetype uses rect_pack's packer when it sees its implementation.
add_library(stb STATIC
	"${STRATA_VENDOR_IMPLEMENTATIONS}/StbImage.c"
	"${STRATA_VENDOR_IMPLEMENTATIONS}/StbImageWrite.c"
	"${STRATA_VENDOR_IMPLEMENTATIONS}/StbImageResize.c"
	"${STRATA_VENDOR_IMPLEMENTATIONS}/StbDxt.c"
	"${STRATA_VENDOR_IMPLEMENTATIONS}/StbTrueType.c")
target_include_directories(stb SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stb")

# --- cgltf ----------------------------------------------------------------------------------------
add_library(cgltf STATIC EXCLUDE_FROM_ALL "${STRATA_VENDOR_IMPLEMENTATIONS}/CgltfImplementation.c")
target_include_directories(cgltf SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cgltf")

# --- MikkTSpace -----------------------------------------------------------------------------------
add_library(MikkTSpace STATIC "${CMAKE_CURRENT_SOURCE_DIR}/MikkTSpace/mikktspace.c")
target_include_directories(MikkTSpace SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/MikkTSpace")

# --- meshoptimizer --------------------------------------------------------------------------------
strata_vendor_option(MESHOPT_BUILD_DEMO OFF)
strata_vendor_option(MESHOPT_BUILD_GLTFPACK OFF)
strata_vendor_option(MESHOPT_BUILD_SHARED_LIBS OFF)
strata_vendor_option(MESHOPT_INSTALL OFF)
add_subdirectory(meshoptimizer SYSTEM)

# --- miniaudio (+ stb_vorbis for Ogg Vorbis decoding) ---------------------------------------------
add_library(miniaudio STATIC "${STRATA_VENDOR_IMPLEMENTATIONS}/MiniaudioImplementation.c")
target_include_directories(miniaudio SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio")
target_include_directories(miniaudio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio/extras")
if(UNIX AND NOT APPLE)
	find_package(Threads REQUIRED)
	target_link_libraries(miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# --- Jolt Physics (Jolt) --------------------------------------------------------------------------
strata_vendor_option(OVERRIDE_CXX_FLAGS OFF)
strata_vendor_option(INTERPROCEDURAL_OPTIMIZATION OFF)
strata_vendor_option(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
strata_vendor_option(ENABLE_ALL_WARNINGS OFF)
strata_vendor_option(ENABLE_INSTALL OFF)
strata_vendor_option(ENABLE_OBJECT_STREAM OFF)
strata_vendor_option(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
strata_vendor_option(PROFILER_IN_DEBUG_AND_RELEASE OFF)
strata_vendor_option(USE_STATIC_MSVC_RUNTIME_LIBRARY ON)
strata_vendor_option(CPP_RTTI_ENABLED ON)
strata_vendor_option(CPP_EXCEPTIONS_ENABLED ON)
strata_vendor_option(JPH_USE_DX12 OFF)
strata_vendor_option(JPH_USE_VK OFF)
strata_vendor_option(JPH_USE_MTL OFF)
strata_vendor_option(TARGET_UNIT_TESTS OFF)
strata_vendor_option(TARGET_HELLO_WORLD OFF)
strata_vendor_option(TARGET_PERFORMANCE_TEST OFF)
strata_vendor_option(TARGET_SAMPLES OFF)
strata_vendor_option(TARGET_VIEWER OFF)
add_subdirectory(JoltPhysics/Build SYSTEM)

# --- zstd (zstd_decompress, zstd_compress) --------------------------------------------------------
# Asset packs store their blocks zstd-compressed. Games only decompress; compression is a separate library that only the
# pack writer (tooling) references, so a shipped game links none of it. Built without zstd's multithreading (packs
# compress their independent blocks on the engine's job system), without the legacy format decoders, and without the
# x86-64 assembly Huffman decoder: its .S file would need an assembler step on GCC and Clang only, and the portable C
# decoder it replaces is the one MSVC builds always use, so every platform runs the same code.
set(ZSTD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib")
add_library(zstd_decompress STATIC
	"${ZSTD_DIR}/common/debug.c"
	"${ZSTD_DIR}/common/entropy_common.c"
	"${ZSTD_DIR}/common/error_private.c"
	"${ZSTD_DIR}/common/fse_decompress.c"
	"${ZSTD_DIR}/common/pool.c"
	"${ZSTD_DIR}/common/threading.c"
	"${ZSTD_DIR}/common/xxhash.c"
	"${ZSTD_DIR}/common/zstd_common.c"
	"${ZSTD_DIR}/decompress/huf_decompress.c"
	"${ZSTD_DIR}/decompress/zstd_ddict.c"
	"${ZSTD_DIR}/decompress/zstd_decompress.c"
	"${ZSTD_DIR}/decompress/zstd_decompress_block.c")
target_include_directories(zstd_decompress SYSTEM PUBLIC "${ZSTD_DIR}")
target_compile_definitions(zstd_decompress PUBLIC ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0)
add_library(zstd_compress STATIC EXCLUDE_FROM_ALL
	"${ZSTD_DIR}/compress/fse_compress.c"
	"${ZSTD_DIR}/compress/hist.c"
	"${ZSTD_DIR}/compress/huf_compress.c"
	"${ZSTD_DIR}/compress/zstd_compress.c"
	"${ZSTD_DIR}/compress/zstd_compress_literals.c"
	"${ZSTD_DIR}/compress/zstd_compress_sequences.c"
	"${ZSTD_DIR}/compress/zstd_compress_superblock.c"
	"${ZSTD_DIR}/compress/zstd_double_fast.c"
	"${ZSTD_DIR}/compress/zstd_fast.c"
	"${ZSTD_DIR}/compress/zstd_lazy.c"
	"${ZSTD_DIR}/compress/zstd_ldm.c"
	"${ZSTD_DIR}/compress/zstd_opt.c"
	"${ZSTD_DIR}/compress/zstd_preSplit.c")
target_link_libraries(zstd_compress PUBLIC zstd_decompress)
# MSVC x86 builds require AVX2 already: Jolt compiles itself and its users with /arch:AVX2 (or AVX512; same condition).
# It lets zstd use its BMI2 bit readers (STATIC_BMI2), about 10% faster at decompressing; GCC and Clang builds pick
# them at run time (DYNAMIC_BMI2).
if(MSVC AND (USE_AVX2 OR USE_AVX512) AND CMAKE_VS_PLATFORM_NAME MATCHES "^(x86|x64)$")
	target_compile_options(zstd_decompress PRIVATE /arch:AVX2)
	target_compile_options(zstd_compress PRIVATE /arch:AVX2)
endif()

# --- glslang (glslang-standalone: build-time GLSL -> SPIR-V compiler) ------------------------------
strata_vendor_option(BUILD_EXTERNAL OFF)
strata_vendor_option(ENABLE_OPT OFF)
strata_vendor_option(ENABLE_HLSL OFF)
strata_vendor_option(ENABLE_GLSLANG_BINARIES ON)
strata_vendor_option(ENABLE_GLSLANG_JS OFF)
strata_vendor_option(ENABLE_SPIRV ON)
strata_vendor_option(GLSLANG_TESTS OFF)
strata_vendor_option(GLSLANG_ENABLE_INSTALL OFF)
add_subdirectory(glslang SYSTEM)

# --- doctest (header-only, tests) -----------------------------------------------------------------
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")

# --- Tracy profiler (optional) --------------------------------------------------------------------
if(STRATA_ENABLE_TRACY)
	strata_vendor_option(TRACY_ENABLE ON)
	strata_vendor_option(TRACY_ON_DEMAND ON)
	add_subdirectory(tracy SYSTEM)
endif()

# --- nativefiledialog-extended (editor) -----------------------------------------------------------
if(STRATA_BUILD_EDITOR)
	strata_vendor_option(NFD_BUILD_TESTS OFF)
	strata_vendor_option(NFD_INSTALL OFF)
	add_subdirectory(nfd SYSTEM EXCLUDE_FROM_ALL)
endif()

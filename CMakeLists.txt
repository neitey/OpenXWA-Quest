cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
        AND (NOT DEFINED CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Oldest macOS version supported by the generated binaries" FORCE)
endif()

project(OpenXWA C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Emit compile_commands.json in every build dir so clangd/LSP can resolve flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional AddressSanitizer build. Reconfigure with -DXWA_ENABLE_ASAN=ON to
# instrument every target (libraries + executable). ASAN red-zones globals, so
# it catches out-of-bounds writes between adjacent arrays (e.g. the iMUSE
# g_imMixBuffer/g_imOutputStage mix stage) as well as heap/stack overruns and
# use-after-free, including on the SDL audio thread. Must be set before targets
# are defined so the flags propagate via add_compile_options/add_link_options.
option(XWA_ENABLE_ASAN "Build with AddressSanitizer" OFF)
if(XWA_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "AddressSanitizer enabled (XWA_ENABLE_ASAN=ON)")
endif()

# The original game was built for x86, where plain `char` is signed. ARM
# Linux/Android ABIs make it unsigned, which silently breaks table lookups
# with negative indices (e.g. the iMUSE VIMA step-index adjust). Apple's ARM
# ABI already uses signed char.
if(NOT MSVC AND NOT APPLE AND (ANDROID OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|arm)"))
    add_compile_options(-fsigned-char)
endif()

# Aeron engine (window, GPU HAL, input, audio, VFS) — shared git submodule.
option(XWA_BUILD_TOOLS "Build OpenXWA and Aeron asset/developer tools" ON)
set(AERON_BUILD_TOOLS ${XWA_BUILD_TOOLS})
# Films need FFmpeg; OFF builds a stand-in that skips them (headless tests).
option(XWA_ENABLE_VIDEO "Play the game's films (needs FFmpeg)" ON)
set(AERON_ENABLE_VIDEO ${XWA_ENABLE_VIDEO})
set(AERON_HOST_TOOLS_ONLY OFF)
set(AERON_ENABLE_COMPAT ON)
# In-game debug UI (ImGui overlay, backquote to toggle).
option(XWA_ENABLE_DEBUG_UI "Build the Aeron ImGui debug overlay" ON)
set(AERON_DEBUG_UI ${XWA_ENABLE_DEBUG_UI})
add_subdirectory(aeron)

if(NOT XWA_ENABLE_VIDEO)
    add_library(aeron_video STATIC src/xwa_runtime/runtime/video_none.c)
    target_link_libraries(aeron_video PUBLIC aeron)
endif()

add_library(xwa_2d_formats STATIC
    src/xwa_formats/xwa_2d/xwa_2d.c
    src/xwa_formats/xwa_2d/xwa_dat.c
    src/xwa_formats/xwa_2d/xwa_flc.c
    src/xwa_formats/xwa_2d/xwa_font.c
    src/xwa_formats/xwa_2d/xwa_frontend_image.c
)
target_include_directories(xwa_2d_formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/xwa_formats/xwa_2d)
target_link_libraries(xwa_2d_formats PUBLIC aeron_asset)
set_target_properties(xwa_2d_formats PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
    target_link_libraries(xwa_2d_formats PRIVATE m)
endif()

set(XWA_RECOVERED_SOURCES
    src/xwa/audio/cd_audio.c
    src/xwa/audio/fsfx.c
    src/xwa/audio/music.c
    src/xwa/audio/imuse/imhost.c
    src/xwa/audio/imuse/imcodec.c
    src/xwa/audio/imuse/imuse.c
    src/xwa/audio/imuse/immap.c
    src/xwa/audio/imuse/imgroup.c
    src/xwa/audio/imuse/implayer.c
    src/xwa/audio/imuse/imscript.c
    src/xwa/audio/imuse/imsound.c
    src/xwa/audio/sound.c
    src/xwa/render/std3d_device.c
    src/xwa/render/std3d_texture.c
    src/xwa/console/console.c
    src/xwa/config/game_config.c
    src/xwa/config/pilot.c
    src/xwa/assets/file_io.c
    src/xwa/assets/flight_model.c
    src/xwa/assets/linez.c
    src/xwa/assets/model_bounds.c
    src/xwa/assets/model_def.c
    src/xwa/assets/model_mesh.c
    src/xwa/assets/model_texture.c
    src/xwa/assets/model_type.c
    src/xwa/assets/object_type.c
    src/xwa/assets/opt_model.c
    src/xwa/assets/ship_list.c
    src/xwa/assets/sprite_resource.c
    src/xwa/assets/sprite_texture.c
    src/xwa/assets/string_table.c
    src/xwa/frontend/briefing_room.c
    src/xwa/frontend/briefing_script.c
    src/xwa/frontend/combat_sim_menu.c
    src/xwa/frontend/concourse.c
    src/xwa/frontend/cutscene.c
    src/xwa/frontend/family_transport_room.c
    src/xwa/frontend/frontend_bootstrap.c
    src/xwa/frontend/frontend_color.c
    src/xwa/frontend/frontend_cursor.c
    src/xwa/frontend/frontend_button.c
    src/xwa/frontend/frontend_display.c
    src/xwa/frontend/frontend_dialog.c
    src/xwa/frontend/frontend_draw.c
    src/xwa/frontend/frontend_escape.c
    src/xwa/frontend/frontend_file_list.c
    src/xwa/frontend/frontend_file_stream.c
    src/xwa/frontend/frontend_flight.c
    src/xwa/frontend/frontend_image.c
    src/xwa/frontend/frontend_input.c
    src/xwa/frontend/frontend_medals.c
    src/xwa/frontend/frontend_email.c
    src/xwa/frontend/frontend_mission.c
    src/xwa/frontend/frontend_mission_list.c
    src/xwa/frontend/frontend_mission_session.c
    src/xwa/frontend/frontend_net.c
    src/xwa/frontend/net_transport.c
    src/xwa/frontend/frontend_resources.c
    src/xwa/frontend/frontend_scratch.c
    src/xwa/frontend/frontend_screen.c
    src/xwa/frontend/frontend_scrollbar.c
    src/xwa/frontend/frontend_sound.c
    src/xwa/frontend/frontend_text.c
    src/xwa/frontend/frontend_wave_stream.c
    src/xwa/frontend/film_room.c
    src/xwa/frontend/flight_loading.c
    src/xwa/frontend/mission_debrief.c
    src/xwa/frontend/mission_briefing.c
    src/xwa/frontend/mission_setup.c
    src/xwa/frontend/model_preview.c
    src/xwa/frontend/skirmish.c
    src/xwa/frontend/tech_library.c
    src/xwa/flight/ai/pai.c
    src/xwa/flight/ai/paifight.c
    src/xwa/flight/ai/paiman.c
    src/xwa/flight/ai/paiorder.c
    src/xwa/flight/ai/pai_plan.c
    src/xwa/flight/death_star.c
    src/xwa/flight/fediskio.c
    src/xwa/flight/film.c
    src/xwa/flight/flight.c
    src/xwa/flight/flight_net.c
    src/xwa/flight/flight_sync.c
    src/xwa/flight/flight_display.c
    src/xwa/flight/flight_light.c
    src/xwa/flight/flight_map.c
    src/xwa/flight/flight_text.c
    src/xwa/flight/flight_view.c
    src/xwa/flight/hangar.c
    src/xwa/flight/hud/hud.c
    src/xwa/flight/mission/mission.c
    src/xwa/flight/net_session.c
    src/xwa/flight/object/collision.c
    src/xwa/flight/object/damage.c
    src/xwa/flight/object/debris.c
    src/xwa/flight/object/laser.c
    src/xwa/flight/object/object.c
    src/xwa/flight/player/player.c
    src/xwa/flight/starfield.c
    src/xwa/flight/targeting.c
    src/xwa/flight/yard.c
    src/xwa/input/dinput.c
    src/xwa/input/forcefeedback.c
    src/xwa/math/math2.c
    src/xwa/math/math3d.c
    src/xwa/math/scalar.c
    src/xwa/math/trig2.c
    src/xwa/movie/movie.c
    src/xwa/render/effects.c
    src/xwa/render/engine_glow.c
    src/xwa/render/lens_flare.c
    src/xwa/render/render_clip.c
    src/xwa/render/render_quad.c
    src/xwa/render/render_scene_alloc.c
    src/xwa/render/render_scene_core.c
    src/xwa/render/render_scene_effects.c
    src/xwa/render/render_scene_faces.c
    src/xwa/render/render_scene_model.c
    src/xwa/render/render_scene_preview.c
    src/xwa/render/renderer.c
    src/xwa/render/renderer_std3d.c
    src/xwa/render/scene_billboard.c
    src/xwa/render/transfm2.c
    src/xwa/xwa_options.c
    src/xwa/util/color.c
    src/xwa/util/debug.c
    src/xwa/util/memory.c
    src/xwa/util/random.c
    src/xwa/util/string.c
    src/xwa/util/time.c
)

set(XWA_RUNTIME_SOURCES
    src/xwa_runtime/config/modern_input_options.c
    src/xwa_runtime/config/modern_controller_options_screen.c
    src/xwa_runtime/config/modern_input_options_screen.c
    src/xwa_runtime/config/modern_options_menu.c
    src/xwa_runtime/config/modern_pilot_profiles_screen.c
    src/xwa_runtime/config/modern_video_options.c
    src/xwa_runtime/config/modern_video_options_screen.c
    src/xwa_runtime/config/modern_vr_options_screen.c
    src/xwa_runtime/config/modern_vr_buttons_screen.c
    src/xwa_runtime/config/modern_vr_credits_screen.c
    src/xwa_runtime/config/modern_mission_hints_screen.c
    src/xwa_runtime/hooks/orientation_hook.c
    src/xwa_runtime/input/input_bridge.c
    src/xwa_runtime/input/controller_mapping.c
    src/xwa_runtime/input/rumble_provider.c
    src/xwa_runtime/input/winmm_joystick_provider.c
    src/xwa_runtime/input/mouse_flight.c
    src/xwa_runtime/net/netplay.c
    src/xwa_runtime/net/netplay_game.c
    src/xwa_runtime/net/netplay_screens.c
    src/xwa_runtime/runtime/flight_pause_task.c
    src/xwa_runtime/runtime/flight_savestate.c
    src/xwa_runtime/runtime/flight_checkpoints.c
    src/xwa_runtime/runtime/flight_task.c
    src/xwa_runtime/runtime/frontend_task.c
    src/xwa_runtime/runtime/movie_task.c
    src/xwa_runtime/runtime/port.c
    src/xwa_runtime/runtime/presentation.c
    src/xwa_runtime/runtime/vr_buttons.c
    src/xwa_runtime/runtime/vr_mapping.c
    src/xwa_runtime/snapshot/snapshot.c
    src/xwa_runtime/snapshot/snapshot_export.c
    src/xwa_runtime/snapshot/snapshot_flight_map.c
    src/xwa_runtime/snapshot/snapshot_hud.c
    src/xwa_runtime/timing/host_clock.c
    src/xwa_runtime/timing/modern_flight_timing.c
)

add_library(xwa_core ${XWA_RECOVERED_SOURCES} ${XWA_RUNTIME_SOURCES})

# The recovered sources reproduce MSVC 5.0-era code, which reinterprets one
# struct pointer type as another (iMUSE passes ImStreamZone*/ImTrack* lists
# through the ImListNode* helpers, for example). That is strict-aliasing UB, and
# GCC acts on it: at -O3 it hoisted the ImStreamZone* list head out of the
# drain loops in implayer.c and emitted six infinite loops (ImDpSwitchStream
# hung the hangar transition on the Windows and Linux release builds). Keeping
# the original shapes means keeping the assumption, so opt the recovered
# translation units out of type-based alias analysis. Port, remaster, app and
# Aeron code is not covered and must stay strict-aliasing clean.
if(NOT MSVC)
    # -ffp-contract=off: no fused multiply-add. Clang fuses by default on
    # ARM64 (the Quest) but x86-64 cannot without FMA flags, and network games
    # need every machine to compute the same flight, so keep plain IEEE math.
    set_source_files_properties(${XWA_RECOVERED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing;-ffp-contract=off")
endif()

# Remaster driver — snapshot -> aeron_scene translation.
# Snapshot data is its semantic render input; narrow modern-adapter controls
# coordinate ownership of classic presentation layers.
add_library(xwa_remaster STATIC
    src/xwa_remaster/xwa_remaster.c
    src/xwa_remaster/cutscene.c
    src/xwa_remaster/frontend.c
    src/xwa_remaster/effects.c
    src/xwa_remaster/flight.c
    src/xwa_remaster/flight_map.c
    src/xwa_remaster/glow_marks.c
    src/xwa_remaster/hyperspace.c
    src/xwa_remaster/hud.c
    src/xwa_remaster/hud_boxes.c
    src/xwa_remaster/hud_cmd.c
    src/xwa_remaster/hud_cmd_math.c
    src/xwa_remaster/hud_fixed.c
    src/xwa_remaster/hud_layout.c
    src/xwa_remaster/hud_text.c
    src/xwa_remaster/preview.c
    src/xwa_remaster/ship.c
    src/xwa_remaster/opt_mesh.c
    src/xwa_remaster/original_2d.c
    src/xwa_remaster/sky_stars.c
    src/xwa_remaster/vr_cockpit_hud.c
    src/xwa_remaster/vr_revival_cockpit.c
    src/xwa_remaster/vr_lap_console.c
    src/xwa_remaster/vr_hud_occlusion.c
    src/xwa_remaster/vr_hand_overlay.c
    src/xwa_remaster/vr_gloves.c
    src/xwa_remaster/vr_controllers.c
    src/xwa_remaster/vr_ktx2_decode.cpp
    src/xwa_remaster/vr_flight_stick.c
    src/xwa_remaster/vr_env.c
    src/xwa_remaster/vr_intro_crawl.c
    src/xwa_remaster/vr_intro_hoth.c
    src/xwa_remaster/vr_menu_beam.c
    src/xwa_remaster/assets.c
    src/xwa_remaster/text.c
)
if(XWA_ENABLE_DEBUG_UI)
    target_sources(xwa_remaster PRIVATE
        src/xwa_remaster/debug_tools.cpp)
endif()
# meshoptimizer's simplifier (MIT): light cut-out meshes of the VR
# controller models.
add_library(xwa_meshopt STATIC
    third_party/meshoptimizer/simplifier.cpp
    third_party/meshoptimizer/allocator.cpp)
target_include_directories(xwa_meshopt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/meshoptimizer)
# Basis Universal's transcoder (Apache 2.0): the KTX2 textures of the
# runtime's controller models (button letters, shading), to RGBA. The
# GPU-format targets the game does not use are switched off.
find_package(zstd REQUIRED CONFIG)
add_library(xwa_basisu STATIC third_party/basisu/transcoder/basisu_transcoder.cpp)
target_include_directories(xwa_basisu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/basisu/transcoder)
target_compile_definitions(xwa_basisu PUBLIC
    BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1
    BASISD_SUPPORT_DXT1=0 BASISD_SUPPORT_DXT5A=0 BASISD_SUPPORT_PVRTC1=0 BASISD_SUPPORT_PVRTC2=0
    BASISD_SUPPORT_ETC2_EAC_A8=0 BASISD_SUPPORT_ETC2_EAC_RG11=0 BASISD_SUPPORT_ATC=0 BASISD_SUPPORT_FXT1=0)
target_link_libraries(xwa_basisu PRIVATE zstd::libzstd)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xwa_basisu PRIVATE -w -O2)
endif()
target_include_directories(xwa_remaster
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE
        # stb_image: the VR controller models' textures; stb_truetype: the
        # VR intro crawl's text.
        ${CMAKE_CURRENT_SOURCE_DIR}/aeron/third_party/stb
)
target_link_libraries(xwa_remaster
    PUBLIC
        aeron
        aeron_scene
        aeron_asset
        xwa_2d_formats
    PRIVATE
        # The runtime's controller models: glTF parsing, KTX2 atlas.
        aeron_cgltf
        imgbake
        xwa_meshopt
        xwa_basisu
)

# OpenXWA-owned scene shaders use Aeron's cross-platform shader pipeline and
# land beside the shared runtime shaders so Aeron_CreateShader can resolve
# them by name on every backend.
aeron_compile_hlsl(XWA_SHADER_OUTPUTS ${CMAKE_BINARY_DIR}/shaders
	${CMAKE_CURRENT_SOURCE_DIR}/shaders/hyperspace_streak.vert.hlsl
	${CMAKE_CURRENT_SOURCE_DIR}/shaders/hyperspace_streak.frag.hlsl
	${CMAKE_CURRENT_SOURCE_DIR}/shaders/hyperspace_tunnel.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hyperspace_tunnel.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hyperspace_environment.comp.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky_stars.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky_stars.frag.hlsl
)
add_custom_target(xwa_shaders DEPENDS ${XWA_SHADER_OUTPUTS})
add_dependencies(xwa_remaster xwa_shaders)
add_dependencies(xwa_core xwa_shaders)

target_compile_definitions(xwa_core
    PUBLIC
        XWA_MODERN
)

target_include_directories(xwa_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(xwa_core
    PUBLIC
        aeron
        aeron_compat
        aeron_video
)

set(XWA_APP_SOURCES
    src/xwa_app/main.c
    src/xwa_app/host_config.c
    src/xwa_app/setup.c
    src/xwa_app/setup_screen.c
    src/xwa_app/vr_keyboard.c
    src/xwa_app/crash_report.c
    src/xwa_app/voice_core.c
    src/xwa_app/vr_voice.c
    src/xwa_app/xr_tier.c
)

if(ANDROID)
    # SDL's Android activity loads the game as libmain.so and calls SDL_main.
    add_library(xwa SHARED ${XWA_APP_SOURCES})
    set_target_properties(xwa PROPERTIES OUTPUT_NAME "main")
    # Vulkan shim between SDL and the system loader: fixed foveated rendering
    # (packaging/quest/vkshim/vkshim.c). Packaged as libopenxwa_vkshim.so.
    add_library(openxwa_vkshim SHARED packaging/quest/vkshim/vkshim.c)
    target_include_directories(openxwa_vkshim PRIVATE third_party/vulkan-headers)
    set_target_properties(openxwa_vkshim PROPERTIES C_STANDARD 99 C_VISIBILITY_PRESET hidden)
    target_link_libraries(openxwa_vkshim PRIVATE dl m)
    add_dependencies(xwa openxwa_vkshim)
else()
    add_executable(xwa ${XWA_APP_SOURCES})
    set_target_properties(xwa PROPERTIES OUTPUT_NAME "OpenXWA")
endif()

target_link_libraries(xwa
    PRIVATE
        xwa_core
        xwa_remaster
)
target_include_directories(xwa PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src/xwa_app
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers) # xr_tier.c's GPU probe
if(UNIX)
    target_link_libraries(xwa PRIVATE ${CMAKE_DL_LIBS})
endif()

set(XWA_AERON_SCENE3D_DEFAULTS
    "${CMAKE_CURRENT_SOURCE_DIR}/aeron/config/scene3d_defaults.yaml")

set(XWA_VERSION "0.0.0-dev" CACHE STRING "OpenXWA application version")
target_compile_definitions(xwa_core PRIVATE
    OPENXWA_VERSION="${XWA_VERSION}"
)
target_compile_definitions(xwa PRIVATE
    OPENXWA_VERSION="${XWA_VERSION}"
)

if(APPLE)
    set(XWA_MACOS_BUILD_VERSION "1" CACHE STRING
        "OpenXWA macOS bundle build version")
    if(NOT DEFINED XWA_MACOS_BUNDLE_IDENTIFIER)
        set(XWA_MACOS_BUNDLE_IDENTIFIER "org.openxwa.openxwa" CACHE STRING
            "OpenXWA macOS bundle identifier")
    elseif(XWA_MACOS_BUNDLE_IDENTIFIER STREQUAL "org.xwa-reimplementation.xwa")
        set(XWA_MACOS_BUNDLE_IDENTIFIER "org.openxwa.openxwa" CACHE STRING
            "OpenXWA macOS bundle identifier" FORCE)
    endif()
    set_property(CACHE XWA_MACOS_BUNDLE_IDENTIFIER PROPERTY HELPSTRING
        "OpenXWA macOS bundle identifier")

    set_target_properties(xwa PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    set(XWA_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/OpenXWA.icns")
    target_sources(xwa PRIVATE "${XWA_MACOS_ICON}")
    set_source_files_properties("${XWA_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

    set(XWA_MACOS_RESOURCE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/remaster/config.yaml"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/remaster/opt_alpha_overrides.yaml")
    target_sources(xwa PRIVATE ${XWA_MACOS_RESOURCE_FILES})
    set_source_files_properties(${XWA_MACOS_RESOURCE_FILES} PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources/resources/remaster)

    target_sources(xwa PRIVATE "${XWA_AERON_SCENE3D_DEFAULTS}")
    set_source_files_properties("${XWA_AERON_SCENE3D_DEFAULTS}" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources/resources/aeron)

    set(XWA_MACOS_SHADER_OUTPUTS ${AERON_SHADER_OUTPUTS} ${XWA_SHADER_OUTPUTS})
    target_sources(xwa PRIVATE ${XWA_MACOS_SHADER_OUTPUTS})
    set_source_files_properties(${XWA_MACOS_SHADER_OUTPUTS} PROPERTIES
        GENERATED TRUE
        MACOSX_PACKAGE_LOCATION Resources/shaders)

    add_custom_command(TARGET xwa POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DXWA_BUNDLE_CONTENT_DIR=$<TARGET_BUNDLE_CONTENT_DIR:xwa>"
            "-DXWA_SHADER_SOURCE_DIR=${CMAKE_BINARY_DIR}/shaders"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/StageBundle.cmake"
        COMMENT "Staging OpenXWA macOS shaders"
        VERBATIM
    )

    install(TARGETS xwa BUNDLE DESTINATION .)

    set(XWA_MACOS_DEPENDENCY_DIRS)
    foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND XWA_MACOS_DEPENDENCY_DIRS "${prefix}/lib")
    endforeach()
    list(JOIN XWA_MACOS_DEPENDENCY_DIRS ";" XWA_MACOS_DEPENDENCY_DIRS)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/FixupBundle.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/FixupMacBundle.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FixupMacBundle.cmake")
else()
    add_custom_target(xwa_aeron_scene3d_defaults
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:xwa>/resources/aeron"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XWA_AERON_SCENE3D_DEFAULTS}"
            "$<TARGET_FILE_DIR:xwa>/resources/aeron/scene3d_defaults.yaml"
        DEPENDS "${XWA_AERON_SCENE3D_DEFAULTS}"
        COMMENT "Staging Aeron Scene3D defaults for OpenXWA"
        VERBATIM
    )
    add_dependencies(xwa xwa_aeron_scene3d_defaults)
    add_custom_command(TARGET xwa POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources
            $<TARGET_FILE_DIR:xwa>/resources
        COMMENT "Staging OpenXWA application resources"
    )
endif()

if(WIN32)
    set_target_properties(xwa PROPERTIES WIN32_EXECUTABLE TRUE)
    # Netplay (src/xwa_runtime/net) uses Winsock.
    target_link_libraries(xwa_core PUBLIC ws2_32)

    # Executable icon and VERSIONINFO resource. FILEVERSION needs numeric
    # components; non-release versions such as 0.0.0-dev fall back to 0.0.0.
    enable_language(RC)
    if(XWA_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(XWA_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(XWA_VERSION_MINOR ${CMAKE_MATCH_2})
        set(XWA_VERSION_PATCH ${CMAKE_MATCH_3})
    else()
        set(XWA_VERSION_MAJOR 0)
        set(XWA_VERSION_MINOR 0)
        set(XWA_VERSION_PATCH 0)
    endif()
    set(XWA_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/openxwa.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/xwa.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/xwa.rc"
        @ONLY)
    target_sources(xwa PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/xwa.rc")
endif()

if(XWA_BUILD_TOOLS)
    add_executable(mission_dump tools/mission_dump.c)
    target_include_directories(mission_dump PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_executable(xwa_asset_bake
        tools/xwa_asset_bake/xwa_asset_bake.c
        tools/xwa_asset_bake/bake_source.c
        tools/xwa_asset_bake/bake_flight_fonts.c
        tools/xwa_asset_bake/bake_fonts.c
        tools/xwa_asset_bake/bake_sprites.c
    )
    target_link_libraries(xwa_asset_bake PRIVATE xwa_2d_formats imgbake)

    add_executable(imc_decode tools/imc_decode.c)
    target_link_libraries(imc_decode PRIVATE xwa_core)

endif()

if(MSVC)
    target_compile_options(xwa_core PRIVATE /W4)
    target_compile_options(xwa PRIVATE /W4)
else()
    target_compile_options(xwa_core PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -fno-common)
    target_compile_options(xwa PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -fno-common)
endif()
if(XWA_BUILD_TOOLS)
    if(MSVC)
        target_compile_options(mission_dump PRIVATE /W4)
        target_compile_options(imc_decode PRIVATE /W4)
    else()
        target_compile_options(mission_dump PRIVATE
            -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -fno-common)
        target_compile_options(imc_decode PRIVATE
            -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -fno-common)
    endif()
endif()

set(XWA_FORMAT_HEADERS
    src/xwa_runtime/config/modern_input_options.h
    src/xwa_runtime/config/modern_controller_options_screen.h
    src/xwa_runtime/config/modern_input_options_screen.h
    src/xwa_runtime/config/modern_options_menu.h
    src/xwa_runtime/config/modern_pilot_profiles_screen.h
    src/xwa_runtime/config/modern_video_options.h
    src/xwa_runtime/config/modern_video_options_screen.h
    src/xwa/audio/cd_audio.h
    src/xwa/audio/music.h
    src/xwa/config/game_config.h
    src/xwa/config/pilot.h
    src/xwa/input/forcefeedback.h
    src/xwa/assets/file_io.h
    src/xwa/assets/flight_model.h
    src/xwa/assets/linez.h
    src/xwa/assets/model_bounds.h
    src/xwa/assets/model_def.h
    src/xwa/assets/model_mesh.h
    src/xwa/assets/model_texture.h
    src/xwa/assets/model_type.h
    src/xwa/assets/object_type.h
    src/xwa/assets/opt_model.h
    src/xwa/assets/ship_list.h
    src/xwa/assets/sprite_resource.h
    src/xwa/assets/sprite_texture.h
    src/xwa/assets/string_table.h
    src/xwa/assets/ui_string.h
    src/xwa/frontend/briefing_room.h
    src/xwa/frontend/briefing_script.h
    src/xwa/frontend/combat_sim_menu.h
    src/xwa/frontend/cutscene.h
    src/xwa/frontend/family_transport_room.h
    src/xwa/frontend/frontend_button.h
    src/xwa/frontend/concourse.h
    src/xwa/frontend/frontend_bootstrap.h
    src/xwa/frontend/frontend_color.h
    src/xwa/frontend/frontend_cursor.h
    src/xwa/frontend/frontend_display.h
    src/xwa/frontend/frontend_dialog.h
    src/xwa/frontend/frontend_draw.h
    src/xwa/frontend/frontend_escape.h
    src/xwa/frontend/frontend_file_list.h
    src/xwa/frontend/frontend_file_stream.h
    src/xwa/frontend/frontend_flight.h
    src/xwa/frontend/frontend_image.h
    src/xwa/frontend/frontend_input.h
    src/xwa/frontend/frontend_medals.h
    src/xwa/frontend/frontend_email.h
    src/xwa/frontend/frontend_mission.h
    src/xwa/frontend/frontend_mission_list.h
    src/xwa/frontend/frontend_mission_session.h
    src/xwa/frontend/frontend_net.h
    src/xwa/frontend/frontend_rect.h
    src/xwa/frontend/frontend_resources.h
    src/xwa/frontend/frontend_scratch.h
    src/xwa/frontend/frontend_screen.h
    src/xwa/frontend/frontend_scrollbar.h
    src/xwa/frontend/frontend_sound.h
    src/xwa/frontend/frontend_text.h
    src/xwa/frontend/frontend_wave_stream.h
    src/xwa/frontend/film_room.h
    src/xwa/frontend/flight_loading.h
    src/xwa/frontend/mission_debrief.h
    src/xwa/flight/ai/pai.h
    src/xwa/flight/ai/ai_internal.h
    src/xwa/flight/ai/paifight.h
    src/xwa/flight/ai/paiman.h
    src/xwa/flight/ai/paiorder.h
    src/xwa/flight/ai/pai_plan.h
    src/xwa/flight/flight.h
    src/xwa/flight/flight_debug.h
    src/xwa/flight/flight_map.h
    src/xwa/flight/flight_sync.h
    src/xwa/flight/object/collision.h
    src/xwa/flight/object/damage.h
    src/xwa/flight/flight_light.h
    src/xwa/flight/flight_text.h
    src/xwa/math/trig2.h
    src/xwa/frontend/mission_setup.h
    src/xwa/frontend/skirmish.h
    src/xwa/frontend/tech_library.h
    src/xwa/movie/movie.h
    src/xwa/flight/player/player.h
    src/xwa/render/renderer.h
    src/xwa/xwa.h
    src/xwa/xwa_options.h
    src/xwa_runtime/runtime/flight_pause_task.h
    src/xwa_runtime/runtime/flight_savestate.h
    src/xwa_runtime/runtime/flight_checkpoints.h
    src/xwa_runtime/runtime/flight_task.h
    src/xwa_runtime/runtime/frontend_task.h
    src/xwa_runtime/timing/host_clock.h
    src/xwa_runtime/input/input_bridge.h
    src/xwa_runtime/input/rumble_provider.h
    src/xwa_runtime/input/winmm_joystick_provider.h
    src/xwa_runtime/input/controller_mapping.h
    src/xwa_runtime/input/mouse_flight.h
    src/xwa_runtime/net/netplay.h
    src/xwa_runtime/net/netplay_game.h
    src/xwa_runtime/net/netplay_screens.h
    src/xwa_runtime/timing/modern_flight_timing.h
    src/xwa_runtime/runtime/movie_task.h
    src/xwa_runtime/hooks/orientation_hook.h
    src/xwa_runtime/runtime/port.h
    src/xwa_runtime/runtime/presentation.h
    src/xwa_runtime/snapshot/snapshot.h
    src/xwa_runtime/snapshot/snapshot_flight_map.h
    src/xwa_runtime/snapshot/snapshot_hud.h
    src/xwa_runtime/compat/middleware_crt.h
    src/xwa_app/host_config.h
    src/xwa_app/setup.h
    src/xwa/util/byte_order.h
    src/xwa/util/color.h
    src/xwa/util/debug.h
    src/xwa/util/memory.h
    src/xwa/util/string.h
    src/xwa/util/time.h
)

set(XWA_FORMAT_FILES
    ${XWA_FORMAT_HEADERS}
    ${XWA_RECOVERED_SOURCES}
    ${XWA_RUNTIME_SOURCES}
    ${XWA_APP_SOURCES}
    ${XWA_TOOL_SOURCES}
)

find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} --Wno-error=unknown -i ${XWA_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Formatting C sources with clang-format"
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --Wno-error=unknown --dry-run --Werror ${XWA_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking C source formatting"
    )
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found; cannot format sources"
    )

    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found; cannot check source formatting"
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()

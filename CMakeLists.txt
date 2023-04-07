cmake_minimum_required(VERSION 3.9.0)

project(ezquake C)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    option(GIT_SUBMODULE "Check submodules during build" ON)
    if(GIT_SUBMODULE)
        message(STATUS "Submodule update")
        execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE GIT_SUBMOD_RESULT)
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update --init --recursive failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
        endif()
    endif()
endif()

macro(add_resources target_var)
  set(generated_c_files "")
  foreach(source_file ${ARGN})
    get_filename_component(source_file_name ${source_file} NAME)
    string(REGEX REPLACE "\\.[^.]*$" "" source_file_name ${source_file_name})
    set(c_file_name ${source_file_name}.c)
    set(generated_file_name ${CMAKE_CURRENT_BINARY_DIR}/${c_file_name})
    add_custom_command(
      OUTPUT ${generated_file_name}
      COMMAND ${CMAKE_SOURCE_DIR}/json2c.sh ${source_file} > ${generated_file_name}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      DEPENDS ${source_file} ${CMAKE_SOURCE_DIR}/json2c.sh
      COMMENT "Generating C file from ${source_file}"
    )
    list(APPEND generated_c_files ${generated_file_name})
  endforeach()
  set(${target_var} ${generated_c_files})
endmacro()

execute_process(
        COMMAND "./version.sh" "--version"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE VERSION
)

execute_process(
        COMMAND "./version.sh" "--revision"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE REVISION
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(EXPAT REQUIRED expat)
pkg_check_modules(FREETYPE2 REQUIRED freetype2)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(LIBJPEG REQUIRED libjpeg)
pkg_check_modules(LIBPCRE REQUIRED libpcre)
pkg_check_modules(MINIZIP REQUIRED minizip)
pkg_check_modules(LIBPNG REQUIRED libpng)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(SPEEX REQUIRED speex speexdsp)
pkg_check_modules(ZLIB REQUIRED zlib)

add_compile_options(
  -Wall
  -Wno-pointer-to-int-cast
  -Wno-int-to-pointer-cast
  -Wno-strict-aliasing
  -Werror=strict-prototypes
  -Werror=old-style-definition
  -g
  -MMD
  #-fsanitize=address
  #-fno-omit-frame-pointer
  #-fPIE
  #-pie
  #-fno-optimize-sibling-calls
  #-O0
  #-fsanitize-memory-track-origins
)

add_link_options(
  #-fsanitize=address
)

add_definitions(
  -DBUILDSTRING="${CMAKE_SYSTEM_NAME}"
  -DCPUSTRING="${CMAKE_SYSTEM_PROCESSOR}"
  -DEZ_FREETYPE_SUPPORT
  -DJSS_CAM
  -DRENDERER_OPTION_CLASSIC_OPENGL
  -DRENDERER_OPTION_MODERN_OPENGL
  -DREVISION="${REVISION}"
  -DUSE_PR2
  -DUSE_SDL2
  -DVERSION="${VERSION}"
  -DWITH_NQPROGS
  -DWITH_ZIP
  -DWITH_ZLIB
  -DWITH_SPEEX
  -DWITH_JPEG
  -DWITH_PNG
)

include_directories(
  src/qwprot/src
  ${SDL2_INCLUDE_DIRS}
  ${CURL_INCLUDE_DIRS}
  ${EXPAT_INCLUDE_DIRS}
  ${SNDFILE_INCLUDE_DIRS}
  ${FREETYPE2_INCLUDE_DIRS}
  ${SPEEX_INCLUDE_DIRS}
  ${JANSSON_INCLUDE_DIRS}
  ${LIBJPEG_INCLUDE_DIRS}
  ${LIBPNG_INCLUDE_DIRS}
  ${LIBPCRE_INCLUDE_DIRS}
  ${MINIZIP_INCLUDE_DIRS}
)

link_directories(
  ${SDL2_LIBRARY_DIRS}
  ${CURL_LIBRARY_DIRS}
  ${EXPAT_LIBRARY_DIRS}
  ${SNDFILE_LIBRARY_DIRS}
  ${FREETYPE2_LIBRARY_DIRS}
  ${SPEEX_LIBRARY_DIRS}
  ${JANSSON_LIBRARY_DIRS}
  ${LIBJPEG_LIBRARY_DIRS}
  ${LIBPNG_LIBRARY_DIRS}
  ${LIBPCRE_LIBRARY_DIRS}
  ${MINIZIP_LIBRARY_DIRS}
)

add_library(common OBJECT
  src/cmd.c
  src/cmodel.c
  src/com_msg.c
  src/common.c
  src/crc.c
  src/cvar.c
  src/fs.c
  src/hash.c
  src/host.c
  src/mathlib.c
  src/md4.c
  src/net.c
  src/net_chan.c
  src/pmove.c
  src/pmovetst.c
  src/q_shared.c
  src/sha3.c
  src/version.c
  src/vfs_doomwad.c
  src/vfs_gzip.c
  src/vfs_mmap.c
  src/vfs_os.c
  src/vfs_pak.c
  src/vfs_tar.c
  src/vfs_tcp.c
  src/vfs_zip.c
  src/zone.c
)

add_library(server OBJECT
  src/pr2_cmds.c
  src/pr2_edict.c
  src/pr2_exec.c
  src/pr2_vm.c
  src/pr_cmds.c
  src/pr_edict.c
  src/pr_exec.c
  src/sv_ccmds.c
  src/sv_demo.c
  src/sv_demo_misc.c
  src/sv_demo_qtv.c
  src/sv_ents.c
  src/sv_init.c
  src/sv_login.c
  src/sv_main.c
  src/sv_master.c
  src/sv_mod_frags.c
  src/sv_move.c
  src/sv_nchan.c
  src/sv_phys.c
  src/sv_save.c
  src/sv_send.c
  src/sv_user.c
  src/sv_world.c
)

add_resources(shaders_modern
  src/glsl/common.glsl
  src/glsl/constants.glsl
  src/glsl/draw_aliasmodel.fragment.glsl
  src/glsl/draw_aliasmodel.vertex.glsl
  src/glsl/draw_sprites.fragment.glsl
  src/glsl/draw_sprites.vertex.glsl
  src/glsl/draw_world.fragment.glsl
  src/glsl/draw_world.vertex.glsl
  src/glsl/fx_world_geometry.fragment.glsl
  src/glsl/fx_world_geometry.vertex.glsl
  src/glsl/hud_draw_circle.fragment.glsl
  src/glsl/hud_draw_circle.vertex.glsl
  src/glsl/hud_draw_image.fragment.glsl
  src/glsl/hud_draw_image.geometry.glsl
  src/glsl/hud_draw_image.vertex.glsl
  src/glsl/hud_draw_line.fragment.glsl
  src/glsl/hud_draw_line.vertex.glsl
  src/glsl/hud_draw_polygon.fragment.glsl
  src/glsl/hud_draw_polygon.vertex.glsl
  src/glsl/lighting.compute.glsl
  src/glsl/lighting_copy.compute.glsl
  src/glsl/post_process_screen.fragment.glsl
  src/glsl/post_process_screen.vertex.glsl
  src/glsl/simple.fragment.glsl
  src/glsl/simple.vertex.glsl
  src/glsl/simple3d.fragment.glsl
  src/glsl/simple3d.vertex.glsl
)

add_library(modern_opengl OBJECT
  ${shaders_modern}
  src/glm_aliasmodel.c
  src/glm_brushmodel.c
  src/glm_draw.c
  src/glm_framebuffer.c
  src/glm_lightmaps.c
  src/glm_main.c
  src/glm_md3.c
  src/glm_misc.c
  src/glm_particles.c
  src/glm_performance.c
  src/glm_rmain.c
  src/glm_rsurf.c
  src/glm_sdl.c
  src/glm_sprite.c
  src/glm_sprite3d.c
  src/glm_state.c
  src/glm_texture_arrays.c
  src/glm_vao.c
)

add_resources(shaders_classic
  src/glsl/glc/glc_aliasmodel_shadow.fragment.glsl
  src/glsl/glc/glc_aliasmodel_shadow.vertex.glsl
  src/glsl/glc/glc_aliasmodel_shell.fragment.glsl
  src/glsl/glc/glc_aliasmodel_shell.vertex.glsl
  src/glsl/glc/glc_aliasmodel_std.fragment.glsl
  src/glsl/glc/glc_aliasmodel_std.vertex.glsl
  src/glsl/glc/glc_caustics.fragment.glsl
  src/glsl/glc/glc_caustics.vertex.glsl
  src/glsl/glc/glc_draw_sprites.fragment.glsl
  src/glsl/glc/glc_draw_sprites.vertex.glsl
  src/glsl/glc/glc_hud_images.fragment.glsl
  src/glsl/glc/glc_hud_images.vertex.glsl
  src/glsl/glc/glc_post_process_screen.fragment.glsl
  src/glsl/glc/glc_post_process_screen.vertex.glsl
  src/glsl/glc/glc_sky.fragment.glsl
  src/glsl/glc/glc_sky.vertex.glsl
  src/glsl/glc/glc_turbsurface.fragment.glsl
  src/glsl/glc/glc_turbsurface.vertex.glsl
  src/glsl/glc/glc_world_drawflat.fragment.glsl
  src/glsl/glc/glc_world_drawflat.vertex.glsl
  src/glsl/glc/glc_world_secondpass.fragment.glsl
  src/glsl/glc/glc_world_secondpass.vertex.glsl
  src/glsl/glc/glc_world_textured.fragment.glsl
  src/glsl/glc/glc_world_textured.vertex.glsl
)

add_library(classic_opengl OBJECT
  ${shaders_classic}
  src/glc_aliasmodel.c
  src/glc_aliasmodel_mesh.c
  src/glc_bloom.c
  src/glc_brushmodel.c
  src/glc_draw.c
  src/glc_framebuffer.c
  src/glc_lightmaps.c
  src/glc_main.c
  src/glc_matrix.c
  src/glc_md3.c
  src/glc_misc.c
  src/glc_particles.c
  src/glc_performance.c
  src/glc_sdl.c
  src/glc_sky.c
  src/glc_sprite3d.c
  src/glc_state.c
  src/glc_surf.c
  src/glc_turb_surface.c
  src/glc_vao.c
  src/glc_warp.c
)

add_library(common_opengl OBJECT
  src/gl_aliasmodel.c
  src/gl_aliasmodel_md3.c
  src/gl_buffers.c
  src/gl_debug.c
  src/gl_drawcall_wrappers.c
  src/gl_framebuffer.c
  src/gl_misc.c
  src/gl_program.c
  src/gl_sdl.c
  src/gl_sprite3d.c
  src/gl_state.c
  src/gl_texture.c
  src/gl_texture_functions.c
  src/vid_common_gl.c
)

add_library(common_renderer OBJECT
  src/r_aliasmodel.c
  src/r_aliasmodel_md3.c
  src/r_aliasmodel_mesh.c
  src/r_aliasmodel_skins.c
  src/r_atlas.c
  src/r_bloom.c
  src/r_brushmodel.c
  src/r_brushmodel_bspx.c
  src/r_brushmodel_load.c
  src/r_brushmodel_sky.c
  src/r_brushmodel_surfaces.c
  src/r_brushmodel_textures.c
  src/r_brushmodel_warpsurfaces.c
  src/r_buffers.c
  src/r_chaticons.c
  src/r_draw.c
  src/r_draw_charset.c
  src/r_draw_circle.c
  src/r_draw_image.c
  src/r_draw_line.c
  src/r_draw_polygon.c
  src/r_hud.c
  src/r_lightmaps.c
  src/r_main.c
  src/r_matrix.c
  src/r_misc.c
  src/r_model.c
  src/r_netgraph.c
  src/r_palette.c
  src/r_part.c
  src/r_part_trails.c
  src/r_particles_qmb.c
  src/r_particles_qmb_spawn.c
  src/r_particles_qmb_trails.c
  src/r_performance.c
  src/r_refrag.c
  src/r_rlight.c
  src/r_rmain.c
  src/r_rmisc.c
  src/r_sprite3d.c
  src/r_sprites.c
  src/r_states.c
  src/r_texture.c
  src/r_texture_cvars.c
  src/r_texture_load.c
  src/r_texture_util.c
  src/vx_camera.c
  src/vx_coronas.c
  src/vx_stuff.c
  src/vx_vertexlights.c
)

add_library(common_hud OBJECT
  src/hud.c
  src/hud_262.c
  src/hud_ammo.c
  src/hud_armor.c
  src/hud_autoid.c
  src/hud_centerprint.c
  src/hud_clock.c
  src/hud_common.c
  src/hud_editor.c
  src/hud_face.c
  src/hud_frags.c
  src/hud_gamesummary.c
  src/hud_groups.c
  src/hud_guns.c
  src/hud_health.c
  src/hud_items.c
  src/hud_net.c
  src/hud_performance.c
  src/hud_qtv.c
  src/hud_radar.c
  src/hud_scores.c
  src/hud_speed.c
  src/hud_teaminfo.c
  src/hud_tracking.c
  src/hud_weapon_stats.c
)

add_resources(documentation
  help_cmdline_params.json
  help_commands.json
  help_macros.json
  help_variables.json
)

add_library(other OBJECT
  ${documentation}
  src/Ctrl.c
  src/Ctrl_EditBox.c
  src/Ctrl_PageViewer.c
  src/Ctrl_ScrollBar.c
  src/Ctrl_Tab.c
  src/EX_FileList.c
  src/EX_browser.c
  src/EX_browser_net.c
  src/EX_browser_pathfind.c
  src/EX_browser_ping.c
  src/EX_browser_qtvlist.c
  src/EX_browser_sources.c
  src/EX_qtvlist.c
  src/cd_null.c
  src/cl_cam.c
  src/cl_cmd.c
  src/cl_demo.c
  src/cl_ents.c
  src/cl_input.c
  src/cl_main.c
  src/cl_multiview.c
  src/cl_nqdemo.c
  src/cl_parse.c
  src/cl_pred.c
  src/cl_screen.c
  src/cl_screenshot.c
  src/cl_skygroups.c
  src/cl_slist.c
  src/cl_tent.c
  src/cl_view.c
  src/collision.c
  src/common_draw.c
  src/config_manager.c
  src/console.c
  src/demo_controls.c
  src/document_rendering.c
  src/ez_button.c
  src/ez_controls.c
  src/ez_label.c
  src/ez_scrollbar.c
  src/ez_scrollpane.c
  src/ez_slider.c
  src/ez_window.c
  src/fchecks.c
  src/fmod.c
  src/fonts.c
  src/fragstats.c
  src/help.c
  src/help_files.c
  src/ignore.c
  src/image.c
  src/in_sdl2.c
  src/irc.c
  src/irc_filter.c
  src/keys.c
  src/logging.c
  src/match_tools.c
  src/match_tools_challenge.c
  src/menu.c
  src/menu_demo.c
  src/menu_ingame.c
  src/menu_multiplayer.c
  src/menu_options.c
  src/menu_proxy.c
  src/movie.c
  src/mvd_autotrack.c
  src/mvd_utils.c
  src/mvd_xmlstats.c
  src/parser.c
  src/qtv.c
  src/rulesets.c
  src/sbar.c
  src/settings_page.c
  src/sha1.c
  src/skin.c
  src/snd_main.c
  src/snd_mem.c
  src/snd_mix.c
  src/snd_voip.c
  src/stats_grid.c
  src/sys_sdl2.c
  src/teamplay.c
  src/teamplay_locfiles.c
  src/textencoding.c
  src/tp_msgs.c
  src/tp_triggers.c
  src/utils.c
  src/vid_sdl2.c
  src/vid_vsync.c
  src/vx_tracker.c
  src/wad.c
  src/xsd.c
  src/xsd_document.c
)

add_library(sys_linux OBJECT
  src/localtime_posix.c
  src/sys_posix.c
  src/linux_signals.c
)

add_library(central OBJECT
  src/central.c
)

add_executable(ezquake
  $<TARGET_OBJECTS:central>
  $<TARGET_OBJECTS:classic_opengl>
  $<TARGET_OBJECTS:common>
  $<TARGET_OBJECTS:common_hud>
  $<TARGET_OBJECTS:common_opengl>
  $<TARGET_OBJECTS:common_renderer>
  $<TARGET_OBJECTS:modern_opengl>
  $<TARGET_OBJECTS:other>
  $<TARGET_OBJECTS:server>
  $<TARGET_OBJECTS:sys_linux>
)

target_link_libraries(ezquake
  ${CURL_LIBRARIES}
  ${EXPAT_LIBRARIES}
  ${FREETYPE2_LIBRARIES}
  ${JANSSON_LIBRARIES}
  ${LIBJPEG_LIBRARIES}
  ${LIBPCRE_LIBRARIES}
  ${LIBPNG_LIBRARIES}
  ${MINIZIP_LIBRARIES}
  ${SDL2_LIBRARIES}
  ${SNDFILE_LIBRARIES}
  ${SPEEX_LIBRARIES}
  ${ZLIB_LIBRARIES}
  m
  GL
  pthread
)
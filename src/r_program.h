
#ifndef EZQUAKE_R_PROGRAM_HEADER
#define EZQUAKE_R_PROGRAM_HEADER

typedef enum {
	r_program_none,

	r_program_aliasmodel,
	r_program_brushmodel,
	r_program_sprite3d,
	r_program_hud_images,
	r_program_hud_circles,
	r_program_post_process,

	r_program_post_process_glc,
	r_program_sky_glc,
	r_program_turb_glc,
	r_program_caustics_glc,
	r_program_aliasmodel_std_glc,
	r_program_aliasmodel_shell_glc,
	r_program_aliasmodel_shadow_glc,
	r_program_aliasmodel_outline_glc,
	r_program_world_drawflat_glc,
	r_program_world_textured_glc,
	r_program_world_secondpass_glc,
	r_program_sprites_glc,
	r_program_hud_images_glc,

	r_program_lightmap_compute,

	r_program_fx_world_geometry,
	r_program_brushmodel_alphatested,
	r_program_simple,
	r_program_simple3d,

	r_program_count
} r_program_id;

typedef enum {
	r_program_uniform_aliasmodel_drawmode,
	r_program_uniform_brushmodel_outlines,
	r_program_uniform_brushmodel_sampler,
	r_program_uniform_sprite3d_alpha_test,
	r_program_uniform_hud_circle_matrix,
	r_program_uniform_hud_circle_color,
	r_program_uniform_post_process_glc_gamma,
	r_program_uniform_post_process_glc_base,
	r_program_uniform_post_process_glc_overlay,
	r_program_uniform_post_process_glc_v_blend,
	r_program_uniform_post_process_glc_contrast,
	r_program_uniform_sky_glc_cameraPosition,
	r_program_uniform_sky_glc_speedscale,
	r_program_uniform_sky_glc_speedscale2,
	r_program_uniform_sky_glc_skyTex,
	r_program_uniform_sky_glc_skyDomeTex,
	r_program_uniform_sky_glc_skyDomeCloudTex,
	r_program_uniform_turb_glc_texSampler,
	r_program_uniform_turb_glc_time,
	r_program_uniform_caustics_glc_texSampler,
	r_program_uniform_caustics_glc_time,
	r_program_uniform_aliasmodel_std_glc_angleVector,
	r_program_uniform_aliasmodel_std_glc_shadelight,
	r_program_uniform_aliasmodel_std_glc_ambientlight,
	r_program_uniform_aliasmodel_std_glc_texSampler,
	r_program_uniform_aliasmodel_std_glc_fsTextureEnabled,
	r_program_uniform_aliasmodel_std_glc_fsMinLumaMix,
	r_program_uniform_aliasmodel_std_glc_time,
	r_program_uniform_aliasmodel_std_glc_fsCausticEffects,
	r_program_uniform_aliasmodel_std_glc_lerpFraction,
	r_program_uniform_aliasmodel_std_glc_causticsSampler,
	r_program_uniform_aliasmodel_shell_glc_fsBaseColor1,
	r_program_uniform_aliasmodel_shell_glc_fsBaseColor2,
	r_program_uniform_aliasmodel_shell_glc_texSampler,
	r_program_uniform_aliasmodel_shell_glc_lerpFraction,
	r_program_uniform_aliasmodel_shell_glc_scroll,
	r_program_uniform_aliasmodel_shadow_glc_lerpFraction,
	r_program_uniform_aliasmodel_shadow_glc_shadevector,
	r_program_uniform_aliasmodel_shadow_glc_lheight,
	r_program_uniform_world_drawflat_glc_wallcolor,
	r_program_uniform_world_drawflat_glc_floorcolor,
	r_program_uniform_world_drawflat_glc_skycolor,
	r_program_uniform_world_drawflat_glc_watercolor,
	r_program_uniform_world_drawflat_glc_slimecolor,
	r_program_uniform_world_drawflat_glc_lavacolor,
	r_program_uniform_world_drawflat_glc_telecolor,
	r_program_uniform_world_textured_glc_lightmapSampler,
	r_program_uniform_world_textured_glc_texSampler,
	r_program_uniform_world_textured_glc_lumaSampler,
	r_program_uniform_world_textured_glc_causticSampler,
	r_program_uniform_world_textured_glc_detailSampler,
	r_program_uniform_world_textured_glc_time,
	r_program_uniform_world_textured_glc_lumaScale,
	r_program_uniform_world_textured_glc_fbScale,
	r_program_uniform_world_textured_glc_r_floorcolor,
	r_program_uniform_world_textured_glc_r_wallcolor,
	r_program_uniform_sprites_glc_materialSampler,
	r_program_uniform_sprites_glc_alphaThreshold,
	r_program_uniform_hud_images_glc_primarySampler,
	r_program_uniform_hud_images_glc_secondarySampler,
	r_program_uniform_aliasmodel_outline_glc_lerpFraction,
	r_program_uniform_aliasmodel_outline_glc_outlineScale,
	r_program_uniform_brushmodel_alphatested_outlines,
	r_program_uniform_brushmodel_alphatested_sampler,
	r_program_uniform_turb_glc_alpha,
	r_program_uniform_turb_glc_color,
	r_program_uniform_simple_color,
	r_program_uniform_world_textures_glc_texture_multiplier,
	r_program_uniform_simple3d_color,
	r_program_uniform_lighting_firstLightmap,
	r_program_uniform_sky_glc_fog_skyFogMix,
    r_program_uniform_outline_color,
    r_program_uniform_outline_accuracy,
    r_program_uniform_aliasmodel_outline_color,
	r_program_uniform_count
} r_program_uniform_id;

typedef enum {
	r_program_attribute_aliasmodel_std_glc_flags,
	r_program_attribute_aliasmodel_shell_glc_flags,
	r_program_attribute_aliasmodel_shadow_glc_flags,
	r_program_attribute_aliasmodel_outline_glc_flags,
	r_program_attribute_world_drawflat_style,
	r_program_attribute_world_textured_style,
	r_program_attribute_world_textured_detailCoord,

	r_program_attribute_count
} r_program_attribute_id;

typedef enum {
	r_program_memory_barrier_image_access,
	r_program_memory_barrier_texture_access,
	r_program_memory_barrier_count
} r_program_memory_barrier_id;

void R_ProgramInitialiseState(void);
int R_ProgramCustomOptions(r_program_id program_id);
qbool R_ProgramReady(r_program_id program_id);
void R_ProgramUse(r_program_id program_id);
r_program_id R_ProgramInUse(void);
r_program_id R_ProgramForAttribute(r_program_attribute_id attr_id);
int R_ProgramCustomOptions(r_program_id program_id);
void R_ProgramSetCustomOptions(r_program_id program_id, int options);

void R_ProgramComputeDispatch(r_program_id program_id, unsigned int num_groups_x, unsigned int num_groups_y, unsigned int num_groups_z);
void R_ProgramMemoryBarrier(r_program_id program_id);
void R_ProgramComputeSetMemoryBarrierFlag(r_program_id program_id, r_program_memory_barrier_id barrier_id);

void R_ProgramUniform1i(r_program_uniform_id uniform_id, int value);
void R_ProgramUniform1f(r_program_uniform_id uniform_id, float value);
void R_ProgramUniform4fv(r_program_uniform_id uniform_id, const float* values);
void R_ProgramUniform3f(r_program_uniform_id uniform_id, float x, float y, float z);
void R_ProgramUniform3fv(r_program_uniform_id uniform_id, const float* values);
void R_ProgramUniform2fv(r_program_uniform_id uniform_id, const float* values);
void R_ProgramUniform3fNormalize(r_program_uniform_id uniform_id, const byte* values);
void R_ProgramUniformMatrix4fv(r_program_uniform_id uniform_id, const float* values);
int R_ProgramUniformGet1i(r_program_uniform_id uniform_id, int default_value);

int R_ProgramAttributeLocation(r_program_attribute_id attr_id);

// Check if a program needs to be recompiled
qbool R_ProgramRecompileNeeded(r_program_id program_id, unsigned int options);

// Compiles a simple program
qbool R_ProgramCompile(r_program_id program_id);

// Compiles a program with custom includes based on user's config
qbool R_ProgramCompileWithInclude(r_program_id program_id, const char* included_definitions);

// Asks all programs to compile themselves
void R_ProgramCompileAll(void);

// Switches between sub-programs (allows multiple copies of the same program with different flags)
void R_ProgramSetSubProgram(r_program_id program_id, int sub_index);

// Sets uniforms based on program flags (used for fog at the moment)
void R_ProgramSetStandardUniforms(r_program_id program_id);

#endif // EZQUAKE_R_PROGRAM_HEADER

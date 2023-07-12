/*
Copyright (C) 2018 ezQuake team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// vk_main.c
// - Main entry point for Vulkan

#ifdef RENDERER_OPTION_VULKAN

#include <vulkan/vulkan.h>

#include <SDL.h>
#include <SDL_vulkan.h>

#include "vk_local.h"
#include "vk_texture.h"
#include "r_renderer.h"
#include "r_brushmodel.h"

vk_options_t vk_options;

static void VK_Begin2DRendering(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_Screenshot(byte *b, size_t s)
{
	VK_NOT_IMPLEMENTED;
}

static size_t VK_ScreenshotWidth(void)
{
	VK_NOT_IMPLEMENTED;
	return 0;
}

static size_t VK_ScreenshotHeight(void)
{
	VK_NOT_IMPLEMENTED;
	return 0;
}

static void VK_CvarForceRecompile(cvar_t *cvar)
{
	VK_NOT_IMPLEMENTED;
}

static const char* VK_DescriptiveString(void)
{
	VK_NOT_IMPLEMENTED;
	return NULL;
}

static void VK_Viewport(int x, int y, int width, int height)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_PrepareAliasModel(model_t *m, aliashdr_t *hdr)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_ClearRenderingSurface(qbool clear_color)
{
	VK_NOT_IMPLEMENTED;
}

static qbool VK_IsFramebufferEnabled3D(void)
{
	VK_NOT_IMPLEMENTED;
	return false;
}


static qbool VK_FramebufferCreate(framebuffer_id id, int width, int height)
{
	VK_NOT_IMPLEMENTED;
	return false;
}

static void VK_ProgramsShutdown(qbool restarting)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_ProgramsInitialise(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_PrepareModelRendering(qbool vid_restart)
{
	if (!cl.worldmodel)
		return;

	R_BrushModelMeasureVBOSize(cl.worldmodel);

	VK_NOT_IMPLEMENTED;
}

typedef struct vk_vertex_s {
	float position[3];
	float texCoord[2];
} vk_vertex_t;

static void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer, VkDeviceMemory* bufferMemory) {
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(vk_options.logicalDevice, &bufferInfo, NULL, buffer) != VK_SUCCESS) {
		printf("failed to create buffer!\n");
		return;
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(vk_options.logicalDevice, *buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType(memRequirements.memoryTypeBits, properties);

	if (vkAllocateMemory(vk_options.logicalDevice, &allocInfo, NULL, bufferMemory) != VK_SUCCESS) {
		printf("failed to allocate buffer memory!\n");
		return;
	}

	vkBindBufferMemory(vk_options.logicalDevice, *buffer, *bufferMemory, 0);
}

void VK_CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer commandBuffer = VK_BeginSingleTimeCommands();

	VkBufferCopy copyRegion = {};
	copyRegion.srcOffset = 0; // Optional
	copyRegion.dstOffset = 0; // Optional
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	VK_EndSingleTimeCommands(commandBuffer);
}

static void VK_CreateVertexBuffer(VkBuffer* vertexBuffer, VkDeviceMemory* vertexBufferMemory, vk_vertex_t* vertices, size_t numverts) {
	VkDeviceSize bufferSize = sizeof(vertices[0]) * numverts;
	VkBuffer stagingBuffer;
	VkDeviceMemory stagingBufferMemory;
	VK_CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingBuffer, &stagingBufferMemory);

	void* data;
	vkMapMemory(vk_options.logicalDevice, stagingBufferMemory, 0, bufferSize, 0, &data);
	memcpy(data, vertices, (size_t)bufferSize);
	vkUnmapMemory(vk_options.logicalDevice, stagingBufferMemory);

	VK_CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertexBuffer, vertexBufferMemory);

	VK_CopyBuffer(stagingBuffer, *vertexBuffer, bufferSize);

	vkDestroyBuffer(vk_options.logicalDevice, stagingBuffer, NULL);
	vkFreeMemory(vk_options.logicalDevice, stagingBufferMemory, NULL);
}

static void VK_DrawWorld(void)
{
}

static void VK_DrawAliasFrame(entity_t* ent, model_t* model, int pose1, int pose2,
							  texture_ref texture, texture_ref fb_texture,
							  qbool outline, int effects, int render_effects, float lerpfrac)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_DrawSpriteModel(entity_t* entity)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_DrawSimpleItem(model_t* m, int skin, vec3_t origin, float scale_, vec3_t up, vec3_t right)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_DrawClassicParticles(int particles_to_draw)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_LightmapFrameInit(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_RenderDynamicLightmaps(msurface_t* s, qbool world)
{
	// gigaspaaam
	// VK_NOT_IMPLEMENTED;
}

static void VK_InvalidateLightmapTextures(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_LightmapShutdown(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_SetupGL(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_ChainBrushModelSurfaces(model_t* clmodel, entity_t* ent)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_DrawBrushModel(entity_t* ent, qbool polygonOffset, qbool caustics)
{
	VK_NOT_IMPLEMENTED;
}

static int VK_BrushModelCopyVertToBuffer(model_t* mod, void* vbo_buffer_, int position, float* source,
										 int lightmap, int material, float scaleS, float scaleT,
										 msurface_t* surf, qbool has_fb_texture, qbool has_luma_texture)
{
	VK_NOT_IMPLEMENTED;
	return 0;
}

static void VK_DrawWaterSurfaces(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_ScreenDrawStart(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_RenderView(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_PreRenderView(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_PostProcessScreen(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_BrightenScreen(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_PolyBlend(float v_blend[4])
{
	VK_NOT_IMPLEMENTED;
}

static void VK_TimeRefresh(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_BindVertexArrayElementBuffer(r_vao_id vao, r_buffer_id ref)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_Prepare3DSprites(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_Draw3DSprites(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_RenderFramebuffers(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_ApplyRenderingState(r_state_id id)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_DrawSky(void)
{
	//VK_NOT_IMPLEMENTED;
}

static void VK_DrawAliasModelShadow(entity_t *ent)
{
	//VK_NOT_IMPLEMENTED;
}

static void VK_DrawAliasModelPowerupShell(entity_t *ent)
{
	//VK_NOT_IMPLEMENTED;
}

static void VK_DrawAlias3ModelPowerupShell(entity_t *ent)
{
	//VK_NOT_IMPLEMENTED;
}

static void VK_DrawDisc(void)
{
	//VK_NOT_IMPLEMENTED;
}

static void VK_EnsureFinished(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_TextureInitialiseState(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_Draw3DSpritesInline(void)
{
	VK_NOT_IMPLEMENTED;
}

static void VK_InvalidateViewport(void)
{
	VK_NOT_IMPLEMENTED;
}


#define RENDERER_METHOD(returntype, name, ...) \
{ \
	extern returntype VK_ ## name(__VA_ARGS__); \
	renderer.name = VK_ ## name; \
}

VkCommandPool vk_command_pool; // global or part of your struct

static qbool VK_CreateCommandPool(void) {
	VkCommandPoolCreateInfo poolInfo = {0};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = vk_options.physicalDeviceGraphicsQueueFamilyIndex;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

	if (vkCreateCommandPool(vk_options.logicalDevice, &poolInfo, NULL, &vk_command_pool) != VK_SUCCESS) {
		Con_Printf("Failed to create command pool!\n");
		return false;
	}
	return true;
}

qbool VK_Initialise(SDL_Window* window)
{
	memset(&vk_options, 0, sizeof(vk_options));

	if (!VK_CreateInstance(window, &vk_options.instance)) {
		return false;
	}

	if (!VK_CreateWindowSurface(window, vk_options.instance, &vk_options.surface)) {
		VK_Shutdown(r_shutdown_full);
		return false;
	}

	if (!VK_SelectPhysicalDevice(vk_options.instance, vk_options.surface)) {
		VK_Shutdown(r_shutdown_full);
		return false;
	}

	if (!VK_CreateLogicalDevice(vk_options.instance)) {
		VK_Shutdown(r_shutdown_full);
		return false;
	}

	if (!VK_CreateCommandPool()) {
		VK_Shutdown(r_shutdown_full);
		return false;
	}

	if (!VK_CreateSwapChain(window, vk_options.instance, vk_options.surface)) {
		VK_Shutdown(r_shutdown_full);
		return false;
	}

	VK_InitialiseBufferHandling(&buffers);

	Con_Printf("Vulkan initialised successfully\n");


#include "r_renderer_structure.h"

	return true;
}

void VK_Shutdown(r_shutdown_mode_t mode)
{
	if (mode != r_shutdown_reload) {
		VK_DestroySwapChain();

		VK_RenderPassDelete(vk_renderpass_none);

		if (vk_options.instance) {
			VK_DestroyWindowSurface(vk_options.instance, vk_options.surface);
			VK_ShutdownDebugCallback(vk_options.instance);
			vkDestroyInstance(vk_options.instance, NULL);
		}

		memset(&vk_options, 0, sizeof(vk_options));
	}

	// FIXME
}

void VK_PopulateConfig(void)
{
}

#endif

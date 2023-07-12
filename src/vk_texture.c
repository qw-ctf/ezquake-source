/*
Copyright (C) 2023 ezQuake team

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

#include "vk_local.h"
#include "vk_texture.h"

VkCommandBuffer VK_BeginSingleTimeCommands(void) {
	VkCommandBufferAllocateInfo allocInfo = {0};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = vk_command_pool;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer;
	vkAllocateCommandBuffers(vk_options.logicalDevice, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo = {0};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	return commandBuffer;
}

void VK_EndSingleTimeCommands(VkCommandBuffer commandBuffer) {
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo = {0};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkQueueSubmit(vk_options.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(vk_options.graphicsQueue);

	vkFreeCommandBuffers(vk_options.logicalDevice, vk_command_pool, 1, &commandBuffer);
}

uint32_t VK_FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(vk_options.physicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}

	assert(!"Failed to find suitable memory type!");

	return 0;
}

static void VK_ImageBarrier(VkImage image)
{
	VkCommandBuffer commandBuffer = VK_BeginSingleTimeCommands();

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	barrier.srcAccessMask = 0; // TODO
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

	vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, NULL,
			0, NULL,
			1, &barrier
	);

	VK_EndSingleTimeCommands(commandBuffer);
}

static void VK_CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
	VkCommandBuffer commandBuffer = VK_BeginSingleTimeCommands();

	VkBufferImageCopy region = {0};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;

	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;

	region.imageOffset = (VkOffset3D){0, 0, 0};
	region.imageExtent = (VkExtent3D){width, height, 1};

	vkCmdCopyBufferToImage(
			commandBuffer,
			buffer,
			image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&region
	);

	VK_EndSingleTimeCommands(commandBuffer);
}

void VK_CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImage* image, VkDeviceMemory* imageMemory)
{
	VkImageCreateInfo imageInfo = {0};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateImage(vk_options.logicalDevice, &imageInfo, NULL, image) != VK_SUCCESS) {
		assert(!"Failed to create image!");
	}

	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(vk_options.logicalDevice, *image, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {0};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	if (vkAllocateMemory(vk_options.logicalDevice, &allocInfo, NULL, imageMemory) != VK_SUCCESS) {
		assert(!"Failed to allocate image memory!");
	}

	vkBindImageMemory(vk_options.logicalDevice, *image, *imageMemory, 0);

	VK_ImageBarrier(*image);
}

void VK_UploadTexture(
		VkImage image,
		VkDeviceMemory textureImageMemory,
		uint32_t width,
		uint32_t height,
		const void* data)
{
	// Create staging buffer
	VkBufferCreateInfo bufferInfo = {0};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = width * height * 4; // assuming 4 bytes per pixel
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer stagingBuffer;
	if (vkCreateBuffer(vk_options.logicalDevice, &bufferInfo, NULL, &stagingBuffer) != VK_SUCCESS) {
		Con_Printf("Could not create staging buffer for texture\n");
		return;
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(vk_options.logicalDevice, stagingBuffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {0};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType(memRequirements.memoryTypeBits,
												  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
												  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VkDeviceMemory stagingBufferMemory;
	if (vkAllocateMemory(vk_options.logicalDevice, &allocInfo, NULL, &stagingBufferMemory) != VK_SUCCESS) {
		// handle error
	}

	vkBindBufferMemory(vk_options.logicalDevice, stagingBuffer, stagingBufferMemory, 0);

	// Copy the texture data into the staging buffer
	void* mappedData;
	vkMapMemory(vk_options.logicalDevice, stagingBufferMemory, 0, bufferInfo.size, 0, &mappedData);
	memcpy(mappedData, data, (size_t)bufferInfo.size);
	vkUnmapMemory(vk_options.logicalDevice, stagingBufferMemory);

	// Now we can copy the staging buffer into the texture image
	VK_CopyBufferToImage(stagingBuffer, image, width, height);

	// Don't forget to clean up after yourself
	vkDestroyBuffer(vk_options.logicalDevice, stagingBuffer, NULL);
	vkFreeMemory(vk_options.logicalDevice, stagingBufferMemory, NULL);
}

void VK_AllocateTextureNames(gltexture_t* glt)
{
	VK_NOT_IMPLEMENTED;
}

void VK_AllocateStorage(gltexture_t* texture)
{
	VK_NOT_IMPLEMENTED;
}

qbool VK_TextureAllocateArrayStorage(gltexture_t* slot)
{
	VK_NOT_IMPLEMENTED;
	return false;
}

void VK_TextureDelete(texture_ref ref)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureMipmapGenerate(texture_ref ref)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureWrapModeClamp(texture_ref ref)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureLabelSet(texture_ref ref, const char *label)
{
	VK_NOT_IMPLEMENTED;
}

qbool VK_TextureUnitBind(int unit, texture_ref ref)
{
	VK_NOT_IMPLEMENTED;
	return false;
}

qbool VK_TextureIsUnitBound(int uint, texture_ref ref)
{
	VK_NOT_IMPLEMENTED;
	return false;
}

void VK_TextureUnitMultiBind(int first, int count, texture_ref* textures)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureGet(texture_ref texture, int buffer_size, byte* buffer, int bpp)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureCompressionSet(qbool enabled)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureCreate2D(texture_ref* texture, int width, int height, const char* name, qbool is_lightmap)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TexturesCreate(r_texture_type_id type, int n, texture_ref* textures)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureReplaceSubImageRGBA(texture_ref ref, int offsetx, int offsety, int width, int height, byte* buffer)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureSetFiltering(texture_ref texture, texture_minification_id minification_filter, texture_magnification_id magnification_filter)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureSetAnisotropy(texture_ref ref, int anisotropy)
{
	VK_NOT_IMPLEMENTED;
}

void VK_TextureLoadCubemapFace(texture_ref cubemap, r_cubemap_direction_id direction, const byte* data, int width, int height)
{
	VK_NOT_IMPLEMENTED;
}

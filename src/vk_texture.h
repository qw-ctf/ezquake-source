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

#include "quakedef.h"
#include "r_texture_internal.h"
#include "r_renderer.h"

void VK_UploadTexture(VkImage image, VkDeviceMemory textureImageMemory, uint32_t width, uint32_t height, const void* data);
void VK_CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImage* image, VkDeviceMemory* imageMemory);
void VK_AllocateTextureNames(gltexture_t* glt);
void VK_AllocateStorage(gltexture_t* texture);
qbool VK_TextureAllocateArrayStorage(gltexture_t* slot);
void VK_TextureDelete(texture_ref ref);
void VK_TextureMipmapGenerate(texture_ref ref);
void VK_TextureWrapModeClamp(texture_ref ref);
void VK_TextureLabelSet(texture_ref ref, const char *label);
qbool VK_TextureUnitBind(int unit, texture_ref ref);
qbool VK_TextureIsUnitBound(int uint, texture_ref ref);
void VK_TextureUnitMultiBind(int first, int count, texture_ref* textures);
void VK_TextureGet(texture_ref texture, int buffer_size, byte* buffer, int bpp);
void VK_TextureCompressionSet(qbool enabled);
void VK_TextureCreate2D(texture_ref* texture, int width, int height, const char* name, qbool is_lightmap);
void VK_TexturesCreate(r_texture_type_id type, int n, texture_ref* textures);
void VK_TextureReplaceSubImageRGBA(texture_ref ref, int offsetx, int offsety, int width, int height, byte* buffer);
void VK_TextureSetFiltering(texture_ref texture, texture_minification_id minification_filter, texture_magnification_id magnification_filter);
void VK_TextureSetAnisotropy(texture_ref ref, int anisotropy);
void VK_TextureLoadCubemapFace(texture_ref cubemap, r_cubemap_direction_id direction, const byte* data, int width, int height);

// move me
VkCommandBuffer VK_BeginSingleTimeCommands(void);
void VK_EndSingleTimeCommands(VkCommandBuffer commandBuffer);
uint32_t VK_FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
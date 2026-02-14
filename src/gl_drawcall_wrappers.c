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

#include <SDL.h>

#include "quakedef.h"
#include "gl_model.h"
#include "gl_local.h"
#include "tr_types.h"
#include "r_vao.h"

// <draw-functions (various)>
GL_StaticProcedureDeclaration(glMultiDrawArrays, "mode=%u, first=%p, count=%p, drawcount=%d", GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount)
GL_StaticProcedureDeclaration(glMultiDrawElements, "mode=%u, count=%p, type=%u, indices=%p, drawcount=%d", GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices, GLsizei drawcount)
GL_StaticProcedureDeclaration(glDrawElementsBaseVertex, "mode=%u, count=%d, type=%u, indices=%p, basevertex=%d", GLenum mode, GLsizei count, GLenum type, GLvoid* indices, GLint basevertex)
GL_StaticProcedureDeclaration(glPrimitiveRestartIndex, "index=%u", GLuint index)

// (modern/4.3+)
// typedef void (APIENTRY *glDrawArraysInstanced_t)(GLenum mode, GLint first, GLsizei count, GLsizei primcount);
GL_StaticProcedureDeclaration(glMultiDrawArraysIndirect, "mode=%u, indirect=%p, drawcount=%d, stride=%d", GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride)
GL_StaticProcedureDeclaration(glMultiDrawElementsIndirect, "mode=%u, type=%u, indirect=%p, drawcount=%d, stride=%d", GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride)
GL_StaticProcedureDeclaration(glDrawArraysInstancedBaseInstance, "mode=%u, first=%d, count=%d, primcount=%d, baseinstance=%u", GLenum mode, GLint first, GLsizei count, GLsizei primcount, GLuint baseinstance)
GL_StaticProcedureDeclaration(glDrawElementsInstancedBaseInstance, "mode=%u, count=%d, type=%u, indices=%p, primcount=%d, baseinstance=%d", GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount, GLuint baseinstance)
GL_StaticProcedureDeclaration(glDrawElementsInstancedBaseVertexBaseInstance, "mode=%u, count=%d, type=%u, indices=%p, primcount=%d, basevertex=%d, baseinstance=%u", GLenum mode, GLsizei count, GLenum type, GLvoid* indices, GLsizei primcount, GLint basevertex, GLuint baseinstance)
GL_StaticProcedureDeclaration(glDrawElementsIndirect, "mode=%u, type=%u, indirect=%p", GLenum mode, GLenum type, GLvoid* indirect)
GL_StaticProcedureDeclaration(glUniformBlockBinding, "program=%u, index=%u, binding=%u", GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
GL_StaticFunctionDeclaration(glGetUniformBlockIndex, "program=%u, name=%s", "returns %u", GLuint, GLuint program, const GLchar *uniformBlockName)
GL_StaticFunctionWrapperBody(glGetUniformBlockIndex, GLuint, program, uniformBlockName)

// </draw-functions>

void GL_LoadDrawFunctions(void)
{
	glConfig.supported_features &= ~(R_SUPPORT_INDIRECT_RENDERING | R_SUPPORT_INSTANCED_RENDERING | R_SUPPORT_PRIMITIVERESTART);

	if (GL_VersionAtLeast(4, 3) || SDL_GL_ExtensionSupported("GL_ARB_multi_draw_indirect")) {
		qbool all_available = true;

		GL_LoadMandatoryFunctionExtension(glMultiDrawArraysIndirect, all_available);
		GL_LoadMandatoryFunctionExtension(glMultiDrawElementsIndirect, all_available);

		glConfig.supported_features |= (all_available ? R_SUPPORT_INDIRECT_RENDERING : 0);
	}

	if (GL_VersionAtLeast(4, 2) || SDL_GL_ExtensionSupported("GL_ARB_base_instance")) {
		qbool all_available = true;

		GL_LoadMandatoryFunctionExtension(glDrawArraysInstancedBaseInstance, all_available);
		GL_LoadMandatoryFunctionExtension(glDrawElementsInstancedBaseInstance, all_available);
		GL_LoadMandatoryFunctionExtension(glDrawElementsInstancedBaseVertexBaseInstance, all_available);

		glConfig.supported_features |= (all_available ? R_SUPPORT_INSTANCED_RENDERING : 0);
	}

	if (GL_VersionAtLeast(4, 0)) {
		qbool all_available = true;
		GL_LoadMandatoryFunctionExtension(glDrawElementsIndirect, all_available);
		GL_LoadMandatoryFunctionExtension(glUniformBlockBinding, all_available);
		GL_LoadMandatoryFunctionExtension(glGetUniformBlockIndex, all_available);

		// lies
		glConfig.supported_features |= (all_available ? R_SUPPORT_INSTANCED_RENDERING : 0);
	}


	// Draw functions used for modern & classic
	GL_LoadOptionalFunction(glMultiDrawArrays);
	GL_LoadOptionalFunction(glMultiDrawElements);

	// Use this instead of glDrawArrays() if on particular drivers (see github bug #416)
	if (GL_Available(glMultiDrawArrays) && glConfig.amd_issues) {
		glConfig.broken_features |= R_BROKEN_PREFERMULTIDRAW;
	}

	if (GL_VersionAtLeast(3, 2) || SDL_GL_ExtensionSupported("GL_ARB_draw_elements_base_vertex")) {
		GL_LoadOptionalFunction(glDrawElementsBaseVertex);
	}

	glConfig.supported_features &= ~R_SUPPORT_PRIMITIVERESTART;
	if (R_UseModernOpenGL() || GL_VersionAtLeast(3, 1)) {
		GL_LoadOptionalFunction(glPrimitiveRestartIndex);
		if (GL_Available(glPrimitiveRestartIndex)) {
			glEnable(GL_PRIMITIVE_RESTART);
			if (GL_VersionAtLeast(4, 3)) {
				glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
			}
			else {
				GL_Procedure(glPrimitiveRestartIndex, (~(GLuint)0));
			}
			glConfig.supported_features |= R_SUPPORT_PRIMITIVERESTART;
		}
	}
}

// Wrappers around drawing functions
void GL_MultiDrawArrays(GLenum mode, GLint* first, GLsizei* count, GLsizei primcount)
{
	if (GL_Available(glMultiDrawArrays)) {
		GL_Procedure(glMultiDrawArrays, mode, first, count, primcount);
		++frameStats.draw_calls;
		frameStats.subdraw_calls += primcount;
	}
	else {
		int i;
		for (i = 0; i < primcount; ++i) {
			GL_DrawArrays(mode, first[i], count[i]);
		}
	}
}

void GL_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
	R_TraceLogAPICall("glDrawArrays(%d verts)", count);
	assert(R_VAOBound());
	if (!R_VAOBound()) {
		Con_Printf("GL_DrawArrays() with no VAO bound\n");
		return;
	}
	if (GL_WorkaroundNeeded(R_BROKEN_PREFERMULTIDRAW) && GL_Available(glMultiDrawArrays)) {
		GL_Procedure(glMultiDrawArrays, mode, &first, &count, 1);
	}
	else {
		GL_BuiltinProcedure(glDrawArrays, "(mode=%u, first=%d, count=%d)", mode, first, count);
	}
	++frameStats.draw_calls;
}

void GL_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, GLvoid* indices, GLint basevertex)
{
	if (basevertex && !GL_Available(glDrawElementsBaseVertex)) {
		Sys_Error("glDrawElementsBaseVertex called, not supported");
	}
	else if (GL_Available(glDrawElementsBaseVertex)) {
		GL_Procedure(glDrawElementsBaseVertex, mode, count, type, indices, basevertex);
	}
	else {
		GL_BuiltinProcedure(glDrawElements, "mode=%u, count=%d, type=%u, indices=%p", mode, count, type, indices);
	}
	++frameStats.draw_calls;
}

qbool GL_DrawElementsBaseVertexAvailable(void)
{
	return GL_Available(glDrawElementsBaseVertex);
}

void GL_DrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices)
{
	R_TraceLogAPICall("glDrawElements(%d verts)", count);
	glDrawElements(mode, count, type, indices);
	++frameStats.draw_calls;
}

void GL_MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride)
{
	GL_Procedure(glMultiDrawArraysIndirect, mode, indirect, drawcount, stride);
	++frameStats.draw_calls;
	frameStats.subdraw_calls += drawcount;
}

void GL_MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride)
{
	GL_Procedure(glMultiDrawElementsIndirect, mode, type, indirect, drawcount, stride);
	++frameStats.draw_calls;
	frameStats.subdraw_calls += drawcount;
}

void GL_DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, GLvoid* indices, GLsizei primcount, GLint basevertex, GLuint baseinstance)
{
	GL_Procedure(glDrawElementsInstancedBaseVertexBaseInstance, mode, count, type, indices, primcount, basevertex, baseinstance);
	++frameStats.draw_calls;
}

void glGetBufferParameteriv(GLenum t, GLenum v, void* data);
void glUnmapBuffer(GLenum t);
void glGetVertexAttribiv (GLuint index, GLenum pname, GLint *params);
void glBindBuffer(GLenum target, GLuint buffer);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);

void GL_DrawElementsIndirect(GLenum mode, GLenum type, GLvoid* indirect)
{
	/*
	GLint vaoId;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vaoId);
	GLint indirectId;
	glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &indirectId);
	GLint earrayBufferId;
	glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &earrayBufferId);
	GLint arrayBufferId;
	glGetIntegerv(GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &arrayBufferId);
	GLint indirectSize;
	glGetBufferParameteriv(GL_DRAW_INDIRECT_BUFFER, GL_BUFFER_SIZE, &indirectSize);
	GLint indirectMapped;
	glGetBufferParameteriv(GL_DRAW_INDIRECT_BUFFER, GL_BUFFER_MAPPED, &indirectMapped);
	GLint earrayBufferSize;
	glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &earrayBufferSize);
	GLint earrayBufferMapped;
	glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_MAPPED, &earrayBufferMapped);
	GLint arrayBufferSize;
	glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &arrayBufferSize);
	GLint arrayBufferMapped;
	glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &arrayBufferMapped);
		//glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
		//DrawElementsIndirectCommand *foo = (DrawElementsIndirectCommand*) GL_Function(glMapBufferRange, GL_DRAW_INDIRECT_BUFFER, 0, sizeof(DrawElementsIndirectCommand), GL_MAP_WRITE_BIT);

	printf("vaoId: %d, indirectId: %d, indirectSize: %d, indirectMapped: %d, arrayBufferId: %d, arrayBufferSize: %d, arrayBufferMapped: %d, arrayBufferId: %d, arrayBufferSize: %d, arrayBufferMapped: %d\n",
		vaoId, indirectId, indirectSize, indirectMapped, earrayBufferId, earrayBufferSize, earrayBufferMapped, arrayBufferId, arrayBufferSize, arrayBufferMapped);

	const GLubyte* version = glGetString(GL_VERSION);
	printf("OpenGL Version: %s\n", version);

	GLenum err;
	while ((err = glGetError()) != GL_NO_ERROR) {
		printf("OpenGL Error: %d\n", err);
	}

	GLint linked;
	//glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (linked != GL_TRUE) {
		printf("Shader program not linked\n");
		// Print program info log for details
	}


	GLint i;
	GLint maxAttribs;
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
	for (i = 0; i < maxAttribs; i++) {
		GLint enabled, buffer;
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
		if (enabled == GL_TRUE) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
			if (buffer == 0) {
				printf("Attribute %d enabled but no buffer bound\n", i);
			}
		}
	}

	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
	for (i = 0; i < maxAttribs; i++) {
		GLint enabled;
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
		if (enabled == GL_TRUE) {
			GLint buffer;
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
			if (buffer != 0) {
				GLint mapped;
				glBindBuffer(GL_ARRAY_BUFFER, buffer);
				glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &mapped);
				if (mapped == GL_TRUE) {
					printf("Vertex buffer %d for attribute %d is mapped\n", buffer, i);
				} else {
					printf("not mapped attrib: %d, buffer: %d\n", i, buffer);
				}
			}
		} else {
			printf("%d not enabled\n", i);
		}
	}
*/
	GL_Procedure(glDrawElementsIndirect, mode, type, indirect);
	++frameStats.draw_calls;
}

void GL_UniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding)
{
	GL_Procedure(glUniformBlockBinding, program, uniformBlockIndex, uniformBlockBinding);
}

GLuint GL_GetUniformBlockIndex(GLuint program, const GLchar *uniformBlockName)
{
	return GL_Function(glGetUniformBlockIndex, program, uniformBlockName);
}

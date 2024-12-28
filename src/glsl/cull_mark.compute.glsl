#version 430

layout(local_size_x=64) in;

struct DrawElementsIndirectCommand
{
	uint	count;
	uint	instanceCount;
	uint	firstIndex;
	uint	baseVertex;
	uint	baseInstance;
};

// TODO: Blacklist broken Intel drivers if needed
layout(std430, binding=1) buffer DrawIndirectBuffer
{
	DrawElementsIndirectCommand cmds[];
};

layout(std430, binding=2) restrict writeonly buffer IndexBuffer
{
	uint indices[];
};

layout(std430, binding=3) restrict readonly buffer VisBuffer
{
	uint vis[];
};

struct MarkSurface
{
	uint packedleafsky; // bit 0=sky; 1..31=leafindex
	uint surfindex;
};

layout(std430, binding=4) restrict readonly buffer MarkSurfaceBuffer
{
	MarkSurface marksurfs[];
};

struct Surface
{
	vec4	plane;
	uint	framecount;
	uint	texnum;
	uint	numedges;
	uint	firstvert;
	vec3	mins;
	uint	_pad0;
	vec3	maxs;
	uint	_pad1;
};

// TODO: Blacklist broken Intel drivers if needed
layout(std430, binding=5) restrict buffer SurfaceBuffer
{
	Surface surfaces[];
};

layout(std140, binding=1) uniform FrameCullUBO
{
	vec4	frustum[4];
	vec3	vieworg;
	uint	oldskyleaf;
	uint	framecount;
};

void main()
{
	uint thread_id = gl_GlobalInvocationID.x;
	if (thread_id >= marksurfs.length())
		return;
	MarkSurface mark = marksurfs[thread_id];

	// sky culling: when r_oldskyleaf is 0, surfaces inside a sky leaf are skipped
	if ((mark.packedleafsky & 1u) > oldskyleaf)
		return;

	// vis culling
	uint leaf = mark.packedleafsky >> 1u;
	uint visible = vis[leaf >> 5u] & (1u << (leaf & 31u));
	if (visible == 0u)
		return;

	Surface surfbase = surfaces[mark.surfindex];

	// backface culling
	if (dot(surface.plane.xyz, vieworg) < surface.plane.w)
		return;

	// frustum culling
	vec3 mins = surface.mins;
	vec3 maxs = surface.maxs;
	for (uint i = 0u; i < 4u; i++)
	{
		vec4 plane = frustum[i];
		vec3 v;
		v.x = plane.x < 0.0 ? mins.x : maxs.x;
		v.y = plane.y < 0.0 ? mins.y : maxs.y;
		v.z = plane.z < 0.0 ? mins.z : maxs.z;
		if (dot(plane.xyz, v) < plane.w)
			return;
	}

	// surfaces can appear in multiple leaves
	// check if this is the first time this surface has passed culling this frame
	if (atomicExchange(surface.framecount, framecount) == framecount)
		return;

	// surface is visible, append its triangles to the index buffer
	// and update the draw command corresponding to its texture number
	uint firstvert = surface.firstvert;

	DrawElementsIndirectCommand cmd = cmds[surface.texnum];
	// some bsps out there have faces with < 2 edges, which would cause underflow below
	numedges = max(surface.numedges, 2u);
	uint ofs = cmd.firstIndex + atomicAdd(cmd.count, 3u * (numedges - 2u));
	for (uint i = 2u; i < numedges; i++)
	{
		indices[ofs++] = firstvert;
		indices[ofs++] = firstvert + i - 1u;
		indices[ofs++] = firstvert + i;
	}
}
#ezquake-definitions

#if __VERSION__ >= 330
#define attribute in
#define varying out
#endif

uniform vec3 cameraPosition;

varying vec3 Direction;

void main()
{
	gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; // ftransform();
	Direction = (gl_Vertex.xyz - cameraPosition);
#if defined(DRAW_SKYBOX)
	Direction = vec3(-Direction.y, Direction.z, Direction.x);
#endif
}

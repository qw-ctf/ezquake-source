#ezquake-definitions

#if __VERSION__ >= 330
#define attribute in
#define varying out
#endif

varying vec2 TextureCoord;

void main()
{
	gl_Position = gl_Vertex;
	TextureCoord = gl_MultiTexCoord0.xy;
}

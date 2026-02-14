#ezquake-definitions

#if __VERSION__ >= 330
#define attribute in
#define varying out
#endif

varying vec2 TextureCoord;

void main()
{
	gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; // ftransform();
	TextureCoord = gl_MultiTexCoord0.st;
}

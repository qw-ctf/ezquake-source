#ezquake-definitions

#if __VERSION__ >= 330
#define attribute in
#define varying out
#endif

#ifndef FLAT_COLOR
varying vec2 TextureCoord;
#endif

void main()
{
	gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; // ftransform();
#ifndef FLAT_COLOR
	TextureCoord = gl_MultiTexCoord0.st;
#endif
}

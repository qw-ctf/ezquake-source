#ezquake-definitions

#if __VERSION__ >= 330
out vec4 fragColor;
#else
#define fragColor gl_FragColor
#endif

uniform sampler2D texSampler;
uniform vec4 fsBaseColor1;
uniform vec4 fsBaseColor2;

#if __VERSION__ >= 330
in vec2 fsTextureCoord;
in vec2 fsAltTextureCoord;
#else
varying vec2 fsTextureCoord;
varying vec2 fsAltTextureCoord;
#endif

void main()
{
	vec4 tex = texture2D(texSampler, fsTextureCoord);
	vec4 alt = texture2D(texSampler, fsAltTextureCoord);

	fragColor = tex * fsBaseColor1 + alt * fsBaseColor2;

#ifdef DRAW_FOG
	fragColor = applyFog(fragColor, gl_FragCoord.z / gl_FragCoord.w);
#endif
}

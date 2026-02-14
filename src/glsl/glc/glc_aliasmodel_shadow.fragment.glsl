#ezquake-definitions

#if __VERSION__ >= 330
out vec4 fragColor;
#else
#define fragColor gl_FragColor
#endif

void main()
{
	fragColor = vec4(0, 0, 0, 0.5);

#ifdef DRAW_FOG
	fragColor = applyFog(fragColor, gl_FragCoord.z / gl_FragCoord.w);
#endif
}

#version 430

layout (location = 0) out vec4 frag;

layout (binding = 0) uniform sampler2D accum;
layout (binding = 1) uniform sampler2D reveal;

bool isApproximatelyEqual(float a, float b)
{
    return abs(a - b) <= (abs(a) < abs(b) ? abs(b) : abs(a)) * 1e-5;
}

float max3(vec3 v)
{
    return max(max(v.x, v.y), v.z);
}

void main()
{
    ivec2 coords = ivec2(gl_FragCoord.xy);

    float revealage = texelFetch(reveal, coords, 0).r;

    if (isApproximatelyEqual(revealage, 1.0f))
        discard;

    vec4 accumulation = texelFetch(accum, coords, 0);

    if (isinf(max3(abs(accumulation.rgb))))
        accumulation.rgb = vec3(accumulation.a);

    vec3 average_color = accumulation.rgb / max(accumulation.a, 1e-5);

    frag = vec4(clamp(average_color, 0.0, 1.0), 1.0f - revealage);
}
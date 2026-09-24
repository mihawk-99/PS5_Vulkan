#version 450

// R59: gl_FragCoord's z and w, as the red and green channels.
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(gl_FragCoord.z, gl_FragCoord.w, 0.0, 1.0);
}

#version 450

// R59: a quad whose depth comes from its coordinate's x (0.25 on the left edge
// to 0.75 on the right) and whose clip w is 2, so gl_FragCoord.z ramps across
// the frame and gl_FragCoord.w is 0.5 everywhere.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 coordinate;

void main()
{
    gl_Position = vec4(position, coordinate.x, 1.0) * 2.0;
}

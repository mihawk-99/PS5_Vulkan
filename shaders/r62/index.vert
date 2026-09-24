#version 450

// R62: a uniform array indexed by a vertex attribute, the way Dolphin picks each
// vertex's transform matrix (ctrmtx[posidx], posidx an R8G8B8A8_UINT attribute).
layout(location = 0) in vec2 position;
layout(location = 1) in uvec4 index;

layout(set = 0, binding = 0) uniform Rows
{
    vec4 rows[64];
} block;

layout(location = 0) out vec4 vertex_colour;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vertex_colour = block.rows[index.x];
}

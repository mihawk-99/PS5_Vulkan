#version 450

// R63: Dolphin's skinned-vertex record (36 bytes): a matrix index as
// R8G8B8A8_UINT at offset 0, the position at 4, the normal at 16 and a texture
// coordinate at 28. The band's place comes from the position; its colour from
// the normal's x, the coordinate's y and the row the index selects.
layout(location = 0) in vec3 position;
layout(location = 1) in uvec4 index;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec2 coordinate;

layout(set = 0, binding = 0) uniform Rows
{
    vec4 rows[64];
} block;

layout(location = 0) out vec4 vertex_colour;

void main()
{
    gl_Position = vec4(position, 1.0);
    vertex_colour = vec4(normal.x, coordinate.y, block.rows[index.x].z, 1.0);
}

#version 450
layout(set = 0, binding = 0) readonly buffer Value { vec4 value; };
layout(location = 0) out vec4 color;
void main() { color = value; }

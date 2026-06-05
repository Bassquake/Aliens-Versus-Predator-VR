#version 450
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(set=1, binding=0) uniform UBO { mat4 mvp; };
layout(location=0) out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = mvp * vec4(aPos, 1.0);
}
#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;

struct Light {
    vec3 position;
    float intensity;
    vec3 color;
    int enabled;
};
layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec3 cameraPos;
    float padding1;
    Light lights[3];
    int lightCount;
    float padding2[3];
} ubo;

struct Color4 { float r, g, b, a; };
layout(set = 0, binding = 3, std430) buffer InstanceColors {
    Color4 colors[];
};

layout(location = 0) rayPayloadInEXT vec3 hitValue;
layout(location = 1) rayPayloadEXT bool isShadowed;

hitAttributeEXT vec2 attribs;

vec3 getShadingNormal() {
    return normalize((gl_ObjectToWorldEXT * vec4(0, 1, 0, 0)).xyz);
}

void main() {
    vec3 worldPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    uint idx = gl_InstanceCustomIndexEXT; 
    vec3 objectColor = vec3(colors[idx].r, colors[idx].g, colors[idx].b);

    hitValue = objectColor;
}

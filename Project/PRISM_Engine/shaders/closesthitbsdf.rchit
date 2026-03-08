#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload {
    vec3 color;
    float hitT;
};

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main() {
    payload.color = vec3(1.0, 0.0, 0.0); // 히트 시 무조건 빨간색
    payload.hitT = gl_HitTEXT;
}

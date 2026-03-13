#version 460
#extension GL_EXT_ray_tracing : require

// raygen, closesthit 과 완전히 동일한 구조체로 맞춰야 합니다
struct HitPayload {
    vec3 hitPos;
    vec3 normal;
    vec3 geomNormal;
    vec3 albedo;
    float roughness;
    float metallic;
    vec3 emissive;
    float hitT;
    float specTrans;
    float ior;
    float isRaster;
};

layout(location = 0) rayPayloadInEXT HitPayload payload;

void main() {
    // 하늘(배경)에 맞았다는 신호만 명확하게 주면 됩니다.
    // 나머지 값들은 raygen에서 어차피 쓰지 않으므로 초기화할 필요가 없습니다.
    payload.hitT = -1.0; 
    
    // (선택 사항) 만약 raygen에서 하늘 색상을 직접 더하지 않고 
    // payload.emissive를 통해 처리하도록 짜셨다면 이렇게 하셔도 됩니다.
    // payload.emissive = vec3(0.05, 0.05, 0.1); 
}
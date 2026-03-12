#version 460
#extension GL_EXT_ray_tracing : require

// [수정] raygen, closesthit과 완전히 동일한 구조체로 맞추어야 합니다!
struct HitPayload {
    vec3 hitPos;     // 충돌한 3D 월드 좌표
    vec3 normal;     // 표면 법선 벡터
    vec3 geomNormal; //충돌 회피용 (진짜 법선)
    vec3 albedo;     // 기본 색상
    float roughness; // 거칠기
    float metallic;  // 금속성
    vec3 emissive;   // 발광 색상 (전구면 색상이 있고, 아니면 0)
    float hitT;      // 충돌 거리 (-1.0이면 하늘/허공에 맞은 것으로 규칙을 정함)
    // --- [추가] ---
    float specTrans; // 투명도
    float ior;       // 굴절률
    float subsurface; // 산란 확률
    float ScatterDIstance; // 산란 거리 (투과 시 광선이 내부에서 얼마나 이동했는지)
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
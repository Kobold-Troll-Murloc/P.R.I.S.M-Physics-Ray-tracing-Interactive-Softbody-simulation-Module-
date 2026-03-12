#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;

// 조명 구조체 정의
struct Light {
    vec3 position;
    float intensity;
    vec3 color;
    int enabled;
};

// [수정] C++과 동일한 레이아웃의 머티리얼 구조체
struct InstanceMaterial {
    vec4 albedo;     // rgb: 색상, a: 투명도
    vec4 pbrParams1;  // x: 발광강도, y: 거칠기, z: 금속성, w: 여분
    vec4 pbrParams2; // 새로 추가된 그룹
};

// 기존 Binding 3 버퍼를 이 구조체 배열로 받습니다.
layout(set = 0, binding = 3, std430) buffer InstanceMaterials { 
    InstanceMaterial materials[]; 
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

//layout(set = 0, binding = 3, std430) buffer InstanceColors {
    //Color4 colors[];
//};

struct Vertex {
  vec3 pos;
  float pad1; // C++와 동일하게 추가
  vec3 normal;
  float pad2; // C++와 동일하게 추가
};

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
    float scatterDist; // 산란 거리 (투과 시 광선이 내부에서 얼마나 이동했는지)
};

layout(buffer_reference, scalar) buffer Vertices { Vertex v[]; };
layout(buffer_reference, scalar) buffer Indices { uint i[]; };

struct ObjDesc {
  uint64_t vertexAddress;
  uint64_t indexAddress;
};

layout(set = 0, binding = 4, scalar) buffer ObjDescBuffer {
  ObjDesc i[];
} objDesc;

// Ray Payloads
// layout(location = 0) rayPayloadInEXT vec3 hitValue;
layout(location = 0) rayPayloadInEXT HitPayload payload; // vec3 hitValue 대체
layout(location = 1) rayPayloadEXT bool isShadowed;

hitAttributeEXT vec2 attribs;


vec3 getShadingNormal() {
    uint objId = gl_InstanceCustomIndexEXT;
    ObjDesc desc = objDesc.i[objId];
    Vertices vertices = Vertices(desc.vertexAddress);
    Indices indices = Indices(desc.indexAddress);

    uint ind0 = indices.i[3 * gl_PrimitiveID + 0];
    uint ind1 = indices.i[3 * gl_PrimitiveID + 1];
    uint ind2 = indices.i[3 * gl_PrimitiveID + 2];

    vec3 n0 = vertices.v[ind0].normal;
    vec3 n1 = vertices.v[ind1].normal;
    vec3 n2 = vertices.v[ind2].normal;

    const vec3 barycentrics = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec3 normal = n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z;
    
    vec3 worldNormal = normalize(vec3(normal * gl_WorldToObjectEXT));
    
    // if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0) {
        // worldNormal = -worldNormal;
    // }
    return worldNormal;
}

void main() {
    uint idx = gl_InstanceCustomIndexEXT; 

    // --- [추가됨] 진짜 기하학적 법선(Geometry Normal) 계산 ---
    ObjDesc desc = objDesc.i[idx];
    Vertices vertices = Vertices(desc.vertexAddress);
    Indices indices = Indices(desc.indexAddress);

    uint ind0 = indices.i[3 * gl_PrimitiveID + 0];
    uint ind1 = indices.i[3 * gl_PrimitiveID + 1];
    uint ind2 = indices.i[3 * gl_PrimitiveID + 2];

    vec3 v0 = vertices.v[ind0].pos;
    vec3 v1 = vertices.v[ind1].pos;
    vec3 v2 = vertices.v[ind2].pos;

    vec3 geomNormal = normalize(cross(v1 - v0, v2 - v0));
    geomNormal = normalize(vec3(geomNormal * gl_WorldToObjectEXT));
    
    // if (dot(geomNormal, gl_WorldRayDirectionEXT) > 0.0) {
        // geomNormal = -geomNormal;
    // }
    // -----------------------------------------------------------
    
    // 1. 버퍼에서 독립된 데이터들을 아주 깔끔하게 꺼내옵니다!
    vec4 albedoAlpha = materials[idx].albedo;
    vec4 params1 = materials[idx].pbrParams1;
    vec4 params2 = materials[idx].pbrParams2;
    
    vec3 baseColor = albedoAlpha.rgb;
    
    // payload에 데이터 넣기
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.normal = getShadingNormal();
    payload.geomNormal = geomNormal;
    payload.hitT = gl_HitTEXT; 
    
    payload.roughness = params1.y;
    payload.metallic = params1.z;
    
    // --- [추가] 페이로드에 투명도와 굴절률 전달 ---
    payload.specTrans = params2.x;
    payload.ior = params2.y > 0.0 ? params2.y : 1.5; // 방어 코드 (0.0 방지)
    // ----------------------------------------------

    // [가장 중요!] subsurface 변수를 반드시 초기화해야 사각형 노이즈가 사라집니다!
    // C++에서 params2.z에 매터리얼의 subsurface 값을 넘겨주고 있다고 가정합니다.
    payload.subsurface = params2.z;

    payload.scatterDist = params2.w; // (투과 시 내부에서 이동한 거리)

    float emissiveIntensity = params1.x;
    if (emissiveIntensity > 0.0) {
        payload.albedo = vec3(0.0);
        payload.emissive = baseColor * emissiveIntensity;
    } else {
        payload.albedo = baseColor;
        payload.emissive = vec3(0.0);
    }

    //payload.normal = getShadingNormal();
    
    // [디버그 전용] 조명을 무시하고, 법선 방향(Normal) 자체를 색상으로 만듭니다.
    // xyz(-1.0 ~ 1.0) 값을 rgb(0.0 ~ 1.0)으로 매핑
    //vec3 debugColor = payload.normal * 0.5 + 0.5; 
    
    //payload.emissive = debugColor;
    //payload.albedo = vec3(0.0);
}
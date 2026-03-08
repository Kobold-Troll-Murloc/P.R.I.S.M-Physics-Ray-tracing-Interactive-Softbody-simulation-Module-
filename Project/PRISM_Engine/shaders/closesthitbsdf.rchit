#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;

struct Light {
    vec3 position;
    float intensity;
    vec3 color;
    int enabled;
};

struct InstanceMaterial {
    vec4 albedo;
    vec4 pbrParams1;
    vec4 pbrParams2;
};

layout(set = 0, binding = 3, std430) buffer InstanceMaterials { 
    InstanceMaterial materials[]; 
};

layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec3 cameraPos;
    int frameCount; 
    Light lights[3];
    int lightCount;
    float padding2[3];
} ubo;

struct Vertex {
  vec3 pos;
  float pad1;
  vec3 normal;
  float pad2;
};

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

layout(location = 0) rayPayloadInEXT HitPayload payload;

hitAttributeEXT vec2 attribs;

void main() {
    uint idx = gl_InstanceCustomIndexEXT; 
    ObjDesc desc = objDesc.i[idx];
    Vertices vertices = Vertices(desc.vertexAddress);
    Indices indices = Indices(desc.indexAddress);

    uint i0 = indices.i[3 * gl_PrimitiveID + 0];
    uint i1 = indices.i[3 * gl_PrimitiveID + 1];
    uint i2 = indices.i[3 * gl_PrimitiveID + 2];

    vec3 v0 = vertices.v[i0].pos;
    vec3 v1 = vertices.v[i1].pos;
    vec3 v2 = vertices.v[i2].pos;

    vec3 n0 = vertices.v[i0].normal;
    vec3 n1 = vertices.v[i1].normal;
    vec3 n2 = vertices.v[i2].normal;

    const vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    
    vec3 normal = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    vec3 worldNormal = normalize(vec3(normal * gl_WorldToObjectEXT));
    
    vec3 geomNormal = normalize(cross(v1 - v0, v2 - v0));
    vec3 worldGeomNormal = normalize(vec3(geomNormal * gl_WorldToObjectEXT));

    InstanceMaterial mat = materials[idx];
    
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.normal = worldNormal;
    payload.geomNormal = worldGeomNormal;
    payload.hitT = gl_HitTEXT; 
    payload.albedo = mat.albedo.rgb;
    payload.roughness = mat.pbrParams1.y;
    payload.metallic = mat.pbrParams1.z;
    payload.emissive = mat.albedo.rgb * mat.pbrParams1.x;
    payload.specTrans = mat.pbrParams2.x;
    payload.ior = mat.pbrParams2.y > 0.0 ? mat.pbrParams2.y : 1.5;
}

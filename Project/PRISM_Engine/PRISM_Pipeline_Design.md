# PRISM Pipeline Design
> 작성일: 2026-03-12
> 방향: OGRE를 제대로 활용한 Hybrid Rendering + Softbody 통합

---

## 1. 핵심 방향

### 기존 접근의 문제

```
[잘못된 구조]

OGRE 래스터 ──→ [버림]
RT 전체     ──→ 화면

문제:
  - OGRE가 잘하는 래스터를 안 씀
  - RT가 모든 걸 혼자 계산 (과부하)
  - OGRE를 쓰는 이유가 없음
```

### 올바른 방향

```
[올바른 구조]

OGRE 래스터 ──→ G-Buffer (기하학 정보 저장)
RT          ──→ G-Buffer 기반으로 특수 효과만 계산
Composite   ──→ 합성 → 화면

역할 분담:
  OGRE가 잘하는 것 → OGRE에게
  RT가 필요한 것   → RT에게
```

### 원칙

- **래스터**: 빠르게 기하학 정보(G-Buffer) 생성
- **RT**: 래스터가 못하는 것만 (반사, 굴절, 정확한 그림자, GI)
- **OGRE HLMS**: 재질 시스템, 셰이더 변형 자동 관리
- **OGRE Compositor**: 패스 순서, 텍스처 연결, 배리어 관리
- **PASS_COMPUTE**: 물리 시뮬레이션을 Compositor 안에서 실행

---

## 2. 전체 파이프라인

```
매 프레임:

┌──────────────────────────────────────────────────────┐
│ [PASS_COMPUTE] 물리 시뮬레이션                       │
│   XPBD Compute Shader 실행                           │
│   positionSSBO (정점 위치) 갱신                      │
└───────────────────────┬──────────────────────────────┘
                        │ positionSSBO
┌───────────────────────▼──────────────────────────────┐
│ [PASS_COMPUTE] BLAS Refit                            │
│   갱신된 정점 위치 → BLAS 업데이트                   │
│   → TLAS 재빌드                                      │
└───────────────────────┬──────────────────────────────┘
                        │ 최신 TLAS
┌───────────────────────▼──────────────────────────────┐
│ [PASS_SCENE] G-Buffer 생성 (OGRE HLMS PBS)           │
│                                                      │
│   출력 텍스처:                                       │
│   ├── gbuffer_albedo   RGBA8    표면 색상            │
│   ├── gbuffer_normal   RGBA16F  표면 법선            │
│   ├── gbuffer_material RGBA8    roughness/metallic/  │
│   │                             specTrans/ior        │
│   └── gbuffer_depth    D32F     깊이                 │
└───────────────────────┬──────────────────────────────┘
                        │ G-Buffer 4장
┌───────────────────────▼──────────────────────────────┐
│ [PASS_CUSTOM] RT 특수 효과                           │
│                                                      │
│   입력: G-Buffer + TLAS                              │
│   출력: rt_reflection / rt_shadow / rt_ao            │
│                                                      │
│   ├── RT Reflection  metallic/specTrans 높은 픽셀    │
│   ├── RT Refraction  specTrans 높은 픽셀             │
│   ├── RT Shadow      정확한 소프트 섀도우            │
│   └── RT AO          Ambient Occlusion               │
└───────────────────────┬──────────────────────────────┘
                        │ rt_reflection, rt_shadow, rt_ao
┌───────────────────────▼──────────────────────────────┐
│ [PASS_QUAD] Composite                                │
│                                                      │
│   G-Buffer + RT 결과 합성                            │
│   → Tone Mapping (Reinhard)                          │
│   → Gamma Correction                                 │
│   → 최종 화면                                        │
└──────────────────────────────────────────────────────┘
```

---

## 3. G-Buffer 구성

### Compositor 텍스처 정의

```cpp
Ogre::CompositorNodeDef* nodeDef = comp->addNodeDefinition("PRISMNode");

// G-Buffer 텍스처 정의
auto addTex = [&](const char* name, Ogre::PixelFormatGpu fmt) {
    auto* texDef = nodeDef->addTextureDefinition(name);
    texDef->width  = 0; // 화면 크기 자동
    texDef->height = 0;
    texDef->format = fmt;
    texDef->textureFlags = Ogre::TextureFlags::RenderToTexture;
};

addTex("gbuffer_albedo",   Ogre::PFG_RGBA8_UNORM);
addTex("gbuffer_normal",   Ogre::PFG_RGBA16_FLOAT);
addTex("gbuffer_material", Ogre::PFG_RGBA8_UNORM);  // R=roughness G=metallic B=specTrans A=ior(encoded)
addTex("gbuffer_depth",    Ogre::PFG_D32_FLOAT);
addTex("rt_output",        Ogre::PFG_RGBA16_FLOAT); // RT 결과
```

### G-Buffer 패스

```cpp
// MRT (Multiple Render Target): 한 번의 드로우에 여러 텍스처에 동시 출력
Ogre::CompositorTargetDef* gbufferTarget = nodeDef->addTargetPass("gbuffer_mrt");
gbufferTarget->setNumPasses(1);

auto* sceneDef = static_cast<Ogre::CompositorPassSceneDef*>(
    gbufferTarget->addPass(Ogre::PASS_SCENE));
sceneDef->setAllClearColours(Ogre::ColourValue(0, 0, 0, 0));
sceneDef->setAllLoadActions(Ogre::LoadAction::Clear);
// MRT 출력 연결: albedo, normal, material, depth
```

---

## 4. HLMS 활용

### HLMS가 해주는 것

OGRE HLMS PBS는 재질 파라미터(roughness, metallic 등)를 읽고, 각 오브젝트에 맞는 셰이더를 자동으로 생성/캐싱합니다.

G-Buffer 출력 모드에서 HLMS가 조명 계산 없이 재질 정보만 G-Buffer에 씁니다.

### HlmsListener로 SSBO 바인딩

물리 시뮬레이션 후 갱신된 정점 위치를 래스터 셰이더에서 읽을 수 있도록 SSBO 바인딩을 추가합니다.

```cpp
class PrismHlmsListener : public Ogre::HlmsListener {
    VkBuffer mSoftbodyPositionSSBO; // XPBD 결과 버퍼

    void propertiesMergedPreGenerationStep(
        Ogre::Hlms* hlms, ...,
        const Ogre::HlmsDatablock* datablock,
        Ogre::Renderable* renderable) override
    {
        // Softbody 오브젝트 여부 프로퍼티 설정
        // → HLMS가 다른 셰이더 변형 자동 생성
        if (isSoftbodyObject(renderable))
            hlms->_setProperty("prism_softbody", 1);
    }
};
```

### HLMS Piece로 셰이더 코드 주입

```glsl
// Media/Hlms/Pbs/GLSL/Pieces/Structs_piece_vs.glsl 에 추가:

@piece(prism_softbody_decl)
@property(prism_softbody)
layout(std430, binding = 7) readonly buffer SoftbodyBuffer {
    vec4 softPositions[];  // XPBD 시뮬레이션 결과 정점 위치
};
@end
@end

// Vertex Shader에서:
@piece(prism_softbody_apply)
@property(prism_softbody)
    // 원래 정점 위치 대신 시뮬레이션 결과 사용
    vec4 simPos = softPositions[gl_VertexIndex];
    inVs_vertex = simPos.xyz;
@end
@end
```

---

## 5. RT 역할 재정의

### RT가 담당하는 것 (G-Buffer 기반)

```glsl
// RT raygen 셰이더에서:

// G-Buffer 읽기
vec4 albedo   = texture(gbuffer_albedo,   uv);
vec4 normal   = texture(gbuffer_normal,   uv);
vec4 material = texture(gbuffer_material, uv);
float depth   = texture(gbuffer_depth,    uv).r;

float roughness = material.r;
float metallic  = material.g;
float specTrans = material.b;

// 픽셀 월드 위치 복원
vec3 worldPos = ReconstructWorldPos(depth, uv, invViewProj);

// 재질에 따라 RT 계산 분기
if (metallic > 0.5 || specTrans > 0.5) {
    // 반사/굴절 광선 추적 (고비용, 퀄리티 필요)
    rt_output = TraceReflectionRefraction(worldPos, normal, ...);
} else {
    // 단순 AO만 (저비용)
    rt_output = TraceAmbientOcclusion(worldPos, normal);
}
```

### RT 역할 분류

| 효과 | 대상 픽셀 | 광선 수 | 우선순위 |
|:--|:--|:--|:--|
| RT Reflection | metallic > 0.5 | 1~4 | 높음 |
| RT Refraction | specTrans > 0.5 | 1~4 | 높음 |
| RT Soft Shadow | 모든 픽셀 | 1 (Shadow Ray) | 중간 |
| RT AO | 모든 픽셀 | 4~8 (짧은 광선) | 중간 |
| Full Path Tracing | 선택적 픽셀 | 7 바운스 | 낮음 (품질 모드) |

---

## 6. Composite 패스

```glsl
// Composite Fragment/Compute Shader:

vec3 albedo    = texture(gbuffer_albedo, uv).rgb;
vec3 normal    = texture(gbuffer_normal, uv).rgb * 2.0 - 1.0;
vec3 rtResult  = texture(rt_output, uv).rgb;

// 래스터 기본 조명 (직접광)
vec3 directLight = ComputeDirectLighting(albedo, normal, roughness, metallic);

// RT 결과 합성
vec3 finalColor = directLight + rtResult;

// Tone Mapping + Gamma
finalColor = finalColor / (finalColor + vec3(1.0));  // Reinhard
finalColor = pow(finalColor, vec3(1.0 / 2.2));       // Gamma 2.2

outColor = vec4(finalColor, 1.0);
```

---

## 7. Compositor 전체 노드 구성

```
PRISMWorkspace
│
└── PRISMNode
    │
    ├── [PASS_COMPUTE]  Physics (XPBD)
    │     positionSSBO 갱신
    │
    ├── [PASS_COMPUTE]  BLAS Refit
    │     positionSSBO → BLAS → TLAS
    │
    ├── [PASS_SCENE]    G-Buffer
    │     MRT: albedo / normal / material / depth
    │     HLMS PBS (G-Buffer 출력 모드)
    │
    ├── [PASS_CUSTOM]   RT Effects
    │     입력: G-Buffer 4장 + TLAS
    │     출력: rt_output
    │     vkCmdTraceRaysKHR
    │
    └── [PASS_QUAD]     Composite + Tone Mapping
          입력: G-Buffer + rt_output
          출력: Swapchain
```

---

## 8. 각 팀의 역할

| 팀 | 담당 | Compositor 패스 |
|:--|:--|:--|
| **3_HSH** | XPBD / FEM Compute Shader | PASS_COMPUTE (Physics) |
| **1_JGN** | G-Buffer 구성, RT 통합, Composite | PASS_SCENE + PASS_CUSTOM + PASS_QUAD |
| **2_LSM** | 전체 파이프라인 레퍼런스 | 5단계 파이프라인 설계 참고 |

---

## 9. 구현 단계

### Phase 1: G-Buffer 구성
- [ ] Compositor에 MRT 텍스처 정의 (albedo, normal, material, depth)
- [ ] PASS_SCENE이 G-Buffer에 출력되도록 설정
- [ ] G-Buffer 내용 화면에 디버그 출력으로 확인

### Phase 2: RT 패스 G-Buffer 연동
- [ ] RT raygen 셰이더가 G-Buffer 읽도록 수정
- [ ] 재질 기반 RT 분기 구현 (metallic/specTrans 기준)
- [ ] Composite 패스 작성

### Phase 3: HLMS 커스텀
- [ ] HlmsListener 작성 (SSBO 바인딩)
- [ ] HLMS Piece 작성 (Softbody 정점 치환)
- [ ] G-Buffer 출력에 재질 파라미터 인코딩

### Phase 4: 물리 통합 (HSH 연동)
- [ ] PASS_COMPUTE 물리 패스 추가
- [ ] BLAS Refit 패스 추가
- [ ] Softbody 변형이 G-Buffer와 RT 모두에 반영되는지 확인

### Phase 5: 고도화
- [ ] RT AO (짧은 광선, 저비용)
- [ ] RT Soft Shadow
- [ ] TAA 또는 간단한 Denoising
- [ ] 품질 모드: Full Path Tracing 전환

---

## 10. 핵심 원칙

1. **OGRE가 잘하는 것은 OGRE에게**
   래스터, 재질 변형, 패스 순서, 텍스처 연결

2. **RT는 OGRE가 못하는 것만**
   반사, 굴절, 정확한 그림자, GI

3. **G-Buffer가 연결고리**
   래스터와 RT가 G-Buffer를 통해 정보를 주고받음

4. **물리는 Compositor 첫 번째 패스**
   물리 → BLAS Refit → 래스터 → RT 순서를 Compositor가 보장

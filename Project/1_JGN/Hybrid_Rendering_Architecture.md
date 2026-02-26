# P.R.I.S.M 하이브리드 렌더링 아키텍처

> Rasterization + Ray Tracing 혼합 렌더링 설계서
> 작성일: 2026-02-24

---

## 1. 핵심 설계 철학

### "Item이 자신의 렌더링 방식을 알고 있다"

OGRE NEXT의 HLMS 시스템을 확장하여, **각 Item(오브젝트)이 렌더링 시점에 래스터화/레이트레이싱/혼합 중 어느 방식으로 그려질지를 데이터블록(Datablock) 단위로 결정**한다.

```
SceneManager
├── Item A (Cube)          → RenderMode: Rasterize
├── Item B (Mirror)        → RenderMode: RayTrace
├── Item C (Glass Sphere)  → RenderMode: Hybrid (표면=Rasterize, 굴절/반사=RayTrace)
└── Item D (SoftBody Mesh) → RenderMode: Hybrid (기본 표면=Rasterize, 고품질 자기그림자=RayTrace)
```

---

## 2. Per-Item 렌더 모드 시스템

### 2.1 렌더 모드 정의 (HLMS Property)

```cpp
// HlmsPbsDatablock 혹은 커스텀 PRISMDatablock에 추가할 필드
enum PrismRenderMode
{
    PRISM_RENDER_RASTERIZE  = 0,  // 래스터화만 사용
    PRISM_RENDER_RAYTRACING = 1,  // 레이트레이싱만 사용
    PRISM_RENDER_HYBRID     = 2,  // 표면=래스터화, 특수효과=레이트레이싱
};
```

### 2.2 HLMS Property 매핑

| HLMS Property | 의미 |
| :--- | :--- |
| `prism_render_mode` | 0/1/2: 렌더 모드 |
| `prism_rt_reflections` | 레이트레이싱 반사 사용 여부 |
| `prism_rt_shadows` | 레이트레이싱 그림자 사용 여부 |
| `prism_rt_refraction` | 레이트레이싱 굴절 사용 여부 |
| `hlms_compute_deform` | Compute Shader 변형 적용 여부 (SoftBody용) |
| `prism_softbody_dynamic_blas` | 매 프레임 BLAS 재빌드 필요 여부 |

### 2.3 HLMS 내부 처리 흐름

```
calculateHash() 호출
  ├── Datablock의 prism_render_mode 확인
  ├── → RASTERIZE: 일반 PBS 쉐이더 생성 (기존 경로)
  ├── → RAYTRACING: RayGen/Hit/Miss 쉐이더 조립
  └── → HYBRID:
        ├── Rasterize용: G-Buffer 기록 쉐이더 생성
        └── RayTrace용:  G-Buffer 읽어서 RT 합성 쉐이더 생성
```

---

## 3. 프레임 렌더링 파이프라인 (Compositor 구성)

```
한 프레임 = 아래 패스들의 순서대로 실행

[1] Compute Pass: SoftBody 물리 연산
      └── Compute Shader (FEM/XPBD/PhysicsAI) → Deformed Vertex SSBO 기록

[2] BLAS Update Pass (커스텀)
      └── prism_softbody_dynamic_blas=1 인 Item의 BLAS 재빌드
          (변형된 Vertex SSBO 기반)

[3] TLAS Update Pass (커스텀)
      └── 씬 전체 TLAS 업데이트 (동적 물체 위치 반영)

[4] Rasterize Pass (G-Buffer 생성)
      └── RenderMode = RASTERIZE 또는 HYBRID 인 Item들
          → G-Buffer에 기록 (Position, Normal, Albedo, SpecularRoughness)

[5] Ray Tracing Pass (커스텀 Compositor Pass: PASS_RAYTRACING)
      └── RenderMode = RAYTRACING 또는 HYBRID 인 Item들에 대해
          → Ray Generation Shader 실행
          → G-Buffer + TLAS 참조하여 RT 효과 계산
          → RT 결과 텍스처에 기록

[6] Composite Pass
      └── G-Buffer + RT 결과 텍스처 → 최종 합성
          (HYBRID Item: 표면은 래스터 G-Buffer, 반사/그림자는 RT 결과로 교체)

[7] Post-Process Pass
      └── Denoising, TAA, Tone Mapping
```

### 3.1 Compositor 스크립트 구조 (의사 코드)

```
compositor_node PrismMainNode
{
    // --- Input/Output 텍스처 정의 ---
    texture GBuffer_Position    full_surface PF_FLOAT32_RGBA
    texture GBuffer_Normal      full_surface PF_FLOAT16_RGBA
    texture GBuffer_Albedo      full_surface PF_BYTE_RGBA
    texture RT_Output           full_surface PF_FLOAT16_RGBA
    texture Final               full_surface PF_BYTE_RGBA

    // [1] SoftBody Compute
    pass compute
    {
        job SoftBodyComputeJob    // HlmsCompute 잡 이름
    }

    // [4] G-Buffer 래스터화
    pass render_scene
    {
        load  { all clear }
        store { colour_0 store  colour_1 store  colour_2 store }
        rq_first  0
        rq_last   200   // RASTERIZE/HYBRID Item들만
    }

    // [5] RT Pass (커스텀)
    pass ray_tracing   // PASS_RAYTRACING (엔진 확장 필요)
    {
        input 0 GBuffer_Position
        input 1 GBuffer_Normal
        output RT_Output
        shader_job PrismRayTracingJob
    }

    // [6] Composite
    pass quad
    {
        input 0 GBuffer_Albedo
        input 1 RT_Output
        material PrismComposite
    }
}
```

---

## 4. Single Item 내부 하이브리드 전략

### 4.1 개념: "같은 메시, 두 번 참여"

HYBRID 모드의 Item은 한 프레임에 **두 패스에 모두 참여**한다:
- **래스터 패스**: 표면 색상/법선/Roughness를 G-Buffer에 기록
- **RT 패스**: G-Buffer에 기록된 자신의 데이터를 바탕으로 반사/굴절/그림자를 RT로 계산

```
Item (유리 구체, HYBRID 모드)
  ├── [래스터 패스] 기록
  │     └── G-Buffer: Albedo(투명), Normal(법선), Roughness(0.0)
  │
  └── [RT 패스] 참여
        ├── 자신의 G-Buffer 데이터를 읽음
        ├── RayGen: 반사 방향으로 광선 발사
        └── 결과: 반사된 주변 환경 + 굴절 계산
```

### 4.2 SoftBody Item의 하이브리드 렌더링

```
SoftBody Item (천 재질, HYBRID 모드)
  │
  ├── [1] Compute Pass
  │     └── XPBD로 정점 변형 → Deformed SSBO 기록
  │
  ├── [2] BLAS Update
  │     └── 변형된 정점으로 BLAS 재빌드 (RT 정확도 보장)
  │
  ├── [4] Rasterize Pass
  │     ├── hlms_compute_deform=1 → Vertex Shader가 Deformed SSBO 읽어서 변형 적용
  │     └── G-Buffer에 변형된 천의 표면 색상/법선 기록
  │
  └── [5] RT Pass
        └── 변형된 BLAS 기반으로
            ├── 자기 그림자(Self-Shadow) 계산 → 천이 접힌 부분 정확한 그림자
            └── 주변 오브젝트로부터의 그림자 수신
```

### 4.3 SubMesh 단위 모드 분리 (고급 옵션)

하나의 Mesh가 여러 SubMesh로 구성된 경우, SubMesh별로 다른 Datablock을 가지므로:

```cpp
// 예: 캐릭터 메시
//   SubMesh[0] (피부)  → HYBRID Datablock (피부 산란: 래스터 SSS + RT 그림자)
//   SubMesh[1] (눈)    → RAYTRACING Datablock (눈 굴절/반사)
//   SubMesh[2] (옷감)  → HYBRID Datablock (SoftBody 변형 + RT 자기그림자)

Ogre::Item *character = sceneManager->createItem("Character.mesh", ...);
character->getSubItem(0)->setDatablock( skinDatablock );     // HYBRID
character->getSubItem(1)->setDatablock( eyeDatablock );      // RAYTRACING
character->getSubItem(2)->setDatablock( clothDatablock );    // HYBRID + SoftBody
```

---

## 5. 가속 구조(BLAS/TLAS) 관리 전략

### 5.1 Item 타입별 BLAS 정책

| Item 타입 | BLAS 빌드 시점 | 업데이트 방식 |
| :--- | :--- | :--- |
| 정적 지형/건물 (SCENE_STATIC) | 로딩 시 1회 | 변경 없음 |
| 동적 리지드 오브젝트 (SCENE_DYNAMIC) | 로딩 시 1회 | TLAS 인스턴스 행렬만 갱신 |
| **SoftBody 오브젝트** | 로딩 시 1회 | **매 프레임 BLAS Refit or Rebuild** |

### 5.2 SoftBody BLAS 업데이트 전략

```
매 프레임 순서:
  Compute Shader → Deformed SSBO
       ↓
  [선택 A] BLAS Refit (빠름, 토폴로지 변화 없을 때)
       - vkCmdBuildAccelerationStructuresKHR (UPDATE 모드)
       - 비용: 빌드의 약 10-20%
       - 제한: 정점이 크게 이동하면 RT 품질 저하 가능

  [선택 B] BLAS Full Rebuild (정확, 비용 높음)
       - vkCmdBuildAccelerationStructuresKHR (BUILD 모드)
       - 매 프레임 full rebuild는 성능 병목 유발

  → PRISM 전략:
       - 기본: Refit (매 프레임)
       - 큰 변형 감지 시: Full Rebuild (N 프레임마다)
       - Physics AI 예측값이 변형량을 미리 알 경우: 지능형 선택
```

### 5.3 TLAS 구성

```
TLAS
├── Instance 0: 건물 BLAS (행렬: Identity)
├── Instance 1: 캐릭터 BLAS (행렬: WorldTransform)
└── Instance 2: SoftBody BLAS (행렬: Identity, BLAS 자체가 매 프레임 갱신됨)
```

---

## 6. OGRE NEXT 엔진 수정 계획 (구체적 파일 목록)

### 6.1 Vulkan 렌더러 확장 (`ogre-next/RenderSystems/Vulkan/`)

| 파일 | 수정 내용 |
| :--- | :--- |
| `OgreVulkanDevice.cpp` | `VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure` 확장 활성화 |
| `OgreVulkanQueue.h/cpp` | `EncoderState`에 `EncoderRayTracingOpen` 추가, `getRayTracingEncoder()` 구현 |
| `OgreVulkanRenderSystem.h/cpp` | TLAS 핸들 관리, `vkCmdTraceRaysKHR()` 호출 함수 추가 |
| `OgreVulkanVaoManager.cpp` | `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_...` 플래그 추가, BLAS/TLAS 빌드 함수 |

### 6.2 Compositor 확장 (`ogre-next/OgreMain/`)

| 파일 | 수정 내용 |
| :--- | :--- |
| `OgreCompositorPass.h` | `PASS_RAYTRACING` 타입 추가 |
| `OgreCompositorPassDef.h` | `CompositorPassRayTracingDef` 클래스 정의 |
| `OgreCompositorManager2.cpp` | RT Pass 파싱 및 생성 로직 |

### 6.3 HLMS 확장 (`ogre-next/Components/Hlms/`)

| 파일 | 수정 내용 |
| :--- | :--- |
| `OgreHlmsPbsDatablock.h/cpp` | `PrismRenderMode` 필드 추가 |
| `OgreHlmsPbs.cpp` | `calculateHash()`에서 `prism_render_mode` 프로퍼티 설정 로직 추가 |
| `Pbs/Any/Main/VertexShader_vs.hlms` | `@property(hlms_compute_deform)` 블록 추가 (SoftBody 변형 적용) |

---

## 7. 렌더 큐 분리 전략 (Render Queue)

OGRE의 Render Queue 번호를 활용하여 Item을 패스별로 분리:

```
Render Queue 범위    목적
0   ~ 49           래스터화 전용 (RASTERIZE)
50  ~ 99           하이브리드 래스터 기록 (HYBRID - Rasterize Phase)
100 ~ 149          레이트레이싱 전용 (RAYTRACING) → 래스터 패스에서 Skip
150 ~ 199          하이브리드 RT 참여 (HYBRID - RT Phase)
200+               UI, 오버레이 등

// Compositor의 rq_first / rq_last로 각 패스가 처리할 큐 범위 지정
```

---

## 8. 단계별 구현 로드맵

```
Phase 1 (현재): OGRE NEXT 기본 래스터화 작동 확인
  └── PRISMGameState: Cube 렌더링 ✓

Phase 2: Compute Shader SoftBody 기초
  ├── Compositor에 Compute Pass 추가
  ├── XPBD 기본 Compute Shader 작성
  └── hlms_compute_deform Vertex Shader 수정

Phase 3: RT 인프라 구축
  ├── VulkanDevice RT 확장 활성화
  ├── BLAS/TLAS 빌드 시스템 구현
  └── 기본 RayGen/Hit/Miss Shader 작성

Phase 4: Per-Item 하이브리드 제어
  ├── PrismRenderMode Datablock 필드 추가
  ├── Compositor G-Buffer + RT 합성 패스 구성
  └── Render Queue 분리 전략 적용

Phase 5: SoftBody + Hybrid 완전 통합
  ├── Dynamic BLAS Refit 연동 (매 프레임 Compute → BLAS)
  ├── RT 자기그림자 + 변형 정확도 검증
  └── Physics AI Compute Shader 연동

Phase 6: 최적화
  ├── BLAS 업데이트 지능화 (Refit vs Rebuild 자동 선택)
  ├── Denoising 파이프라인 추가
  └── Per-Item RT 비용 프로파일링
```

---

## 9. 핵심 기술 난제 및 해결 방향

| 난제 | 설명 | 해결 방향 |
| :--- | :--- | :--- |
| **SoftBody BLAS 비용** | 매 프레임 BLAS 재빌드는 RT의 최대 병목 | Refit 우선 + Physics AI로 변형량 예측하여 Rebuild 타이밍 조절 |
| **HLMS와 RT 쉐이더 통합** | HLMS는 래스터용으로 설계됨 | RT 쉐이더는 별도 SBT(Shader Binding Table)로 관리, HLMS Piece는 BRDF 계산 공유 |
| **G-Buffer 정밀도** | SoftBody 변형 후 G-Buffer의 Normal 오차 | Compute Deform 후 Normal 재계산 추가 |
| **RayTrace 전용 Item의 래스터 참여** | RT-Only Item이 그림자/반사 수신 필요 | TLAS에 항상 포함, 래스터 패스에서만 Skip |
| **Physics AI + RT 동기화** | AI 추론 결과 → BLAS 업데이트 순서 | Compute 배리어 체인으로 보장 |

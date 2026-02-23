# P.R.I.S.M 마스터 아키텍처 문서

> 작성일: 2026-02-24
> 목적: 구현 전 전체 구조 확정 및 연구 방향 기준점

---

## 0. 프로젝트 성격 및 현재 단계

```
현재 (Research Phase)                    미래 (Integration Phase)
────────────────────────────────         ───────────────────────
1_JGN (근녕): OGRE NEXT +               → 통합 P.R.I.S.M 엔진
              Hybrid Render +
              SoftBody 연구
                                  합류
2_LSM (수민): Raw Vulkan RT +    ────▶
              Denoising 연구                 (시점 미정)

3_HSH (한승현): Vulkan +         ────▶
                XPBD 물리 구현 연구
                (현재 천 시뮬레이션 구현 진행 중 - 참고용)
```

**이 문서는 1_JGN 도메인의 아키텍처를 확정하는 기준점이다.**

---

## 1. 전체 시스템 레이어

```
┌──────────────────────────────────────────────────────────┐
│  Layer 4 │  Application Layer                            │
│          │  PRISMGameState / Scene Setup / 파라미터 UI   │
├──────────────────────────────────────────────────────────┤
│  Layer 3 │  PRISM Engine Layer                           │
│          │  SoftBodySystem │ HybridRenderer │ ColSystem  │
│          │  (파라미터 기반 연체 제어) │(RT깊이 조절)  │(추후)│
├──────────────────────────────────────────────────────────┤
│  Layer 2 │  OGRE NEXT Extension Layer                    │
│          │  HLMS 확장 │ Compositor 확장 │ VulkanRT 확장  │
├──────────────────────────────────────────────────────────┤
│  Layer 1 │  OGRE NEXT Core                               │
│          │  SceneManager │ VulkanRenderSystem │ HLMS     │
├──────────────────────────────────────────────────────────┤
│  Layer 0 │  Platform                                     │
│          │  Vulkan API │ Windows/Linux │ GPU Hardware     │
└──────────────────────────────────────────────────────────┘
```

---

## 2. 핵심 서브시스템 구조

### 2.1 SoftBody Physics System

**목표**: 어떤 메시(Mesh)든 파라미터 설정만으로 연체(Soft Body) 물성을 갖도록 한다.

```
SoftBodySystem
├── SoftBodyComponent          (Item에 붙는 컴포넌트)
│   ├── SoftBodyMaterial       (물성 파라미터 묶음)
│   │   ├── stiffness          [0.0 ~ 1.0] 강성
│   │   ├── damping            [0.0 ~ 1.0] 감쇠
│   │   ├── density            [kg/m³]     밀도
│   │   ├── poisson_ratio      [0.0 ~ 0.5] 포아송비 (비압축성)
│   │   ├── yield_stress       [Pa]        소성 변형 임계값
│   │   └── sim_method         FEM | XPBD | AI (선택 가능)
│   │
│   └── SoftBodyState          (시뮬레이션 런타임 데이터)
│       ├── position_ssbo      GPU 버퍼: 현재 정점 위치
│       ├── velocity_ssbo      GPU 버퍼: 정점 속도
│       ├── rest_position_ssbo GPU 버퍼: 초기(Rest) 위치
│       ├── constraint_ssbo    GPU 버퍼: 제약 조건 (XPBD용)
│       └── interaction_ssbo   GPU 버퍼: 외부 인터랙션 입력 (핀 고정, 외력 등)
│
├── SimulationDispatcher       (매 프레임 시뮬레이션 Compute Dispatch)
│   ├── FEMSolver              → fem_solve.comp
│   ├── XPBDSolver             → xpbd_solve.comp
│   └── AISolver               → ai_infer.comp (TBD)
│
└── BLASManager                (RT 가속 구조 관리)
    ├── markDirty(item)        변형 발생 시 재빌드 플래그
    ├── refit(blas)            경미한 변형 → Refit (빠름)
    └── rebuild(blas)          큰 변형 → Full Rebuild (정확)
```

**시뮬레이션 방법론 선택 기준:**

| 방법 | 적합한 상황 | 특징 |
| :--- | :--- | :--- |
| **FEM** | 볼류메트릭 변형 (근육/살) 고정밀 필요 시 | 사면체(Tet) 메시 필요, 가장 물리적으로 정확 |
| **XPBD** | 실시간 처리 우선, 탄성 거동이 주 | 표면 메시로도 가능, 빠르고 안정적 |
| **AI** | 반복적/예측 가능한 변형 패턴 최적화 | 오프라인 학습 필요, Compute에서 추론 (TBD) |

> **FEM 전제 조건**: 표면 메시(Surface Mesh)를 볼류메트릭 사면체 메시(Tet Mesh)로 변환하는 **Tetrahedralization** 파이프라인 필요 → 연구 필요 항목

**SSBO 데이터 정렬 규칙 (GPU 필수 사항):**

GPU의 SSBO/UBO는 **16바이트(vec4) 단위 정렬**을 요구한다. 어긋나면 잘못된 데이터를 읽거나 크래시 발생.

```glsl
// 올바른 정점 구조체 설계 (16바이트 배수)
struct SoftBodyVertex {
    vec4 position;       // xyz + padding(w) → 16바이트
    vec4 velocity;       // xyz + padding(w) → 16바이트
    vec4 normal;         // xyz + padding(w) → 16바이트
    vec4 rest_position;  // xyz + padding(w) → 16바이트
};  // 총 64바이트: 16바이트의 배수 ✓

// 잘못된 예 (GPU에서 데이터 오정렬 발생)
struct BadVertex {
    vec3 position;  // 12바이트 → 다음 필드 오정렬 ✗
    float mass;     // 4바이트
    vec3 velocity;  // 12바이트 → vec4 경계 위반 ✗
};

// C++ 측에서도 동일한 패딩 구조로 맞춰야 함
struct SoftBodyVertexCPU {
    float pos[3];  float pad0;   // 16바이트
    float vel[3];  float pad1;   // 16바이트
    float nor[3];  float pad2;   // 16바이트
    float rest[3]; float pad3;   // 16바이트
};
```

---

### 2.2 Hybrid Renderer

**목표**: Item별 / SubMesh별로 렌더링 방식을 HLMS 기반으로 제어. RT 깊이를 엔진 레벨에서 조절 가능.

```
HybridRenderer
├── RenderModeSystem
│   ├── PrismRenderMode        RASTERIZE | RAYTRACING | HYBRID
│   └── RT 깊이(rt_depth)      0 = RT 없음
│                              1 = 직접광 그림자만 RT
│                              2 = + 반사
│                              3 = + 굴절
│                              N = 완전 Path Tracing
│
├── CompositorGraph            (프레임 패스 순서)
│   ├── [1] ComputePass        SoftBody 물리 연산
│   ├── [2] BLASUpdatePass     동적 오브젝트 가속 구조 갱신    (커스텀)
│   ├── [3] TLASUpdatePass     씬 전체 TLAS 업데이트           (커스텀)
│   ├── [4] RasterizePass      G-Buffer 기록 (RASTERIZE+HYBRID)
│   ├── [5] RayTracingPass     RT 효과 계산 (RAYTRACING+HYBRID)(커스텀)
│   ├── [6] CompositePass      G-Buffer + RT 결과 합성
│   └── [7] PostProcessPass    Denoising, TAA, ToneMapping
│
└── PerItemControl             (아이템 단위 제어)
    ├── RenderQueue 분리       0~49: Rasterize / 50~99: Hybrid / 100~149: RT
    └── SubMesh 단위 모드      각 SubMesh의 Datablock으로 독립 제어
```

**RT 깊이 조절 메커니즘:**

RT 쉐이더 내부는 **재귀(recursion) 방식이 아닌 for loop + depth 방식**으로 구현한다.
- 재귀: 스택 메모리 소모, context switch 비용 발생, GPU 쉐이더에서 사실상 지원 안 됨
- for loop: depth 수치만큼 반복하며 레이를 순차 발사, 결과를 누적 → 메모리 효율적이고 종료 조건 명확

```
HlmsDatablock
  └── prism_rt_depth = N
        │
        ▼
calculateHash() 에서 분석
  ├── N == 0 → 일반 래스터 쉐이더 생성 (RT 코드 없음)
  ├── N == 1 → RT Shadow Encoder 활성화    (for loop 1회: 직접광 그림자)
  ├── N == 2 → + RT Reflection Encoder     (for loop 2회: + 반사)
  ├── N == 3 → + RT Refraction Encoder     (for loop 3회: + 굴절)
  └── N >= 4 → Full Path Tracing           (for loop N회: 완전 경로 추적)

RT Shader 내부 구조 (의사코드):
  vec3 radiance = vec3(0);
  Ray  ray      = generatePrimaryRay();

  for (int depth = 0; depth < prism_rt_depth; depth++) {
      HitInfo hit = traceRay(TLAS, ray);
      if (!hit.valid) { radiance += sampleSkybox(ray); break; }
      radiance += hit.emission + BRDF(hit) * throughput;
      throughput *= hit.albedo;
      ray = scatterRay(hit);   // 다음 반사/굴절 방향 결정
  }
```

---

### 2.3 HLMS Extension (확장 전략 - 연구 후 확정)

**현황**: HLMS를 직접 써본 뒤 전략 결정 예정. 세 가지 후보:

```
[A] HlmsPbs 직접 수정
    장점: 단순, 빠른 프로토타입
    단점: OGRE 원본 오염, 업스트림 반영 어려움
    적합: 빠른 실험 단계

[B] HlmsPbs 상속 → HlmsPrism 신규 제작  ← 장기 방향으로 유력
    장점: OGRE 원본 보존, 엔진다운 구조
    단점: 초기 보일러플레이트 많음
    적합: 안정적인 엔진 구조가 필요할 때

[C] HlmsListener (Hook 방식)
    장점: OGRE 공식 확장 포인트, 가장 비침투적
    단점: 할 수 있는 범위가 제한적 (RT 쉐이더 완전 통합에 한계)
    적합: 소규모 프로퍼티 주입에는 최적

→ 연구 순서: C로 시작해서 한계 확인 → 필요시 B로 전환
```

**HLMS 연구 체크리스트 (실습 전에 파악해야 할 것들):**
- [ ] `HlmsListener::preparePassHash()` 로 어디까지 커스터마이징 가능한지
- [ ] RT 전용 쉐이더(RayGen/Hit/Miss)를 HLMS 파이프라인에 연결하는 방법
- [ ] `HlmsDatablock` 커스텀 필드 추가 방법
- [ ] `.hlms` Piece 파일에서 `@property` 조건분기 직접 실습

---

### 2.4 Vulkan Extension Layer

OGRE NEXT Vulkan 렌더러에 추가해야 할 확장들:

```
VulkanExtensionLayer
├── RT 인프라
│   ├── VK_KHR_ray_tracing_pipeline
│   ├── VK_KHR_acceleration_structure
│   └── VK_KHR_deferred_host_operations
│
├── 추가 클래스/함수
│   ├── VulkanQueue::getRayTracingEncoder()
│   ├── VulkanDevice::buildBLAS(mesh)
│   ├── VulkanDevice::buildTLAS(scene)
│   └── VulkanRenderSystem::traceRays()
│
└── Compute 관련
    ├── VulkanQueue::getComputeEncoder()  (이미 OGRE에 있음, 확인 필요)
    └── SSBO 더블 버퍼링 구조 (물리 연산 중 렌더링 충돌 방지)
```

---

## 3. 데이터 흐름 (한 프레임)

```
CPU (Logic Thread)
  └── SoftBodySystem::update(dt)
        ├── 파라미터 변경 감지
        └── Dispatch 명령 큐에 추가

GPU (Graphics Thread)
  │
  ▼
[Compute Pass - SoftBody]
  Input:  rest_position_ssbo, constraint_ssbo, velocity_ssbo
  Shader: xpbd_solve.comp / fem_solve.comp / ai_infer.comp
  Output: position_ssbo (Deformed) ──────────────────────────┐
  Barrier: SSBO Write → Vertex Read / AS Build Read          │
  │                                                          │
  ▼                                                          ▼
[BLAS Update Pass]                              [Rasterize Pass - G-Buffer]
  Input:  position_ssbo (Deformed)               Input:  position_ssbo (hlms_compute_deform=1)
  Action: vkCmdBuildAccelerationStructures        Output: GBuffer (Position/Normal/Albedo/Roughness)
  Output: 갱신된 BLAS ──────────┐
  │                             │
  ▼                             ▼
[TLAS Update Pass]         [Ray Tracing Pass]   ← TLAS + GBuffer 참조
  Output: 갱신된 TLAS ──────▶  Shaders: RayGen, ClosestHit, Miss
                               Output:  RT_Result (반사/그림자/굴절)
                                │
                                ▼
                          [Composite Pass]
                            GBuffer.Albedo + RT_Result → Final Image
                                │
                                ▼
                          [Post Process]
                            Denoising + TAA + ToneMapping
```

---

## 4. 외부 인터랙션 인터페이스

**목표**: GPU에서 돌아가는 물리 시뮬레이션에 외부 입력(마우스, 키보드, 충돌 이벤트 등)이 실시간으로 반영될 수 있는 구조. 시뮬레이터이자 엔진이기 때문에 인터랙티브 제어가 필수.

### 4.1 구조 원칙

CPU는 시뮬레이션 자체를 건드리지 않는다. 대신 **`interaction_ssbo`를 통해 GPU에 명령을 전달**하고, Compute Shader가 이를 읽어 물리 연산에 반영한다.

```
CPU (Logic Thread)                         GPU (Compute Shader)
─────────────────                          ────────────────────
사용자 입력 감지                             매 프레임 시뮬레이션 실행 중
  (마우스 클릭/드래그,                        ↑
   키보드, 외부 API 호출 등)                  │ 읽기
       │                                    │
       ▼                                    │
interaction_ssbo 갱신  ─────────────────▶  interaction_ssbo
  (Vulkan Fence/Semaphore 동기화)             ├── pin[i].nodeIndex    핀 고정 노드
                                             ├── pin[i].mass         → INF 설정 시 고정
                                             ├── force[i].position   외력 적용 위치
                                             ├── force[i].direction  외력 방향/크기
                                             └── force[i].radius     영향 반경
```

### 4.2 인터랙션 버퍼 구조

```glsl
// interaction_ssbo 레이아웃 (16바이트 정렬 준수)
struct PinConstraint {
    int   nodeIndex;     // 고정할 노드 인덱스
    float mass_override; // 0 = 원래 질량, INF = 완전 고정
    float pad0, pad1;    // 16바이트 패딩
};

struct ExternalForce {
    vec4 position;       // xyz: 적용 위치,  w: 반경(radius)
    vec4 direction;      // xyz: 힘 방향,    w: 크기(magnitude)
};

struct InteractionBuffer {
    int            pinCount;        // 활성 핀 수
    int            forceCount;      // 활성 외력 수
    float          pad0, pad1;
    PinConstraint  pins[MAX_PINS];
    ExternalForce  forces[MAX_FORCES];
};
```

### 4.3 인터랙션 타입

| 인터랙션 | 구현 방식 | 예시 |
| :--- | :--- | :--- |
| **노드 핀 고정** | 노드 mass → INF, 속도 = 0 | 마우스로 연체 한 점 잡기 |
| **노드 드래그** | 핀 고정 + 매 프레임 position 목표값 갱신 | 마우스로 연체 끌어당기기 |
| **점 외력 적용** | force 벡터를 반경 내 모든 노드에 분배 | 바람, 폭발, 충격 효과 |
| **파라미터 실시간 변경** | UBO로 stiffness/damping 값 갱신 | 슬라이더로 탄성 조절 |

### 4.4 CPU→GPU 전달 동기화

```
CPU가 interaction_ssbo 수정
       │
       ▼
vkCmdPipelineBarrier
  srcStage: HOST_WRITE
  dstStage: COMPUTE_SHADER_READ
       │
       ▼
Compute Pass 시작 → Shader에서 interaction_ssbo 읽기
```

> 매 프레임 전체 버퍼를 올리지 않고, **변경된 항목만 Dirty Flag 방식으로 업데이트**하여 오버헤드 최소화.

---

## 5. 충돌 시스템 (추후 구현 예정)

> **현재는 설계 방향만 메모. Phase 5 이후 구체화.**
> 참고: 3_HSH 연구에서 단순한 형태의 충돌(구 ↔ 노드 거리 기반 밀어내기)부터 시작하는 접근을 확인. 우리도 동일한 순서 채택.

```
CollisionSystem (미래, 단순 → 복잡 순서로 구현)
│
├── Step 1: 구(Sphere) ↔ SoftBody 노드
│   └── 노드-구 중심 거리 < 반지름 → 법선 방향으로 밀어내기
│       (가장 단순한 형태, 우선 구현 대상)
│
├── Step 2: RigidBody(AABB/OBB) ↔ SoftBody
│   └── 접촉점 계산 → ExternalForce 인터페이스로 주입
│       (야구방망이 ↔ 젤리: 충격량 = 외력으로 변환)
│
├── Step 3: SoftBody ↔ SoftBody
│   └── 질량/스티프니스 반영한 상호작용 계산
│       (복잡도 높음, 마지막 구현)
│
├── Broad Phase
│   └── BLAS/TLAS 재활용 가능성 검토
│       (RT용 가속 구조를 충돌 감지 BVH로도 활용 - 연구 필요)
│
└── 충돌 결과 → 인터랙션 인터페이스로 주입
    └── applyExternalForce(position, impulse, radius)
        → interaction_ssbo의 forces 배열에 기록
```

---

## 5. 확정된 설계 결정 (Confirmed)

| # | 결정 사항 | 이유 |
| :--- | :--- | :--- |
| 1 | Vulkan + OGRE NEXT 기반 | 엔진 구조 학습 + 고성능 렌더링 |
| 2 | Per-Item 렌더 모드 (RASTERIZE/RT/HYBRID) | 오브젝트별 품질/성능 트레이드오프 제어 |
| 3 | RT 깊이 0~N 단계 조절 (HLMS Property) | 엔진 레벨에서 품질 제어 가능 |
| 4 | **RT 내부 구현: for loop + depth (재귀 X)** | GPU 스택 한계, 메모리 효율, 종료 조건 명확 |
| 5 | Compute Shader 기반 물리 (GPU) | 실시간 연산 필수 |
| 6 | 볼류메트릭 연체 타겟 (근육/살/젤리) | 프로젝트 방향 |
| 7 | 파라미터 기반 물성 제어 (SoftBodyMaterial) | 어떤 메시든 연체화 가능하게 |
| 8 | FEM + XPBD + AI 세 방식 병행 지원 | 정밀도/성능/상황별 선택 |
| 9 | BLAS Refit 우선, 큰 변형 시 Rebuild | SoftBody RT 성능 병목 해소 |
| 10 | **SSBO 16바이트(vec4) 단위 정렬** | GPU 메모리 접근 규칙, 미준수 시 데이터 오류 |
| 11 | **interaction_ssbo 기반 인터랙션 구조** | GPU 시뮬레이션에 외부 입력 반영, Dirty Flag 방식 |
| 12 | 충돌: 구↔노드 단순 형태부터 시작 | 복잡도 단계적 확장 |
| 13 | 팀 연구 독립 단계 → 추후 통합 | 현재 도메인 연구 단계 |

---

## 6. 연구 필요 항목 (TBD / Open Questions)

| # | 항목 | 왜 미확정인가 | 연구 방향 |
| :--- | :--- | :--- | :--- |
| R1 | **HLMS 확장 전략** (A/B/C) | 직접 써봐야 한계 파악 가능 | Listener 실습 → 필요시 상속 전환 |
| R2 | **Physics AI 모델 구조** | AI 추론을 Compute에서 돌리는 방법 조사 필요 | ONNX→SPIR-V 변환 가능성 / Slang 활용 검토 |
| R3 | **Tetrahedralization** | FEM을 위한 Tet Mesh 생성 파이프라인 | TetGen, fTetWild 등 라이브러리 조사 |
| R4 | **Denoising 방식** | RT 1spp 노이즈 처리 방법 결정 안 됨 | SVGF, DLSS RR, 자체 TAA 비교 |
| R5 | **SSBO 더블 버퍼링 설계** | 물리 연산 중 렌더링 데이터 레이스 방지 구조 | Ogre Vulkan Queue 배리어 패턴 분석 |
| R6 | **SoftBody ↔ Rigid 충돌** | Phase 5 이후 설계 예정 | 우선 BLAS 재활용 가능성 조사 |
| R7 | **RT 쉐이더와 HLMS 통합** | RayGen/Hit/Miss를 HLMS Piece로 관리 가능한지 | OGRE NEXT SBT 관리 구조 분석 |

---

## 7. 문서 맵

```
PRISM_Architecture_Master.md   ← 현재 파일 (전체 구조 기준점)
│
├── Ogre_Core_Concepts.md          OGRE 아키텍처 기초
├── HLMS_Deep_Dive.md              HLMS 시스템 심층 분석
├── Vulkan_Architecture_Guide.md   Vulkan 렌더러 구조
├── Hybrid_Rendering_Architecture.md  Per-Item 하이브리드 설계
└── PRISM_Project_Overview.md      프로젝트 전체 개요
```

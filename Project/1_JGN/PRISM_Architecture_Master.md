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

### 2.1.1 Physics Timestep & Sub-stepping

> **연구 담당: 3_HSH (참고용)** — 설계 결과 확인 후 1_JGN 통합 시 반영 예정

**XPBD는 timestep 독립적이다** — PBD와의 결정적 차이.

PBD는 stiffness가 timestep에 종속되어 timestep이 바뀌면 물체가 다르게 움직인다.
XPBD는 **Compliance(α)** 파라미터로 stiffness를 timestep과 분리하여 물리적으로 일관된 결과를 보장한다.

```
PBD:  stiffness = f(iteration 횟수, timestep)   → timestep 바뀌면 물성도 바뀜 ✗
XPBD: stiffness = f(compliance α)               → timestep 독립적 ✓

XPBD Sub-stepping 구조:
  renderDt = 1/60s  (렌더 프레임 간격)
  substeps  = N     (설정값, 클수록 정확)
  physicsDt = renderDt / N

  per render frame:
    for (int i = 0; i < N; i++):
        xpbd_solve(physicsDt)   // Compute Dispatch
    interpolate_for_render()    // 렌더용 위치 보간
```

**FEM / AI와의 통합 timestep 정책** (추후 구체화 필요):
- XPBD: sub-step 기반으로 자연스럽게 처리
- FEM: 별도 안정 조건(CFL condition) 기반 timestep 상한 존재 → 별도 전략 필요
- AI: 학습 시 사용한 timestep과 추론 시 timestep을 일치시켜야 정확도 유지

---

### 2.1.2 수치 안정성 (Numerical Stability)

> **연구 담당: 3_HSH (참고용)** — 설계 결과 확인 후 1_JGN 통합 시 반영 예정

**XPBD는 PBD 대비 수치 안정성이 근본적으로 높다.**

PBD에서 stiffness를 높이려면 iteration을 늘려야 하고, 이 과정에서 over-correction → 진동 → 폭발이 발생하기 쉬웠다.
XPBD는 compliance α → 0(rigid)으로 줄여도 constraint correction이 timestep 단위로 분산되어 폭발 조건이 훨씬 엄격해진다.

```
방법별 안정성 비교:

XPBD  ████████░░  높음  — compliance 기반, sub-step으로 추가 제어 가능
FEM   █████░░░░░  중간  — CFL 조건 위반 시 발산. stiffness matrix 조건수 문제
AI    ████████░░  높음  — 학습 데이터 범위 내에서는 안정적, 범위 밖은 미보장
```

**XPBD 안정성 제어 파라미터:**
- `compliance α` : 작을수록 rigid. 0에 가까울수록 더 많은 sub-step 필요
- `substep N`    : 클수록 안정, 비용 증가
- `iteration`    : constraint solver 반복 횟수, 수렴 속도와 트레이드오프

**FEM의 별도 안정성 전략** (추후 구체화 필요):
- CFL(Courant–Friedrichs–Lewy) 조건: `physicsDt < 최소_요소_크기 / 파동_속도`
- 조건수가 높은 stiffness matrix → preconditioning 기법 필요
- 3_HSH 연구 결과 반영 예정

---

### 2.1.3 SoftBody LOD 시스템

**목표**: 씬 스케일 확장을 위한 핵심 최적화. 카메라 거리에 따라 시뮬레이션 해상도와 렌더링 품질을 단계적으로 조절.

```
SoftBodyLOD
├── LOD 레벨 정의
│   ├── LOD 0 (Near)    풀 해상도 노드 + FEM/XPBD + RT Hybrid
│   ├── LOD 1 (Mid)     노드 수 감소(Decimated) + XPBD만 + Rasterize
│   ├── LOD 2 (Far)     최소 노드 + 단순 탄성 근사 + Rasterize
│   └── LOD 3 (Cull)    시뮬레이션 정지 + Static Mesh로 대체
│
├── 적용 범위 (렌더링 + 물리 동시 조절)
│   ├── Compute Dispatch 해상도  → LOD에 따라 노드 수 다른 SSBO 사용
│   ├── BLAS 갱신 주기            → LOD 0: 매 프레임 / LOD 2: N 프레임마다
│   ├── RT 깊이(prism_rt_depth)  → LOD 0: N / LOD 1: 1 / LOD 2+: 0
│   └── Constraint iteration 수  → LOD 낮을수록 반복 횟수 감소
│
└── 전환 조건
    ├── 카메라 거리 임계값 (설정 가능)
    ├── 화면 점유 픽셀 수 기반 전환 (더 정확)
    └── 전환 시 Hysteresis (LOD 0→1 거리 ≠ LOD 1→0 거리, 깜빡임 방지)
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
│   ├── [7] PostProcessPass    Denoising, TAA, ToneMapping
│   └── [8] DebugPass          노드/제약/외력/RT레이 시각화  (추후 논의 예정)
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
├── Compute 관련
│   ├── VulkanQueue::getComputeEncoder()  (이미 OGRE에 있음, 확인 필요)
│   └── SSBO 더블 버퍼링 구조 (물리 연산 중 렌더링 충돌 방지)
│
└── Async Compute Queue  [ 연구 예정 ]
    ├── Graphics Queue와 Compute Queue 병렬 실행 가능 여부
    ├── 물리(Compute)와 렌더링(Graphics)을 다른 큐에서 겹쳐 실행 시 GPU 활용률 향상
    └── OGRE NEXT가 Multi-Queue를 어디까지 지원하는지 확인 필요
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
| 13 | **SoftBody LOD: 렌더링 + 물리 동시 조절** | 씬 스케일 확장 위한 핵심 최적화 |
| 14 | **Physics AI: Isaac SIM + FEM 데이터 → 학습 → Compute 포팅** | 학습 파이프라인 방향 확정 |
| 15 | **Timestep/안정성: 3_HSH 연구 참고 후 통합** | HSH가 연구 중, 결과 확인 후 반영 |
| 16 | 팀 연구 독립 단계 → 추후 통합 | 현재 도메인 연구 단계 |

---

## 6. 연구 필요 항목 (TBD / Open Questions)

| # | 항목 | 왜 미확정인가 | 연구 방향 |
| :--- | :--- | :--- | :--- |
| R1 | **HLMS 확장 전략** (A/B/C) | 직접 써봐야 한계 파악 가능 | Listener 실습 → 필요시 상속 전환 |
| R2 | **Physics AI 학습 파이프라인** | 모델 학습 및 Compute 포팅 방법 확정 필요 | Isaac SIM + FEM 시뮬레이션으로 데이터 수집 → 학습 → ONNX → SPIR-V / Slang으로 Compute Shader 포팅 |
| R3 | **Tetrahedralization** | FEM을 위한 Tet Mesh 생성 파이프라인 | TetGen, fTetWild 등 라이브러리 조사. 런타임 vs 전처리 시점 결정 필요 (→ R8 Asset Pipeline과 연동) |
| R4 | **Denoising 방식** | RT 1spp + SoftBody 이동 시 Ghosting 문제 | SVGF, DLSS RR, 자체 TAA 비교. 움직이는 연체에서의 Temporal Reprojection 전략 별도 검토 필요 |
| R5 | **SSBO 더블 버퍼링 설계** | 물리 연산 중 렌더링 데이터 레이스 방지 구조 | Ogre Vulkan Queue 배리어 패턴 분석 |
| R6 | **SoftBody ↔ Rigid 충돌** | Phase 5 이후 설계 예정 | 구↔노드 단순 형태부터. BLAS 충돌 BVH 재활용 가능성 조사 |
| R7 | **RT 쉐이더와 HLMS 통합** | RayGen/Hit/Miss를 HLMS Piece로 관리 가능한지 | OGRE NEXT SBT 관리 구조 분석 |
| R8 | **Asset Pipeline** | 메시 포맷, Tet Mesh 전처리 시점 미결정 | 런타임 변환 vs 오프라인 전처리 툴 비교. 추후 연구 예정 |
| R9 | **Async Compute Queue** | OGRE NEXT Multi-Queue 지원 범위 미파악 | Vulkan Async Compute 구조 분석. 추후 연구 예정 |
| R10 | **Debug Visualization 시스템** | Compositor Debug Pass 설계 미정 | 노드/제약/RT레이 시각화 방법. 추후 논의 예정 |

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

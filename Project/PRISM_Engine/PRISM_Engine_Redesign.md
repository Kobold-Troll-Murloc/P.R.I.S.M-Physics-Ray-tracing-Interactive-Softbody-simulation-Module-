# PRISM Engine Redesign Plan
> 작성일: 2026-03-12
> 방향: OGRE 제거 → 목적 특화 최소 엔진 (방향 C)

---

## 1. 왜 재설계인가

### 현재 구조의 문제

| 문제 | 원인 |
|:--|:--|
| OGRE 소스를 직접 수정해야 RT 확장 활성화 가능 | OGRE가 RT를 모름 |
| `endAllEncoders()` 타이밍 수동 관리 | OGRE 인코더와 Raw Vulkan 충돌 |
| PASS_SCENE (래스터)가 매 프레임 실행되지만 RT로 덮어써짐 | 낭비 |
| Application 코드가 `VkBuffer`, `VkAccelerationStructure`를 직접 앎 | 추상화 부재 |
| 물리(XPBD)와 렌더링 연동 방법이 미정 | 통합 설계 없음 |

### 목표

> **PRISM = 연구용 Softbody + Path Tracing 렌더러**
>
> Unity/Unreal이 아직 못하는 영역을 제대로 설계된 엔진으로 구현한다.
> 범용 엔진이 아닌 목적 특화 최소 엔진.

---

## 2. 사용할 외부 라이브러리

OGRE가 해주던 역할을 작고 명확한 라이브러리로 대체.

| 역할 | 라이브러리 | 비고 |
|:--|:--|:--|
| Vulkan 초기화 (Instance/Device/Swapchain) | **vk-bootstrap** | 헤더 1개, ~300줄 |
| GPU 메모리 관리 | **VMA** (Vulkan Memory Allocator) | AMD 공식, 업계 표준 |
| 수학 (벡터/행렬/쿼터니언) | **GLM** | 이미 사용 중 |
| 윈도우 / 입력 | **SDL2** | 이미 사용 중 |
| 메시 로딩 | 자체 OBJ 파서 | 이미 있음 (ParseObj) |
| 디버그 UI | **Dear ImGui** | 선택사항, 추후 추가 |

OGRE 의존성 완전 제거.

---

## 3. 새 아키텍처

```
┌─────────────────────────────────────────────────────┐
│  Application Layer                                  │
│  scene->createObject(), obj->setMaterial() 수준     │
│  Vulkan, Barrier, BLAS 같은 개념 노출 없음          │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│  PRISM Engine Core                                  │
│  ├── SceneManager   오브젝트, 카메라, 조명           │
│  ├── PhysicsSystem  XPBD + FEM (Compute Shader)     │
│  └── RenderSystem   Render Graph에 패스 등록         │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│  Render Graph (핵심)                                │
│  ├── ComputePass    물리 시뮬레이션 (XPBD)          │
│  ├── TLASBuildPass  BLAS Refit → TLAS 갱신          │
│  ├── RTPass         Path Tracing (raygen/hit/miss)  │
│  ├── RasterPass     선택적 래스터라이즈              │
│  └── PostPass       Tone Mapping, Denoising         │
│                                                     │
│  → 패스 순서 자동 결정                              │
│  → VkImageMemoryBarrier 자동 삽입                   │
│  → 사용 안 하는 패스 자동 컬링                      │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│  Vulkan Backend (Platform Layer)                    │
│  vk-bootstrap + VMA + 직접 구현                     │
│  버퍼, 이미지, 커맨드버퍼, 동기화                   │
└─────────────────────────────────────────────────────┘
```

---

## 4. 폴더 구조

```
PRISM_Engine/
├── src/
│   ├── main.cpp                    ← Application (씬 기술만)
│   │
│   ├── core/
│   │   ├── Engine.h/.cpp           ← 엔진 초기화, 메인 루프
│   │   ├── SceneManager.h/.cpp     ← 오브젝트, 카메라, 조명
│   │   └── PhysicsSystem.h/.cpp    ← XPBD/FEM Compute Shader 관리
│   │
│   ├── renderer/
│   │   ├── RenderGraph.h/.cpp      ← 핵심: DAG + 배리어 자동화
│   │   ├── RenderPass.h            ← 패스 인터페이스 정의
│   │   ├── RTPass.h/.cpp           ← Path Tracing 패스
│   │   ├── ComputePass.h/.cpp      ← 물리 시뮬레이션 패스
│   │   ├── TLASBuildPass.h/.cpp    ← BLAS Refit + TLAS 갱신
│   │   └── PostPass.h/.cpp         ← Tone Mapping 등
│   │
│   ├── vulkan/
│   │   ├── VulkanContext.h/.cpp    ← vk-bootstrap 래핑, Device/Queue
│   │   ├── VulkanBuffer.h/.cpp     ← VMA 기반 버퍼 관리
│   │   ├── VulkanImage.h/.cpp      ← 이미지, 레이아웃 전환
│   │   ├── VulkanSwapchain.h/.cpp  ← 스왑체인
│   │   └── VulkanRT.h/.cpp         ← BLAS/TLAS/SBT (현 PrismRTPipeline 재정리)
│   │
│   └── asset/
│       ├── MeshLoader.h/.cpp       ← OBJ 파서 (현재 ParseObj 이동)
│       └── MaterialSystem.h/.cpp   ← 재질 정의 (albedo, roughness 등)
│
└── shaders/                        ← 그대로 유지
    ├── raygenbsdf.rgen
    ├── closesthitbsdf.rchit
    └── ...
```

---

## 5. Render Graph 설계

### 핵심 개념

```cpp
// 사용자 (Application)가 선언하는 방식:
auto depth   = graph.createImage("depth",   VK_FORMAT_D32_SFLOAT);
auto rtOut   = graph.createImage("rt_out",  VK_FORMAT_R32G32B32A32_SFLOAT);
auto accum   = graph.createImage("accum",   VK_FORMAT_R32G32B32A32_SFLOAT);

graph.addPass("Physics", PassType::Compute)
    .readwrite(positionSSBO)
    .execute([](CommandBuffer& cmd) { /* vkCmdDispatch */ });

graph.addPass("TLASBuild", PassType::Compute)
    .reads(positionSSBO)
    .execute([](CommandBuffer& cmd) { /* BLAS Refit + TLAS */ });

graph.addPass("RayTracing", PassType::RayTracing)
    .reads(tlas)
    .writes(rtOut, accum)
    .execute([](CommandBuffer& cmd) { /* vkCmdTraceRaysKHR */ });

graph.addPass("PostProcess", PassType::Compute)
    .reads(rtOut)
    .writes(swapchain)
    .execute([](CommandBuffer& cmd) { /* Tone Mapping */ });

// Render Graph가 알아서:
graph.compile();  // DAG 분석 → Barrier 삽입 위치 결정
graph.execute();  // 순서대로 실행
```

### 내부 구현 원리

```
1. compile() 단계:
   - 패스들을 DAG로 구성 (read/write 의존성 분석)
   - 위상 정렬 (실행 순서 결정)
   - 리소스별 상태 전환 추적
     (UNDEFINED → GENERAL → SHADER_READ 등)
   - 필요한 위치에 VkImageMemoryBarrier / VkBufferMemoryBarrier 삽입

2. execute() 단계:
   - CommandBuffer에 순서대로 기록
   - 배리어 자동 삽입
   - 각 패스의 execute 람다 호출
```

---

## 6. 물리-렌더링 연동 (PRISM의 핵심)

Softbody 시뮬레이션이 렌더링과 연동되는 방식:

```
매 프레임:

[Compute Pass] XPBD 시뮬레이션
  positionSSBO (정점 위치) 갱신

[Compute Pass] BLAS Refit
  갱신된 positionSSBO → BLAS 업데이트
  (VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)

[Compute Pass] TLAS 재빌드
  BLAS → TLAS 갱신

[RT Pass] Path Tracing
  최신 TLAS로 광선 추적
  변형된 메시가 정확하게 렌더링됨
```

Render Graph가 이 의존성을 자동으로 배리어로 변환.

---

## 7. 구현 단계 (Phase별 로드맵)

### Phase 0: Vulkan Backend 교체 (OGRE 제거)
- [ ] vk-bootstrap으로 Vulkan 초기화
- [ ] VMA로 메모리 관리
- [ ] SDL2 윈도우 유지
- [ ] 현재 Cornell Box 씬이 동일하게 동작하는 것 확인

### Phase 1: Render Graph 구현
- [ ] RenderGraph 클래스 설계 (DAG, 위상 정렬)
- [ ] 리소스 상태 추적 (ImageLayout, Buffer 접근 패턴)
- [ ] 배리어 자동 삽입
- [ ] 현재 RT 패스를 Render Graph 위에서 동작하도록 이식

### Phase 2: Engine Core 정리
- [ ] SceneManager (오브젝트 추가/삭제, 카메라)
- [ ] MaterialSystem (재질 파라미터 추상화)
- [ ] MeshLoader (현 OBJ 파서 모듈화)
- [ ] Application 코드에서 Vulkan 개념 제거

### Phase 3: 물리 시스템 통합 (HSH 연동)
- [ ] PhysicsSystem 인터페이스 정의
- [ ] XPBD Compute Shader 통합 (HSH 코드 이식)
- [ ] BLAS Refit 연동
- [ ] Render Graph에 물리 패스 추가

### Phase 4: 고도화 (선택)
- [ ] FEM 사면체 시뮬레이션
- [ ] Denoising (SVGF 또는 간단한 TAA)
- [ ] Physics AI (ONNX → SPIR-V Compute)
- [ ] Dear ImGui 디버그 UI

---

## 8. 현재 코드 재활용 계획

버릴 것 vs 살릴 것:

| 현재 코드 | 처리 | 이유 |
|:--|:--|:--|
| `PrismRTPipeline.h/.cpp` | **살림** (vulkan/VulkanRT로 이동) | RT 파이프라인 로직 유효 |
| `ParseObj()` | **살림** (asset/MeshLoader로 이동) | 잘 동작함 |
| 셰이더 전체 `.rgen/.rchit/.rmiss` | **살림** | 변경 없음 |
| OGRE 관련 include/코드 전체 | **제거** | 의존성 제거 |
| `PrismCompositorPass` | **제거** | OGRE 없으면 불필요 |
| `setupScene()` 씬 구성 로직 | **이동** → main.cpp 간소화 | Application 레이어로 |

---

## 9. 팀 통합 계획

| 팀 | 현재 코드 | PRISM 엔진에서의 위치 |
|:--|:--|:--|
| **1_JGN (나)** | RT 파이프라인, 씬 구성 | vulkan/VulkanRT + renderer/RTPass |
| **2_LSM** | Raw Vulkan 하이브리드 파이프라인 | renderer/RasterPass 레퍼런스 |
| **3_HSH** | XPBD/FEM Compute Shader | core/PhysicsSystem + renderer/ComputePass |

---

## 10. 핵심 원칙

1. **Application 코드는 Vulkan을 몰라야 한다**
   - `VkBuffer`, `VkImage`, `VkAccelerationStructure` 등 노출 금지

2. **Render Graph가 배리어를 소유한다**
   - 수동 `vkCmdPipelineBarrier` 호출 금지 (Render Graph 밖에서)

3. **물리와 렌더링은 같은 GPU 타임라인에서 돌아간다**
   - Compute → RT 순서를 Render Graph가 보장

4. **최소한으로 만든다**
   - 필요 없는 기능은 추가하지 않는다
   - 에디터, 오디오, 스크립팅 없음

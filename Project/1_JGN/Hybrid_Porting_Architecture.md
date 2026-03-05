# LSM HybridPipeLine → OGRE NEXT 이식 아키텍처 설계

## Context

LSM의 `Vulkan_HybridPipeLine_Sample` (Raw Vulkan, 단일파일 ~3000줄)을
`D:/Git/P.R.I.S.M/Project/PRISM_Engine/` (OGRE NEXT 기반 통합 개조 엔진)에 이식하기 위한 구조 설계.

**문제**: OGRE NEXT 3.0은 Ray Tracing 지원이 전혀 없음.
RT 패스, BLAS/TLAS, SBT, RT 파이프라인을 모두 커스텀으로 주입해야 함.

**목표**: LSM의 5단계 하이브리드 파이프라인을 OGRE의 Compositor 아키텍처 안에 녹여내는 설계 확정.

---

## 1. LSM 파이프라인 → OGRE Compositor 매핑

```
LSM (recordCommandBuffer)              OGRE Compositor Pass
─────────────────────────              ─────────────────────
Phase 0:  Compute Sim         →   PASS_COMPUTE (HlmsComputeJob)
Phase 0.5: TLAS Build         →   PASS_CUSTOM "tlas_build"      ★ Raw Vulkan
Phase 1:  Rasterize           →   PASS_SCENE (표준 HLMS PBS)
Phase 2:  Copy Background     →   PASS_CUSTOM "copy_rt"         ★ Raw Vulkan
Phase 3:  Ray Tracing         →   PASS_CUSTOM "ray_tracing"     ★ Raw Vulkan
Phase 4-5: Final Copy         →   PASS_QUAD (blit)
```

OGRE에 없는 3개 패스(★)는 `CompositorPassProvider` 인터페이스로 커스텀 구현.

### Compositor Workspace 정의 (프로그래매틱)

```
PrismHybridNode
│
├── target rt_colour (PFG_RGBA8_UNORM_SRGB) + rt_depth (PFG_D32_FLOAT)
│   ├── [0] pass compute       → "prism/simulation" (물리 시뮬레이션)
│   ├── [0.5] pass custom      → tlas_build (TLAS 재빌드)
│   └── [1] pass render_scene  → 표준 래스터 (RQ 0~99)
│
├── target rt_storage (PFG_RGBA8_UNORM, UAV)
│   └── [2] pass custom        → copy_rt (래스터 결과 → storageImage)
│
├── target rt_storage + rt_accum (PFG_RGBA32_FLOAT, UAV)
│   └── [3] pass custom        → ray_tracing (vkCmdTraceRaysKHR)
│
└── target renderwindow
    └── [4] pass quad           → "prism/rt_blit" (최종 합성)
```

---

## 2. 시스템 레이어 다이어그램

```
┌─────────────────────────────────────────────────────────────┐
│  Layer 4 │ Application                                       │
│          │ PRISMGameState, Scene Setup, Camera, Input (SDL2) │
├─────────────────────────────────────────────────────────────┤
│  Layer 3 │ PRISM Engine Layer                                │
│          │ PrismHybridRenderer  │ PrismRTManager             │
│          │ (Workspace 구성)      │ (BLAS/TLAS, SBT, RT PSO)  │
│          ├───────────────────────────────────────────────────┤
│          │ PrismBufferManager   │ PrismMaterialSync          │
│          │ (SSBO/UBO 관리)       │ (Datablock↔RT재질 동기화)  │
├─────────────────────────────────────────────────────────────┤
│  Layer 2 │ OGRE NEXT Extension Layer                         │
│          │ PrismCompositorPassProvider (3개 커스텀 패스 등록) │
│          │   → CompositorPassTLASBuild                       │
│          │   → CompositorPassRayTracing                      │
│          │   → CompositorPassCopyRT                          │
│          │ PrismHlmsListener (SSBO 바인딩 + 프로퍼티 주입)   │
│          │ PrismRTExtensions (Vulkan RT 함수 포인터 로딩)    │
├─────────────────────────────────────────────────────────────┤
│  Layer 1 │ OGRE NEXT Core (수정 필요: VulkanDevice RT확장)   │
│          │ SceneManager │ VulkanRenderSystem │ VulkanQueue    │
│          │ VulkanDevice │ VulkanVaoManager │ HlmsPbs/Compute │
│          │ CompositorManager2                                │
├─────────────────────────────────────────────────────────────┤
│  Layer 0 │ Vulkan 1.2+ │ VK_KHR_ray_tracing_pipeline        │
│          │ VK_KHR_acceleration_structure │ Windows/MSVC      │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 파일/클래스 구조

```
PRISM_Engine/src/
├── main.cpp                              // 앱 진입점, 씬 셋업
│
├── Compositor/
│   ├── PrismCompositorPassProvider.h/.cpp // CompositorPassProvider 구현
│   │   - addPassDef(): "tlas_build", "ray_tracing", "copy_rt" 정의 생성
│   │   - addPass(): 런타임 패스 인스턴스 생성
│   │
│   ├── PrismCompositorPassTLASBuild.h/.cpp
│   │   - execute(): endAllEncoders → barrier → vkCmdBuildAccelerationStructuresKHR
│   │
│   ├── PrismCompositorPassRayTracing.h/.cpp
│   │   - execute(): endAllEncoders → 리소스 바인드 → vkCmdTraceRaysKHR
│   │
│   ├── PrismCompositorPassCopyRT.h/.cpp
│   │   - execute(): endAllEncoders → 이미지 레이아웃 전환 → vkCmdCopyImage
│   │
│   └── PrismWorkspaceBuilder.h/.cpp
│       - buildWorkspace(): Compositor 노드/워크스페이스 프로그래매틱 구성
│
├── RayTracing/
│   ├── PrismRTExtensions.h/.cpp
│   │   - loadFunctionPointers(): vkCmdTraceRaysKHR 등 함수 포인터 로딩
│   │   - checkRTSupport(): 디바이스 RT 지원 확인
│   │
│   ├── PrismRTManager.h/.cpp
│   │   - createTLAS() / rebuildTLAS(): TLAS 생성/매프레임 재빌드
│   │   - 소유: TLAS 버퍼, instanceBuffer, scratchBuffer
│   │
│   ├── PrismBLASBuilder.h/.cpp
│   │   - createBLAS(GeometryData): 메시별 BLAS 생성
│   │   - 소유: BLAS 버퍼 목록
│   │
│   └── PrismRTPipeline.h/.cpp
│       - createRTPipeline(): RT PSO 생성 (raygen/hit/miss 셰이더)
│       - createSBT(): Shader Binding Table 생성
│       - createDescriptorSets(): RT 전용 디스크립터 셋 관리
│       - 소유: VkPipeline, SBT 버퍼, descriptor pool/sets
│
├── Buffer/
│   ├── PrismBufferManager.h/.cpp
│   │   - createObjectSSBO(): VaoManager→createUavBuffer (ObjState[])
│   │   - createRTUniformBuffer(): 카메라/조명 UBO (256바이트)
│   │   - getVkBuffer(): OGRE UavBuffer → VulkanBufferInterface → VkBuffer 추출
│   │
│   └── PrismMaterialSync.h/.cpp
│       - syncMaterials(): OGRE HlmsDatablock 속성 → instanceMaterialBuffer
│       - 소유: instanceMaterialBuffer, objDescBuffer
│
├── HLMS/
│   ├── PrismHlmsListener.h/.cpp
│   │   - setupRootLayout(): objectSSBO 디스크립터 슬롯 추가
│   │   - hlmsTypeChanged(): objectSSBO 바인딩
│   │   - preparePassHash(): prism_render_mode 프로퍼티 주입
│   │   - preparePassBuffer(): viewInverse, projInverse 등 주입
│   │
│   └── PrismComputeJobs.h/.cpp
│       - registerSimulationJob(): simulation.comp → HlmsComputeJob 등록
│       - 바인딩: objectSSBO(slot 0, rw), instanceBuffer(slot 1, rw)
│
├── Vulkan/
│   └── PrismVulkanBootstrap.h/.cpp
│       - enableRTExtensions(): VulkanDevice 확장 활성화 로직
│       - 또는 OgreVulkanDevice.cpp 직접 수정 안내
│
└── Shaders/
    ├── Compute/
    │   └── simulation.comp.any          // HLMS 컴퓨트 템플릿 (또는 raw SPIR-V)
    ├── RayTracing/                      // Offline SPIR-V 컴파일
    │   ├── raygenbsdf.rgen
    │   ├── closesthitbsdf.rchit
    │   ├── miss.rmiss
    │   └── shadow.rmiss
    └── HLMS_Pieces/
        └── PrismInstancing_piece_vs.any // SSBO 인스턴싱 VS 확장
```

---

## 4. 버퍼/리소스 소유권 전략

| 리소스 | 소유자 | 생성 방법 | 이유 |
|:---|:---|:---|:---|
| objectSSBO (ObjState[]) | OGRE VaoManager | `createUavBuffer()` | Compute+Raster 공유, OGRE가 배리어 관리 |
| instanceBuffer (AS 인스턴스) | Raw Vulkan | `vkCreateBuffer` + DEVICE_ADDRESS | AS 빌드 입력용 플래그 필요, OGRE 불가 |
| BLAS 버퍼 | Raw Vulkan | `vkCreateBuffer` + AS_STORAGE | OGRE에 AS 버퍼 타입 없음 |
| TLAS 버퍼 + scratch | Raw Vulkan | 동일 | 동일 |
| storageImage (RT 출력) | OGRE TextureGpu | `TextureFlags::Uav` | OGRE가 레이아웃 전환 관리 가능 |
| accumImage (HDR) | OGRE TextureGpu | PFG_RGBA32_FLOAT + Uav | 동일 |
| depthImage | OGRE (Compositor) | 자동 관리 | 표준 깊이 버퍼 |
| RT UBO (카메라/조명) | OGRE VaoManager | `createConstBuffer` 또는 PassBuffer | 통합 관리 |
| instanceMaterialBuffer | OGRE VaoManager | `createReadOnlyBuffer` | 셰이더에서 읽기 전용 |
| objDescBuffer (디바이스 주소) | Raw Vulkan | DEVICE_ADDRESS 필요 | uint64 디바이스 주소 포함 |
| SBT | Raw Vulkan | 수동 생성 | OGRE 해당 없음 |

### OGRE ↔ Raw Vulkan 브릿지

```cpp
// OGRE가 소유한 버퍼의 VkBuffer 핸들 추출
UavBufferPacked* ogreBuffer = vaoManager->createUavBuffer(...);
VulkanBufferInterface* vkInterface =
    static_cast<VulkanBufferInterface*>(ogreBuffer->getBufferInterface());
VkBuffer rawHandle = vkInterface->getVboName();
```

---

## 5. HLMS 확장 전략: Listener + Pieces

### 왜 Listener인가 (HlmsPbs 상속 아님)

- RT 셰이더(raygen/hit/miss)는 HLMS 파이프라인 **완전히 밖에** 있음 (offline SPIR-V)
- HLMS는 래스터 패스 셰이더만 제어함
- Listener로 SSBO 바인딩 + 프로퍼티 주입이면 래스터 측은 충분
- 향후 SoftBody 통합 시 더 깊은 확장(hlms_compute_deform) 필요하면 그때 상속 전환

### PrismHlmsListener 역할

```cpp
class PrismHlmsListener : public Ogre::HlmsListener {
    // 1. 디스크립터 슬롯 추가 (objectSSBO용)
    void setupRootLayout(RootLayout& rootLayout, ...) override;

    // 2. 래스터 패스에 objectSSBO 바인딩
    void hlmsTypeChanged(bool casterPass, CommandBuffer*, ...) override;

    // 3. Per-Item 렌더 모드 프로퍼티 주입
    void preparePassHash(..., Hlms* hlms) override {
        hlms->_setProperty("prism_render_mode", currentMode);
    }

    // 4. 패스 버퍼에 카메라 역행렬 등 주입
    uint32 getPassBufferSize(...) const override;
    float* preparePassBuffer(...) override;
};
```

### HLMS Piece 파일: SSBO 인스턴싱

```hlsl
// Media/Hlms/Pbs/Any/PrismInstancing_piece_vs.any
@property( prism_ssbo_instancing )
@piece( custom_vs_preTransform )
    // gl_InstanceIndex로 objectSSBO에서 모델 행렬 읽기
    ObjState obj = objectBuffer.objects[inVs_instanceId];
    float4x4 worldMatrix = obj.model;
@end
@end
```

---

## 6. 동기화/배리어 전략

### OGRE 자동 관리 (건드릴 필요 없음)

- PASS_COMPUTE → PASS_SCENE: OGRE의 `getGraphicsEncoder()`가 compute→graphics 배리어 자동 삽입
- PASS_SCENE 렌더 타겟: OGRE가 color/depth 어태치먼트 전환 관리

### 커스텀 패스 내 수동 배리어 (★ 직접 구현)

| 위치 | 배리어 | src → dst |
|:---|:---|:---|
| TLASBuild 시작 | Compute 쓰기 → AS 빌드 읽기 | `COMPUTE_SHADER → AS_BUILD` |
| TLASBuild 끝 | AS 빌드 쓰기 → RT 읽기 | `AS_BUILD → RT_SHADER` |
| CopyRT | 래스터 출력 → Transfer src | `COLOR_ATTACHMENT → TRANSFER` |
| CopyRT | storageImage → General (RT 쓰기용) | `TRANSFER_DST → GENERAL` |
| RayTracing 끝 | storageImage → Transfer src | `GENERAL → TRANSFER_SRC` |

### 핵심: Encoder 상태 관리

```cpp
void PrismCustomPass::execute(const Camera* lodCamera)
{
    // 1. OGRE 인코더를 반드시 먼저 종료
    VulkanQueue* queue = /* VulkanRenderSystem에서 획득 */;
    queue->endAllEncoders();

    // 2. Raw Vulkan 커맨드 기록
    VkCommandBuffer cmd = queue->getCurrentCmdBuffer();
    // ... vkCmdPipelineBarrier, vkCmdBuildAS, vkCmdTraceRays ...

    // 3. 인코더를 Closed 상태로 둠 → OGRE가 다음 패스에서 적절히 재오픈
}
```

---

## 7. OGRE 소스 수정 범위 (VulkanDevice RT 확장)

`OgreVulkanDevice.cpp`의 `createDevice()` (라인 907 부근)에 추가:

```cpp
// RT 확장 활성화
else if (extensionName == VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
    deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
else if (extensionName == VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)
    deviceExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
else if (extensionName == VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
    deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
else if (extensionName == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
    deviceExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
```

`fillDeviceFeatures2()`에 피처 체인 추가:
- `VkPhysicalDeviceAccelerationStructureFeaturesKHR`
- `VkPhysicalDeviceRayTracingPipelineFeaturesKHR`
- `VkPhysicalDeviceBufferDeviceAddressFeatures`

---

## 8. LSM 코드 재사용 vs 재작성 판정

### 그대로 재사용 (SPIR-V 컴파일만)
- `simulation.comp` → HlmsComputeJob 또는 offline SPIR-V
- `raygenbsdf.rgen`, `closesthitbsdf.rchit`, `miss.rmiss`, `shadow.rmiss`
- C++ 데이터 구조체 (ObjState, Vertex, ObjDesc 등 — 16바이트 정렬 완료)

### 추출하여 래핑 (알고리즘 동일, API 호출 패턴 동일)
- `createBottomLevelAS()` → PrismBLASBuilder
- `createTopLevelAS()` + 매프레임 rebuild → PrismRTManager
- RT 파이프라인 생성 + SBT → PrismRTPipeline
- `recordCommandBuffer()` 내 배리어 패턴

### 완전 재작성 (OGRE 방식으로)
- 윈도우/스왑체인 관리 (OGRE 소유)
- 커맨드 버퍼 기록 (Compositor 패스로 분할)
- 래스터 파이프라인 (HLMS PBS가 PSO 생성)
- 디스크립터 셋 관리 (OGRE 관리 + RT 전용 분리)
- 씬 셋업 (OGRE SceneManager API)
- 입력 처리 (SDL2, 기존 main.cpp 패턴)

---

## 9. 구현 단계 로드맵

### Phase 1: Vulkan 전환 + Compute Pass
- D3D11 → Vulkan 렌더러 전환
- OGRE 소스에 RT 확장 활성화
- simulation.comp → HlmsComputeJob 등록
- objectSSBO 생성 + Compute Dispatch 확인

### Phase 2: 커스텀 Compositor Pass 인프라
- PrismCompositorPassProvider 구현 (3개 패스 등록)
- PrismHlmsListener 구현 (SSBO 바인딩)
- SSBO 인스턴싱 VS Piece 파일 작성
- 프로그래매틱 Workspace 구성
- 확인: Compute → Raster 데이터 흐름

### Phase 3: RT 인프라 (BLAS/TLAS/파이프라인)
- RT 함수 포인터 로딩
- BLAS 빌더 (OGRE 메시 → BLAS)
- TLAS 관리자 + 매프레임 재빌드
- RT 파이프라인 + SBT + 디스크립터 셋
- 확인: Validation Layer 클린

### Phase 4: 하이브리드 렌더링 실행
- CopyRT 패스: 래스터 결과 → storageImage
- RayTracing 패스: vkCmdTraceRaysKHR 실행
- 최종 합성 (PASS_QUAD blit)
- 누적/톤매핑 로직
- 확인: RT 오브젝트 렌더링, 깊이 인식 동작

### Phase 5: SoftBody 통합 준비
- Per-Item 렌더 모드 (RASTERIZE/RT/HYBRID)
- 동적 BLAS Refit 경로 (변형 메시용)
- RenderQueue 기반 필터링
- 디버그 시각화 패스

---

## 10. 핵심 설계 결정 요약

| 결정 | 선택 | 근거 |
|:---|:---|:---|
| RT 패스 구현 | PASS_CUSTOM + CompositorPassProvider | OGRE 공식 확장 포인트 |
| 버퍼 소유권 분리 | OGRE(compute/raster 공유) + Raw Vulkan(RT전용) | OGRE VaoManager가 AS 플래그 미지원 |
| HLMS 확장 | Listener + Pieces (상속 아님) | RT 셰이더는 HLMS 밖; Listener로 래스터 측 충분 |
| OGRE 소스 수정 | VulkanDevice에 RT 확장 추가 | 소스 빌드 기반이므로 가장 깔끔 |
| RT 셰이더 | Offline SPIR-V (HLMS 아님) | raygen/hit/miss는 HLMS 템플릿과 무관 |
| Encoder 관리 | 커스텀 패스마다 endAllEncoders() 호출 | OGRE 상태 충돌 방지 |
| Workspace 정의 | 프로그래매틱 (스크립트 아님) | 커스텀 패스는 C++ 구성 필수 |
| TLAS 전략 | 매프레임 Full Rebuild (LSM 동일) | 단순/정확; Refit 최적화는 Phase 5 |

---

## 11. 대상 파일 참조

- **LSM 원본**: `Project/2_LSM/Vulkan_HybridPipeLine_Sample/Vulkan_ex01.cpp`
  - BLAS 생성: `createBottomLevelAS()`
  - TLAS 생성: `createTopLevelAS()`
  - RT PSO/SBT: `createRTPipeline()`, `createShaderBindingTable()`
  - 커맨드 기록: `recordCommandBuffer()` (배리어 패턴)
- **OGRE 확장점**: `1_JGN/ogre-next/` 소스
  - `OgreVulkanDevice.cpp` — RT 확장 활성화 수정점
  - `OgreVulkanQueue.h` — EncoderState, endAllEncoders()
  - `OgreCompositorPassProvider.h` — 커스텀 패스 인터페이스
  - `OgreHlmsListener.h` — HLMS 확장 인터페이스
  - `OgreHlmsCompute.h` / `OgreHlmsComputeJob.h` — Compute 잡 시스템
- **현재 프로토타입**: `Project/PRISM_Engine/src/main.cpp` (Phase 1에서 Vulkan 전환)

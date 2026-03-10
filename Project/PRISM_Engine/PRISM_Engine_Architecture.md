# PRISM Engine - 아키텍처 분석 문서

> **작성 기준**: 2026-03-10
> **현재 Phase**: Phase 1 완료 (하드웨어 RT 활성화 + Disney BSDF Path Tracing)
> **관련 문서**: `PRISM_Engine_Modifications.md` (엔진 수정 내역), `0308.md` (최신 개발 일지)

---

## 1. 프로젝트 개요

**목표**: OGRE Next 3.0 + Vulkan 1.2 Ray Tracing Extensions를 통합한 **실시간 물리 기반 Path Tracing 렌더러**
**검증 모델**: Stanford Bunny (2,503 정점)
**향후 목표**: Softbody XPBD 물리 연산과 RT 파이프라인 통합 (Phase 2)

---

## 2. 디렉토리 구조

```
PRISM_Engine/
├── src/
│   ├── main.cpp                   # 앱 진입점, OGRE 초기화, 렌더 루프
│   ├── PrismRTPipeline.h/cpp      # Vulkan RT 파이프라인 핵심 클래스
│   └── PrismCompositorPass.h/cpp  # OGRE Compositor ↔ Vulkan RT 브릿지
├── shaders/
│   ├── raygenbsdf.rgen            # 레이 생성 + 7바운스 Path Tracing
│   ├── closesthitbsdf.rchit       # 히트포인트 처리 + PBR 재질
│   ├── miss.rmiss                 # 레이 미스 → 하늘색 처리
│   ├── shadow.rmiss               # (현재 미사용)
│   └── *.spv                      # 컴파일된 SPIR-V 바이너리
├── CMakeLists.txt                 # 빌드 구성 (셰이더 자동 컴파일, DLL 복사)
├── vcpkg.json                     # 의존성 (SDL2, imgui, spdlog)
├── vcpkg-overlays/                # OGRE Next 커스텀 수술 스크립트
├── bunny.obj                      # Stanford Bunny 모델
├── PRISM_Engine_Modifications.md  # OGRE 소스 수정 내역
├── 0308.md                        # 개발 일지 (2026-03-08, 최신)
└── 03_05.md                       # 통합 회고 (2026-03-05)
```

---

## 3. 빌드 구성 (CMakeLists.txt)

**C++ 표준**: C++17
**빌드 타겟**: Debug (MSVC)

**링크 라이브러리**:
```
OgreNextMain_d.lib
OgreNextHlmsPbs_d.lib
OgreNextHlmsUnlit_d.lib
RenderSystem_Vulkan_d.lib    ← PRISM 수정본 (Vulkan 1.2 + RT 확장)
Vulkan::Vulkan
SDL2::SDL2
```

**셰이더 자동 컴파일** (glslangValidator, SPIR-V 1.5 / Vulkan 1.2 타겟):
```
raygenbsdf.rgen      → raygenbsdf.rgen.spv
miss.rmiss           → miss.rmiss.spv
closesthitbsdf.rchit → closesthitbsdf.rchit.spv
shadow.rmiss         → shadow.rmiss.spv
```

**POST_BUILD 자동 복사 항목**:
- `RenderSystem_Vulkan_d.dll`
- `ltcMatrix0.dds`, `ltcMatrix1.dds`, `brtfLutDfg.dds` → `./Media/Common/`

---

## 4. 핵심 클래스 구조

### 4.1 PrismRTPipeline

Vulkan Ray Tracing 파이프라인의 모든 것을 소유·관리하는 클래스.

**보유 리소스 목록**:

| 멤버 | 타입 | 설명 |
|------|------|------|
| `mRenderSystem` | `Ogre::VulkanRenderSystem*` | OGRE Vulkan 렌더 시스템 |
| `mDevice` | `Ogre::VulkanDevice*` | 디바이스/큐 핸들 |
| `mRTPipeline` | `VkPipeline` | RT 파이프라인 |
| `mPipelineLayout` | `VkPipelineLayout` | 레이아웃 |
| `mBLAS` | `VkAccelerationStructureKHR` | Bottom-Level AS (메시 단위) |
| `mTopLevelAS` | `VkAccelerationStructureKHR` | Top-Level AS (인스턴스 단위) |
| `mBLASAddress` | `VkDeviceAddress` | BLAS GPU 주소 |
| `mStorageImage` | `VkImage` | RT 출력 이미지 (RGBA8) |
| `mAccumImage` | `VkImage` | Progressive 누적 버퍼 (RGBA32F) |
| `mCameraUBOBuffer` | `VkBuffer` | 카메라 / 조명 UBO |
| `mMaterialBuffer` | `VkBuffer` | 재질 파라미터 SSBO |
| `mObjDescBuffer` | `VkBuffer` | 정점/인덱스 GPU 주소 테이블 |
| `mRTVertexBuffer` | `VkBuffer` | RT 전용 정점 버퍼 (RTVertex) |
| `mRTIndexBuffer` | `VkBuffer` | RT 전용 인덱스 버퍼 |
| `mSBTBuffer` | `VkBuffer` | Shader Binding Table |

**주요 메서드**:

| 메서드 | 설명 |
|--------|------|
| `initialize()` | RT 함수 포인터 로드, CommandPool 생성, 파이프라인 초기화 |
| `buildBLAS(Ogre::MeshPtr)` | OGRE 메시 → RTVertex 변환 → BLAS 구축 |
| `buildTLAS(objects)` | 인스턴스 → TLAS 구축 (현재 단일 객체) |
| `createRTPipeline()` | SPIR-V 로드 → VkRayTracingPipeline 생성 |
| `createSBT()` | Shader Binding Table 생성 및 초기화 |
| `createRTImages()` | Storage/Accum 이미지 생성 및 레이아웃 전환 |
| `createDescriptorSet()` | 7개 바인딩 디스크립터 셋 구성 |
| `recordRayTracingCommands()` | `vkCmdTraceRaysKHR` 실행 |
| `updateCameraUBO()` | 매 프레임 카메라/조명/frameCount 업데이트 |

**Descriptor Set 바인딩 (0~6)**:
```
Binding 0: Acceleration Structure  (TLAS)
Binding 1: Storage Image           (RT 출력)
Binding 2: Uniform Buffer          (CameraUBO)
Binding 3: Storage Buffer          (Materials)
Binding 4: Storage Buffer          (ObjDesc - 정점/인덱스 주소)
Binding 5: Combined Image Sampler  (Dummy Depth, 미사용)
Binding 6: Storage Image           (Accumulation)
```

---

### 4.2 RTCompositorPass

OGRE `CompositorPassProvider`를 구현한 브릿지 클래스.
OGRE 렌더 루프 안에서 Vulkan RT를 직접 실행한다.

**`execute()` 흐름**:
```
1. device->mGraphicsQueue.endAllEncoders()
   └─ OGRE 래스터화 완료, 인코더 닫기 (필수)
2. VkCommandBuffer 획득
3. mRTPipeline->recordRayTracingCommands()
   └─ vkCmdTraceRaysKHR 기록
4. Image Barrier: StorageImage (GENERAL → TRANSFER_SRC)
5. Image Barrier: SwapChain   (UNDEFINED → TRANSFER_DST)
6. vkCmdBlitImage()
   └─ RT 결과를 스왑체인으로 복사
7. Barrier 복구: SwapChain → PRESENT_SRC, StorageImage → GENERAL
```

---

### 4.3 PrismApp (main.cpp)

**초기화 순서**:
```
1.  Ogre::Root 생성
2.  RenderSystem_Vulkan_d.dll 로드
3.  SDL2 윈도우 생성 (1280×720)
4.  OGRE 렌더 윈도우 바인딩
5.  PrismRTPipeline 초기화
6.  RTCompositorPassProvider 등록
7.  HLMS (PBS, Unlit) 등록
8.  SceneManager 생성
9.  Camera 생성 + 수동 SceneNode 설정 (auto-attach 해제 필수)
10. bunny.obj 파싱 → OGRE VAO → Item 생성 (Scale 1000배)
11. buildBLAS() → buildTLAS()
12. createDescriptorSet()
13. Compositor 노드 / 작업공간 정의
```

**렌더 루프 (`run()`)의 핵심 로직**:
```cpp
// 이동 감지 → Progressive 리셋
if (bMoved) frameCount = 0;
else        frameCount++;

mRTPipeline->updateCameraUBO(view, proj, camPos, frameCount);
mSceneMgr->updateSceneGraph();   // 씬 그래프 캐시 갱신 (필수!)
mRoot->renderOneFrame();
```

---

## 5. 데이터 구조체

### CameraUBO (GPU 전송용, 272B)
```cpp
struct CameraUBO {
    mat4  viewInverse;      // offset   0 (64B)
    mat4  projInverse;      // offset  64 (64B)
    vec3  cameraPos;        // offset 128 (12B)
    int   frameCount;       // offset 144  (4B)  ← Progressive 제어
    GpuLight lights[3];     // offset 160 (96B)  ← Light당 32B
    int   lightCount;       // offset 256  (4B)
    float padding[3];       // offset 260 (12B)
};
```

### RTVertex (GPU `scalar` layout, 32B)
```cpp
struct RTVertex {
    float pos[3];    float pad1;    // 16B (vec4 경계 맞춤)
    float normal[3]; float pad2;    // 16B
};
```

### ObjDesc (GPU 주소 테이블)
```cpp
struct ObjDesc {
    uint64_t vertexAddress;   // RTVertexBuffer GPU 주소
    uint64_t indexAddress;    // RTIndexBuffer  GPU 주소
};
```

### InstanceMaterial
```cpp
struct InstanceMaterial {
    float albedo[4];      // RGB + 알파
    float pbrParams1[4];  // emissive, roughness, metallic, padding
    float pbrParams2[4];  // specTrans, ior, padding, padding
};
```

---

## 6. 렌더링 파이프라인 전체 흐름

```
┌─────────────────────────────────────────────────────────────────┐
│  OGRE PASS_SCENE (Rasterization)                                │
│  └─ 씬 그래프 기반 래스터화 (현재 배경 클리어 역할)              │
└────────────────────────────┬────────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────────┐
│  RT CUSTOM PASS (RTCompositorPass::execute)                     │
│                                                                 │
│  endAllEncoders()                                               │
│  ↓                                                              │
│  vkCmdTraceRaysKHR                                              │
│  ├─ raygenbsdf.rgen      픽셀당 레이 생성 + Path Tracing        │
│  │  ├─ 7바운스 루프                                             │
│  │  │  ├─ traceRayEXT()                                         │
│  │  │  ├─ Disney BSDF 평가 (Diffuse/Specular/Refraction)        │
│  │  │  └─ Russian Roulette 샘플링                               │
│  │  ├─ Progressive Accumulation (RGBA32F AccumImage)            │
│  │  └─ Tone Mapping (Reinhard, Exposure 2.5, Gamma 2.2)        │
│  ├─ closesthitbsdf.rchit Barycentric 보간 → payload 반환        │
│  └─ miss.rmiss           hitT = -1 (하늘색 처리)                │
│                                                                 │
│  StorageImage → vkCmdBlitImage → SwapChain                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 7. 셰이더 상세 분석

### 7.1 raygenbsdf.rgen

**알고리즘 요약**:
1. Jittered 픽셀 좌표 → NDC → 월드 공간 레이 생성
2. Path Tracing 루프 (최대 7 바운스):
   - `traceRayEXT()` → `payload` 획득
   - `payload.hitT < 0` → Miss (하늘색 누적 후 종료)
   - 재질 기반 BSDF 분기:
     - `rand < prob_reflection` → `SampleGGX` (Specular Reflection)
     - `rand < prob_reflection + prob_transmission` → `SampleBTDF` (Refraction)
     - else → `SampleCosineHemisphere` (Diffuse)
   - `throughput *= (bsdf * cosTheta) / pdf`
3. AccumImage에서 이전 프레임 블렌딩 (`1 / (frameCount + 1)` 가중치)
4. Reinhard 톤 매핑 + Gamma 교정 → `imageStore(storageImage)`

**Disney BRDF 구성**:
- Schlick Fresnel
- GGX Microfacet Distribution (D)
- Smith Visibility Function (G)
- Diffuse: `(albedo / PI) * (1 - metallic) * (1 - specTrans)`
- Specular: `(D * F * G) / (4 * NdotL * NdotV)`

**Disney BTDF (Refraction)**:
- Exact Fresnel-Dielectric 계산
- GGX BTDF + Jacobian 보정

### 7.2 closesthitbsdf.rchit

1. `gl_InstanceCustomIndexEXT` → `ObjDesc` / `InstanceMaterial` 획득
2. `gl_PrimitiveID` → 삼각형 정점 인덱스 (v0, v1, v2)
3. Barycentric Interpolation → Shading Normal, Geometric Normal
4. 월드 공간 변환 (`gl_WorldToObjectEXT`)
5. `payload` 설정: hitPos, normal, geomNormal, albedo, roughness, metallic, specTrans, ior

### 7.3 miss.rmiss

- `payload.hitT = -1.0` 설정
- Raygen에서 이를 감지해 하늘색 적용 후 루프 종료

---

## 8. 구현 현황

### 완료 ✓

| 항목 | 비고 |
|------|------|
| OBJ 파서 + OGRE VAO | 중심 정규화 포함 |
| Vulkan 1.2 디바이스 초기화 | OGRE 소스 수정 |
| RT 확장 4종 활성화 | `PRISM_Engine_Modifications.md` 참고 |
| BLAS / TLAS 구축 | 단일 정적 객체 |
| Shader Binding Table | raygen / miss / hit |
| 7바운스 Disney BSDF Path Tracing | Diffuse + Specular + Refraction |
| Progressive Accumulation | RGBA32F AccumImage |
| Ghosting 제거 | frameCount 리셋 |
| Reinhard Tone Mapping | Exposure 2.5, Gamma 2.2 |
| OGRE Compositor 통합 | RTCompositorPassProvider |
| 카메라 WASD + 마우스 회전 | Yaw/Pitch |
| NaN / Infinity 제거 | 셰이더 내 보호 코드 |

### 미구현 (Phase 2+)

| 항목 | 우선순위 | 비고 |
|------|----------|------|
| **Softbody XPBD Compute** | High | Phase 2 핵심 목표 |
| **BLAS Refit/Rebuild** | High | 변형 메시 매 프레임 갱신 |
| Multi-Object TLAS | High | 여러 오브젝트 인스턴싱 |
| Area Light 직접 샘플링 | Medium | 메시 기반 면광원 |
| IBL (HDRI 큐브맵) | Medium | miss.rmiss에 적용 |
| Denoising | Medium | Temporal 재투사 |
| 해상도 Resize 지원 | Low | AccumImage 재할당 |
| Async Compute | Low | 물리 + RT 병렬 |
| Debug Visualization | Low | Normal/Albedo/Depth |

---

## 9. 주요 코드 패턴 및 주의사항

### 패턴 1: Vulkan RT 함수는 런타임 로드 필수
```cpp
// RT 확장 함수는 컴파일 타임 링크 불가 → 런타임 동적 로드
PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetASBuildSizes =
    (PFN_vkGetAccelerationStructureBuildSizesKHR)
    vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
```

### 패턴 2: RTVertex 정렬 (scalar layout)
```cpp
// GPU scalar layout에서 vec3 뒤 패딩 필수 (총 32B)
struct RTVertex {
    float pos[3];    float pad1;   // 16B
    float normal[3]; float pad2;   // 16B
};
```

### 패턴 3: OGRE 인코더 닫기 후 Vulkan CommandBuffer 사용
```cpp
// Vulkan 명령을 직접 기록하기 전 OGRE 내부 인코더 반드시 종료
device->mGraphicsQueue.endAllEncoders();
VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
// 이후 vkCmd* 직접 호출 가능
```

### 패턴 4: Camera Auto-Attach 해제 (OGRE Next 3.0)
```cpp
Ogre::Camera* camera = sceneManager->createCamera("MainCamera");
if (camera->isAttached()) camera->detachFromParent();  // auto-attach 해제 필수!
Ogre::SceneNode* cameraNode =
    sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
cameraNode->attachObject(camera);
```
> 해제하지 않으면 루트 SceneNode를 이동시켜 전체 씬이 오프셋됨.

### 패턴 5: 씬 그래프 캐시 갱신
```cpp
// 노드 위치 변경 후 반드시 호출 (OgreNode.h:680 Assertion 방지)
mSceneMgr->updateSceneGraph();
```

### 패턴 6: Ghosting 제거 (Progressive Rendering)
```cpp
// C++ 렌더 루프
if (bMoved) frameCount = 0;
else        frameCount++;
```
```glsl
// 셰이더 내 누적
if (ubo.frameCount > 0)
    color = mix(prevColor, newColor, 1.0 / (frameCount + 1));
else
    color = newColor;
```

---

## 10. 빌드 및 실행

### 전제 조건
- Visual Studio 2019+ (MSVC)
- CMake 3.20+
- Vulkan SDK 1.2.162+
- vcpkg (x64-windows)
- GPU: Vulkan 1.2 + RT 지원 (NVIDIA RTX 2080+ 권장)

### 빌드
```bash
cd D:/Git/P.R.I.S.M/Project/PRISM_Engine
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Debug
```

### 실행
```bash
cd build/Debug
./PRISM_Engine.exe
```

### 셰이더 수동 재컴파일
```bash
cd shaders
glslangValidator -V --target-env vulkan1.2 raygenbsdf.rgen -o raygenbsdf.rgen.spv
glslangValidator -V --target-env vulkan1.2 closesthitbsdf.rchit -o closesthitbsdf.rchit.spv
glslangValidator -V --target-env vulkan1.2 miss.rmiss -o miss.rmiss.spv
```

### 카메라 조작
| 입력 | 동작 |
|------|------|
| W/S | 전진/후퇴 |
| A/D | 좌우 이동 |
| 우클릭 드래그 | 시점 회전 (Yaw/Pitch) |
| 마우스 릴리즈 | 누적 재개 |

---

## 11. Phase 2 진입 체크리스트

Phase 2 (Softbody 통합)를 시작하기 전 확인 사항:

- [x] Vulkan 1.2 디바이스 초기화
- [x] BLAS/TLAS 구축
- [x] Descriptor Set 완성 (Binding 0~6)
- [x] Path Tracing 렌더링 동작 확인
- [ ] **Compute Shader 기반 XPBD 정점 업데이트**
- [ ] **매 프레임 BLAS Refit (`VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR`)**
- [ ] **RT Vertex Buffer → Compute SSBO 공유 전략**
- [ ] TLAS 다중 인스턴스 확장

---

*PRISM ENGINE | 1_JGN, 2_LSM, 3_HSH*

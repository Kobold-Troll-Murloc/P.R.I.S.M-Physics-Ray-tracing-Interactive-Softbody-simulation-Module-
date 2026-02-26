# P.R.I.S.M - 프로젝트 전체 개요

**Physics · Ray-tracing · Interactive · Softbody-simulation · Module**

> 작성일: 2026-02-24
> 작성자: 1_JGN (with Claude)

---

## 1. 프로젝트 핵심 목표

OGRE NEXT 엔진을 기반으로, **Compute Shader 기반 Softbody 물리 시뮬레이션 + 하이브리드 렌더링(Rasterization + Ray Tracing) 엔진**을 구축한다.

- **렌더링**: HLMS 기반 Per-Item 렌더 모드 제어. Item별/SubMesh별로 Rasterize, RayTrace, Hybrid 중 선택 가능
- **물리**: FEM, XPBD, Physics AI 모델을 Compute Shader에서 실행
- **통합**: SoftBody 변형 → 동적 BLAS 갱신 → Hybrid 렌더링 으로 이어지는 완전 통합 파이프라인

```
[Compute Pass]  SoftBody 물리 연산 (FEM / XPBD / Physics AI)
      ↓ Deformed Vertex SSBO
[BLAS Update]   동적 오브젝트 가속 구조 갱신
      ↓ TLAS 업데이트
[Rasterize Pass] G-Buffer 기록 (RASTERIZE + HYBRID 모드 Item)
      ↓ G-Buffer
[RT Pass]        광선 추적 (RAYTRACING + HYBRID 모드 Item)
      ↓ RT Output
[Composite Pass] G-Buffer + RT 결과 최종 합성
```

> 상세 설계: [`Hybrid_Rendering_Architecture.md`](./Hybrid_Rendering_Architecture.md)

---

## 2. 팀 구조 및 역할 분담

| 폴더 | 담당 | 역할 |
| :--- | :--- | :--- |
| `1_JGN` | JGN | **OGRE NEXT 엔진 통합** + Softbody Compute Shader 구현 |
| `2_LSM` | LSM | Raw Vulkan API (Basic / HybridPipeLine / RayTracing 샘플) |
| `3_HSH` | HSH | Vulkan 공용 라이브러리 deps 관리 (glm, glfw, SPIRV, Slang, DXC 등) |

---

## 3. 1_JGN 파트 상세 목표

### 3.1 학습 단계: OGRE NEXT 엔진 파악
- OGRE NEXT의 전체 아키텍처를 분석하고, 렌더링 파이프라인의 확장 포인트를 파악
- 핵심 문서: `Ogre_Core_Concepts.md`, `HLMS_Deep_Dive.md`, `Vulkan_Architecture_Guide.md`

### 3.2 구현 단계: Vulkan 렌더러 수정 → Softbody 시뮬레이션 추가
- **Compositor**에 Compute Shader Pass를 삽입하여 렌더링 전 물리 연산 수행
- Compute Shader에서 계산된 변형 오프셋을 **Vertex Buffer(SSBO)에 기록**
- HLMS가 해당 버퍼를 인식하여 Vertex Shader에서 변형 적용 (`hlms_compute_deform`)

### 3.3 최적화 단계: Physics AI 연동
- 전통 물리 방정식(FEM/XPBD) 대신 또는 병행하여 **신경망 추론 모델을 Compute Shader에서 실행**
- 목표: 복잡한 Softbody 시나리오에서 실시간 연산 비용 절감

---

## 4. 핵심 기술 개념

### 4.1 Softbody 시뮬레이션 방법론

| 방법 | 특징 | 적합한 케이스 |
| :--- | :--- | :--- |
| **FEM** (Finite Element Method) | 물리적으로 가장 정확. 연산 비용 높음 | 의료/공학 시뮬레이션, 고정밀 요구 |
| **XPBD** (eXtended Position-Based Dynamics) | Position 기반으로 빠르고 안정적 | 게임 실시간 천/근육 시뮬레이션 |
| **Physics AI** | 학습된 모델로 예측. 대규모 최적화 가능 | 반복적 패턴의 변형 시뮬레이션 |

### 4.2 OGRE NEXT → Vulkan 파이프라인 확장 포인트

```
Compositor
  └── ComputePass (커스텀 추가)
        ├── Input:  Vertex Buffer (원본 위치)
        ├── Shader: softbody_sim.comp.glsl
        └── Output: Deformed Vertex Buffer (SSBO)

VulkanQueue
  └── getComputeEncoder()
        └── [배리어] → getGraphicsEncoder()
              └── 변형된 버퍼로 Draw Call
```

### 4.3 HLMS 커스텀 프로퍼티 (계획)

| Property | 역할 |
| :--- | :--- |
| `hlms_compute_deform` | Vertex Shader에서 변형 오프셋 SSBO를 읽도록 코드 삽입 |
| `hlms_ai_physics` | AI 추론 결과 버퍼를 vertex 데이터 대신 참조 |

---

## 5. 현재 진행 상황 (1_JGN)

### 완료된 작업
- [x] OGRE NEXT 소스코드 빌드 (Windows MSVC 환경 패치 포함)
  - `OgreIrradianceField.cpp` 특수문자 주석 버그 수정
  - `Dependencies.cmake` FreeImage 헤더 경로 수정
- [x] OGRE NEXT 아키텍처 문서화 (`Ogre_Core_Concepts.md`)
- [x] HLMS 시스템 분석 (`HLMS_Deep_Dive.md`)
- [x] Vulkan 렌더러 구조 분석 (`Vulkan_Architecture_Guide.md`)
- [x] P.R.I.S.M 기본 앱 구조 구축 (GraphicsSystem, LogicSystem, GameEntityManager)
- [x] Vulkan 별도 연습 프로젝트 (Basic, HybridPipeLine)

### 다음 단계
- [ ] Compositor에 Compute Pass 통합 (Softbody 연산용)
- [ ] 기본 XPBD Compute Shader 작성 및 SSBO 연동
- [ ] HLMS에 `hlms_compute_deform` 프로퍼티 추가
- [ ] Physics AI 모델 선정 및 SPIR-V 변환 방법 조사

---

## 6. 프로젝트 디렉토리 구조

```
1_JGN/
├── ogre-next/              OGRE NEXT 엔진 소스 (수정본 포함)
├── ogre-next-deps/         OGRE 의존성 (FreeImage, ZLib 등)
├── P.R.I.S.M/              실제 P.R.I.S.M 앱 소스
│   ├── src/
│   │   ├── GraphicsSystem.cpp      렌더링 스레드 메인
│   │   ├── LogicSystem.cpp         게임 로직 스레드
│   │   ├── GameEntityManager.cpp   엔티티 관리
│   │   └── PRISMGameState.cpp      핵심 게임 상태
│   └── include/
├── Vulkan/                 Vulkan 별도 연습 프로젝트
│   ├── Vulkan_Basic/
│   └── Vulkan_HybridPipeLine/
├── Ogre_Core_Concepts.md   OGRE 아키텍처 가이드
├── HLMS_Deep_Dive.md       HLMS 시스템 심층 분석
├── Vulkan_Architecture_Guide.md  Vulkan 렌더러 구조 분석
└── PRISM_Project_Overview.md    (현재 파일) 프로젝트 전체 개요
```

---

## 7. 참고 자료

- [OGRE NEXT 공식 문서](https://ogrecave.github.io/ogre-next/api/latest/)
- [OGRE NEXT GitHub](https://github.com/OGRECave/ogre-next)
- XPBD: "Extended Position Based Dynamics" (Müller et al., 2020)
- FEM Softbody: "Real-time FEM-based simulation" 관련 논문
- Physics Neural Network: "Learning-based simulation" (NeurIPS, ICML 등)

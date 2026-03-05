# PRISM Engine - Ogre-Next 3.0 개조 및 수술 내역서

본 문서는 순정 Ogre-Next 3.0 엔진 소스를 PRISM(하이브리드 레이트레이싱) 프로젝트 목적에 맞게 직접 수정한 기술적 상세 내역을 기록합니다.

---

## 1. Vulkan 인스턴스 및 디바이스 버전 상향
레이트레이싱용 셰이더(SPIR-V 1.5)를 지원하기 위해 엔진의 코어를 Vulkan 1.2 표준으로 격상시켰습니다.

### 수정 파일: `RenderSystems/Vulkan/src/OgreVulkanDevice.cpp`
*   **VulkanInstance 생성자**:
    *   `appInfo.apiVersion`을 `VK_MAKE_VERSION(1, 0, 2)`에서 **`VK_MAKE_VERSION(1, 2, 0)`**으로 상향.
    *   이를 통해 엔진이 드라이버로부터 Vulkan 1.2 기능을 공식적으로 요청하고 SPIR-V 1.5 바이너리를 로드할 수 있게 함.

---

## 2. 하드웨어 레이트레이싱 확장(Extensions) 활성화
기존 엔진에서 필터링되던 레이트레이싱 필수 확장 기능들을 강제로 활성화 리스트에 포함시켰습니다.

### 수정 파일: `RenderSystems/Vulkan/src/OgreVulkanDevice.cpp`
*   **인스턴스 확장 (`enumerateExtensionsAndLayers`)**:
    *   `VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME` 강제 활성화.
    *   `VK_NV_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME` 추가 (Ogre 내부 리소스 호환성).
*   **디바이스 확장 (`createDevice`)**:
    *   다음 확장들을 요청 리스트에 추가:
        *   `VK_KHR_ACCELERATION_STRUCTURE` (가속 구조 구축용)
        *   `VK_KHR_RAY_TRACING_PIPELINE` (RT 파이프라인 생성용)
        *   `VK_KHR_BUFFER_DEVICE_ADDRESS` (GPU 주소 직접 참조용 - RT 필수)
        *   `VK_KHR_DEFERRED_HOST_OPERATIONS` (드라이버 내부 작업용)
        *   `VK_EXT_DESCRIPTOR_INDEXING` (하이브리드 렌더링용 바인딩 확장)
        *   `VK_KHR_EXTERNAL_MEMORY` 및 `win32` 확장 (안정성 확보)

---

## 3. 하드웨어 피처 체인(Features Chain) 설계
가장 난도가 높았던 부분으로, `vkCreateDevice` 호출 시 하드웨어 기능을 드라이버에 전달하는 `pNext` 체인을 수동으로 재설계했습니다.

### 수정 파일: `RenderSystems/Vulkan/include/OgreVulkanDevice.h`
*   **`ExtraVkFeatures` 구조체 확장**:
    *   `rayTracingPipeline`, `accelerationStructure`, `bufferDeviceAddress`, `runtimeDescriptorArray` 멤버 변수 추가.
    *   엔진 내부 어디서든 현재 하드웨어의 RT 지원 여부를 즉시 확인할 수 있도록 설계.

### 수정 파일: `RenderSystems/Vulkan/src/OgreVulkanDevice.cpp`
*   **`createDevice` 내 수동 체인 구축**:
    *   Ogre의 자동화된 `fillDeviceFeatures2`가 RT 구조체의 메모리 정렬을 간혹 깨뜨리는 문제를 해결하기 위해 **Robust Manual Mode** 도입.
    *   `VkPhysicalDeviceFeatures2` -> `RtPipelineFeatures` -> `AsFeatures` -> `Vulkan12Features` -> `DescriptorIndexingFeatures` 순으로 널(Null) 포인터 없이 완벽하게 연결.
    *   **64비트 연산 활성화**: `shaderInt64`, `shaderFloat64` 기능을 기본 활성화하여 고정밀 물리 시뮬레이션 환경 구축.

---

## 4. 엔진-애플리케이션 데이터 동기화 (Sync)
수정된 엔진 소스와 PRISM 애플리케이션 코드가 서로 통신할 수 있도록 인터페이스를 수정했습니다.

### 수정 파일: `RenderSystems/Vulkan/src/OgreVulkanDevice.cpp`
*   **`fillDeviceFeatures2` 함수 개조**:
    *   드라이버로부터 받은 실제 하드웨어 지원 여부를 `mDeviceExtraFeatures`에 다시 써주는(Sync back) 로직 추가.
    *   이를 통해 `PrismRTPipeline.cpp`가 "이 그래픽 카드는 RT가 가능한가?"를 물었을 때 엔진이 정확히 대답할 수 있게 됨.

---

## 💡 유지보수 참고 사항
1.  **재빌드 필수**: 위 파일들을 수정한 후에는 반드시 `RenderSystem_Vulkan` 프로젝트를 **다시 빌드**하여 `RenderSystem_Vulkan_d.dll`을 갱신해야 합니다.
2.  **Vulkan SDK**: 최소 Vulkan SDK 1.2.162 이상이 설치된 환경에서 빌드되어야 합니다.
3.  **DLL 충돌 주의**: 빌드된 DLL은 `PRISM_Engine` 실행 폴더로 자동 복사되도록 `CMakeLists.txt`에 설정되어 있으나, 수동으로 옮길 경우 `vcpkg` 버전과 섞이지 않도록 주의하십시오.

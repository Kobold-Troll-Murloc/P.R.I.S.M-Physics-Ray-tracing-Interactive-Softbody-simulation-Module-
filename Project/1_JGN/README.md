# P.R.I.S.M - Physics-Ray-tracing-Interactive-Softbody-simulation-Module

## 1_JGN: Ogre-Next 엔진 통합

이 디렉토리는 Ogre-Next 엔진과 해당 의존성 라이브러리를 포함하고 있습니다.

### 빌드 수정 사항 (2026-02-06 적용)

Windows(MSVC) 환경에서 발생하던 컴파일 및 링커 오류를 해결하기 위해 원본 소스 코드에 다음 수정 사항을 적용했습니다:

1.  **소스 코드 문법 오류 수정 (`OgreIrradianceField.cpp`)**:
    *   **문제**: 주석에 포함된 수학 특수 문자(윗첨자 `2ⁿ` 등)로 인해 MSVC 컴파일러가 줄바꿈을 잘못 해석하여, 다음 줄의 `totalNumProbes` 변수 선언까지 주석으로 처리해버리는 버그가 있었습니다.
    *   **해결**: 특수 문자를 표준 ASCII 문자(`2^n`)로 교체하고, 변수 선언문을 주석 라인과 확실히 분리했습니다.

2.  **CMake 의존성 경로 해결 (`Dependencies.cmake`)**:
    *   **문제**: 빌드 시스템이 `FreeImage.h` 위치를 찾을 때 `${FreeImage_INCLUDE_DIRS}`(복수형) 변수를 참조했으나, 실제 경로는 `${FreeImage_INCLUDE_DIR}`(단수형)에 저장되어 있어 헤더를 찾지 못하는 문제가 있었습니다.
    *   **해결**: `include_directories` 호출 시 두 변수를 모두 참조하도록 수정하여 헤더 경로가 정확히 잡히도록 했습니다.

3.  **저장소 최적화 (`.gitignore`)**:
    *   **해결**: 10GB 이상의 대용량 빌드 결과물(`.obj`, `.pdb`, `.lib` 등)이 Git에 올라가지 않도록 최상위 `.gitignore`를 설정했습니다. 이를 통해 소스 코드는 모두 포함하면서 저장소 용량은 가벼운 상태를 유지했습니다.

### 새로운 환경에서 빌드하는 방법

1.  **의존성 라이브러리 빌드**:
    *   `Project/1_JGN/ogre-next-deps` 폴더로 이동합니다.
    *   `build` 폴더를 생성하고 CMake를 실행한 후, 생성된 솔루션을 **Debug**와 **Release** 모드에서 각각 빌드합니다.
2.  **Ogre-Next 빌드**:
    *   `Project/1_JGN/ogre-next` 폴더로 이동합니다.
    *   `build` 폴더를 생성하고 CMake를 실행합니다. (이전 단계에서 빌드한 의존성 라이브러리를 자동으로 찾아냅니다.)
    *   생성된 솔루션을 열어 빌드를 진행합니다.
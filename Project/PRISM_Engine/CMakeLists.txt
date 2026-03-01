cmake_minimum_required(VERSION 3.20)
project(PRISM_Engine CXX)

# C++ 17 표준 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg 모듈 경로 추가
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/ogre-next")

# OGRE 및 SDL2 패키지 찾기
find_package(OGRE REQUIRED COMPONENTS Main HlmsPbs HlmsUnlit)
find_package(SDL2 CONFIG REQUIRED)

# 실행 파일 생성
add_executable(${PROJECT_NAME} src/main.cpp)


set(OGRE_INCLUDE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include/OGRE-Next")
target_include_directories(${PROJECT_NAME} PRIVATE
    "${OGRE_INCLUDE_ROOT}"
    "${OGRE_INCLUDE_ROOT}/Hlms/Pbs"
    "${OGRE_INCLUDE_ROOT}/Hlms/Unlit"
    "${OGRE_INCLUDE_ROOT}/Hlms/Common"
)

# OgreNext 라이브러리 링크 (OGRE_LIBRARIES에 HlmsPbs/HlmsUnlit가 포함되지 않으므로 별도 추가)
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OGRE_LIBRARIES}
    ${OGRE_HlmsPbs_LIBRARY}
    ${OGRE_HlmsUnlit_LIBRARY}
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    SDL2::SDL2
)

# vcpkg 경로 (Debug/Release 분기)
set(VCPKG_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")

# OGRE 렌더 시스템 플러그인 DLL — vcpkg가 자동 복사하지 않으므로 직접 복사
set(OGRE_PLUGIN_DLLS RenderSystem_Vulkan RenderSystem_Direct3D11 RenderSystem_GL3Plus)
foreach(DLL ${OGRE_PLUGIN_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${VCPKG_BIN_DIR}/debug/bin/${DLL}_d.dll,${VCPKG_BIN_DIR}/bin/${DLL}.dll>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying ${DLL}..."
    )
endforeach()

# vulkan-1.dll (Debug suffix 없음)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${VCPKG_BIN_DIR}/debug/bin/vulkan-1.dll,${VCPKG_BIN_DIR}/bin/vulkan-1.dll>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying vulkan-1.dll..."
)

# OGRE HLMS 셰이더 소스 복사 (런타임에 GPU 셰이더 컴파일용)
# VCPKG_ROOT 환경 변수를 사용 — 빌드 전 VCPKG_ROOT 설정 필수 (빌드 가이드 참조)
set(OGRE_HLMS_SOURCE "$ENV{VCPKG_ROOT}/buildtrees/ogre-next/src/v3.0.0-3d16ad2943.clean/Samples/Media/Hlms")

# 만약 위 경로가 존재하지 않으면 vcpkg_installed 내의 경로를 시도해볼 수 있으나, 
# 일반적으로 vcpkg는 Media 폴더를 share에 복사하지 않는 경우가 많음.
if(NOT EXISTS "${OGRE_HLMS_SOURCE}")
    message(WARNING "OGRE_HLMS_SOURCE path not found: ${OGRE_HLMS_SOURCE}. HLMS shaders might be missing!")
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${OGRE_HLMS_SOURCE}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Media/Hlms"
    COMMENT "Copying HLMS shaders to Media/Hlms/..."
)

# PBS LTC Matrix DDS 텍스처 복사 (HlmsPbs::loadLtcMatrix() 런타임 로드용)
set(OGRE_COMMON_DDS "$ENV{VCPKG_ROOT}/buildtrees/ogre-next/src/v3.0.0-3d16ad2943.clean/Samples/Media/2.0/scripts/materials/Common")
foreach(DDS ltcMatrix0.dds ltcMatrix1.dds brtfLutDfg.dds)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Media/Common"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OGRE_COMMON_DDS}/${DDS}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Media/Common/${DDS}"
        COMMENT "Copying ${DDS} to Media/Common/..."
    )
endforeach()

# bunny.obj 복사 (main.cpp에서 상대 경로로 로드 가능하도록)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/bunny.obj"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/bunny.obj"
    COMMENT "Copying bunny.obj to build directory..."
)
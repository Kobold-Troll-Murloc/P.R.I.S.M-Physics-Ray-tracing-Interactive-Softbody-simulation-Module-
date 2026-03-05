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
find_package(Vulkan REQUIRED)

# 실행 파일 생성
add_executable(${PROJECT_NAME} 
    src/main.cpp
    src/PrismRTPipeline.h
    src/PrismRTPipeline.cpp
    src/PrismCompositorPass.h
    src/PrismCompositorPass.cpp
)


set(OGRE_INCLUDE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include/OGRE-Next")
set(OGRE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../1_JGN/ogre-next")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${OGRE_SOURCE_ROOT}/OgreMain/include"
    "${OGRE_SOURCE_ROOT}/OgreMain/include/Compositor"
    "${OGRE_SOURCE_ROOT}/OgreMain/include/Compositor/Pass"
    "${OGRE_SOURCE_ROOT}/OgreMain/include/Vao"
    "${OGRE_SOURCE_ROOT}/Components/Hlms/Pbs/include"
    "${OGRE_SOURCE_ROOT}/Components/Hlms/Unlit/include"
    "${OGRE_SOURCE_ROOT}/Components/Hlms/Common/include"
    "${OGRE_SOURCE_ROOT}/RenderSystems/Vulkan/include"
    "${OGRE_SOURCE_ROOT}/build/include" # For OgreBuildSettings.h
    "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include" # For other dependencies
)

# Ogre-Next Libraries from 1_JGN build
set(OGRE_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../1_JGN/ogre-next/build/lib/Debug")
set(OGRE_BIN_DIR_1_JGN "${CMAKE_CURRENT_SOURCE_DIR}/../1_JGN/ogre-next/build/bin/Debug")

target_link_libraries(${PROJECT_NAME} PRIVATE
    "${OGRE_LIB_DIR}/OgreNextMain_d.lib"
    "${OGRE_LIB_DIR}/OgreNextHlmsPbs_d.lib"
    "${OGRE_LIB_DIR}/OgreNextHlmsUnlit_d.lib"
    "${OGRE_LIB_DIR}/RenderSystem_Vulkan_d.lib"
    Vulkan::Vulkan
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    SDL2::SDL2
)

# vcpkg path for other dependencies (SDL2, etc)
set(VCPKG_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")

# Copy DLLs from 1_JGN Ogre build
set(OGRE_PLUGIN_DLLS RenderSystem_Vulkan RenderSystem_Direct3D11 RenderSystem_GL3Plus OgreNextMain)
foreach(DLL ${OGRE_PLUGIN_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OGRE_BIN_DIR_1_JGN}/${DLL}_d.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying ${DLL}_d.dll from 1_JGN..."
    )
endforeach()

# Copy RT shaders
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMENT "Copying PRISM RT shaders to build directory..."
)

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
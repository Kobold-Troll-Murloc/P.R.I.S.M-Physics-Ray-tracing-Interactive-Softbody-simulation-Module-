cmake_minimum_required(VERSION 3.20)
project(PRISM_Engine CXX)

# C++ 17 표준 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OGRE 및 SDL2 패키지 찾기
find_package(OGRE-Next REQUIRED COMPONENTS Main HlmsPbs HlmsUnlit)
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

# Ogre-Next 및 기타 라이브러리 링크
target_link_libraries(${PROJECT_NAME} PRIVATE
    OGRE-Next::OgreMain
    OGRE-Next::OgreHlmsPbs
    OGRE-Next::OgreHlmsUnlit
    Vulkan::Vulkan
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    SDL2::SDL2
)

# vcpkg 경로 (DLL 복사용)
set(VCPKG_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")

# Copy DLLs
set(OGRE_PLUGIN_DLLS RenderSystem_Vulkan RenderSystem_Direct3D11 RenderSystem_GL3Plus OgreNextMain)
foreach(DLL ${OGRE_PLUGIN_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${VCPKG_BIN_DIR}/debug/bin/${DLL}_d.dll,${VCPKG_BIN_DIR}/bin/${DLL}.dll>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying ${DLL} from vcpkg..."
    )
endforeach()

# Copy vulkan-1.dll
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${VCPKG_BIN_DIR}/debug/bin/vulkan-1.dll,${VCPKG_BIN_DIR}/bin/vulkan-1.dll>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying vulkan-1.dll..."
)

# Copy RT shaders
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMENT "Copying PRISM RT shaders to build directory..."
)

# Copy bunny.obj
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/bunny.obj"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/bunny.obj"
    COMMENT "Copying bunny.obj to build directory..."
)

# HLMS Media 복사 (vcpkg buildtrees에서 소스 가져오기)
# 참고: vcpkg buildtrees 경로는 환경마다 다를 수 있으므로 Git 저장소에 Media를 포함하는 것을 권장합니다.
set(OGRE_BUILD_SRC "${CMAKE_CURRENT_BINARY_DIR}/../vcpkg_installed/vcpkg/buildtrees/ogre-next/src")
# 여기서는 편의상 프로젝트 루트의 Media 폴더를 사용하거나 buildtrees를 참조하도록 가이드합니다.

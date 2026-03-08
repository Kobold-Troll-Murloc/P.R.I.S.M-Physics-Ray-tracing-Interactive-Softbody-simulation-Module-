cmake_minimum_required(VERSION 3.20)
project(PRISM_Engine CXX)

# C++ 17 표준 사용
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL2 및 Vulkan 패키지 찾기
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

# [PRISM] Manual Ogre Paths from 1_JGN (To bypass vcpkg build issues for now)
set(OGRE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../1_JGN/ogre-next")
set(OGRE_BUILD_ROOT "${OGRE_SOURCE_ROOT}/build")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${OGRE_SOURCE_ROOT}/OgreMain/include"
    "${OGRE_SOURCE_ROOT}/OgreMain/include/Compositor"
    "${OGRE_SOURCE_ROOT}/OgreMain/include/Compositor/Pass"
    "${OGRE_SOURCE_ROOT}/OgreMain/include/Vao"
    "${OGRE_SOURCE_ROOT}/Components/Hlms/Pbs/include"
    "${OGRE_SOURCE_ROOT}/Components/Hlms/Unlit/include"
    "${OGRE_SOURCE_ROOT}/Components/Hlms/Common/include"
    "${OGRE_SOURCE_ROOT}/RenderSystems/Vulkan/include"
    "${OGRE_BUILD_ROOT}/include" # For OgreBuildSettings.h
    "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include" # Other deps
)

set(OGRE_LIB_DIR "${OGRE_BUILD_ROOT}/lib/Debug")
target_link_libraries(${PROJECT_NAME} PRIVATE
    "${OGRE_LIB_DIR}/OgreNextMain_d.lib"
    "${OGRE_LIB_DIR}/OgreNextHlmsPbs_d.lib"
    "${OGRE_LIB_DIR}/OgreNextHlmsUnlit_d.lib"
    "${OGRE_LIB_DIR}/RenderSystem_Vulkan_d.lib"
    Vulkan::Vulkan
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    SDL2::SDL2
)

# Copy DLLs from 1_JGN
set(OGRE_BIN_DIR_1_JGN "${OGRE_BUILD_ROOT}/bin/Debug")
set(OGRE_PLUGIN_DLLS RenderSystem_Vulkan RenderSystem_Direct3D11 RenderSystem_GL3Plus OgreNextMain)
foreach(DLL ${OGRE_PLUGIN_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OGRE_BIN_DIR_1_JGN}/${DLL}_d.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying ${DLL}_d.dll from 1_JGN..."
    )
endforeach()

# [PRISM] Shader Compilation
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(SHADER_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/raygenbsdf.rgen"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/miss.rmiss"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/closesthitbsdf.rchit"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.rmiss"
)

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    get_filename_component(SHADER_EXT ${SHADER} EXT)
    set(SPV_FILE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_NAME}.spv")
    
    # [PRISM] Map extensions to glslang stages
    if(SHADER_EXT STREQUAL ".rgen")
        set(STAGE rgen)
    elseif(SHADER_EXT STREQUAL ".rmiss")
        set(STAGE rmiss)
    elseif(SHADER_EXT STREQUAL ".rchit")
        set(STAGE rchit)
    else()
        set(STAGE vert)
    endif()

    add_custom_command(
        OUTPUT ${SPV_FILE}
        COMMAND ${GLSLANG_VALIDATOR} --target-env vulkan1.2 -S ${STAGE} -V ${SHADER} -o ${SPV_FILE}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME} to SPIR-V (stage: ${STAGE})..."
    )
    list(APPEND SHADER_SPV_FILES ${SPV_FILE})
endforeach()

# [PRISM] Ensure shaders are compiled before copying
add_custom_target(PRISM_Shaders DEPENDS ${SHADER_SPV_FILES})
add_dependencies(${PROJECT_NAME} PRISM_Shaders)

# Copy RT shaders (Updated to copy compiled .spv files)
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

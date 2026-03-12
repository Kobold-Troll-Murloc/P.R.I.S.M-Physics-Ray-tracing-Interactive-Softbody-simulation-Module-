vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        vulkan   OGRE_BUILD_RENDERSYSTEM_VULKAN
        d3d11    OGRE_BUILD_RENDERSYSTEM_D3D11
        gl3plus  OGRE_BUILD_RENDERSYSTEM_GL3PLUS
)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OGRECave/ogre-next
    REF v3.0.0
    SHA512 2ef8f16517c96cc7ddb31986857e4d0002e33c2eeff845b4af0b8e5848c3e92289dc3b10ededbe66fb63ef6234cbee88ed513466182bd4e70d710d0507f98418
    HEAD_REF master
)

# [PRISM] Surgery via PowerShell Script (Most robust method)
vcpkg_execute_required_process(
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_LIST_DIR}/surgery.ps1" "${SOURCE_PATH}"
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME "surgery-${TARGET_TRIPLET}"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DOGRE_BUILD_SAMPLES2=OFF
        -DOGRE_INSTALL_SAMPLES2=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/OGRE-Next)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
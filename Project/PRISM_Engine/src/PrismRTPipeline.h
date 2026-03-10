#pragma once

#include <vulkan/vulkan.h>
#include <OgreVulkanRenderSystem.h>
#include <OgreVulkanDevice.h>
#include <OgreVector3.h>
#include <OgreMatrix4.h>
#include <array>
#include <unordered_map>
#include <string>
#include <vector>

namespace Prism {

    // [2_LSM 호환] raygenbsdf.rgen / closesthitbsdf.rchit 과 레이아웃 일치 (std140)
    struct GpuLight {
        float position[3];
        float intensity;
        float color[3];
        int   enabled;
    };

    struct CameraUBO {
        float viewInverse[16];
        float projInverse[16];
        float cameraPos[3];
        int   frameCount;       // offset 144
        GpuLight lights[3];     // offset 160 (Light struct size is 32)
        int   lightCount;       // offset 256
        float padding[3];       // offset 260
    };

    // closesthitbsdf.rchit 에서 직접 읽는 정점 구조체 (8 floats = 32 bytes)
    struct RTVertex {
        float pos[3];
        float pad1;
        float normal[3];
        float pad2;
    };

    // closesthitbsdf.rchit: layout(binding=4) buffer ObjDescBuffer
    struct ObjDesc {
        uint64_t vertexAddress;
        uint64_t indexAddress;
    };

    // closesthitbsdf.rchit: layout(binding=3) buffer InstanceMaterials
    struct InstanceMaterial {
        float albedo[4];     // rgb: 색상, a: 투명도
        float pbrParams1[4]; // x: emissive강도, y: roughness, z: metallic, w: padding
        float pbrParams2[4]; // x: specTrans, y: ior, z,w: padding
    };

    struct RTObject {
        VkAccelerationStructureKHR blas;
        VkDeviceAddress blasAddress;
        Ogre::Matrix4 transform;
        uint32_t customIndex;
    };

    // 메시 하나당 BLAS + Vertex/Index 버퍼
    struct MeshBlas {
        VkBuffer       vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkBuffer       indexBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory  = VK_NULL_HANDLE;
        VkBuffer       blasBuffer   = VK_NULL_HANDLE;
        VkDeviceMemory blasMemory   = VK_NULL_HANDLE;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        VkDeviceAddress blasAddress     = 0;
        VkDeviceAddress vertexAddress   = 0;
        VkDeviceAddress indexAddress    = 0;
    };

    class RTPipeline {
    public:
        RTPipeline(Ogre::VulkanRenderSystem* rs);
        ~RTPipeline();

        void initialize();
        void cleanup();

        // Acceleration Structure Management
        // meshKey로 캐시 확인 후 재사용, 반환값은 mMeshBLASes 인덱스
        uint32_t buildBLAS(Ogre::MeshPtr mesh, const std::string& meshKey);
        void buildTLAS(const std::vector<RTObject>& objects);

        // RT Execution
        void recordRayTracingCommands(VkCommandBuffer cmdBuf,
                                    VkDescriptorSet descriptorSet,
                                    uint32_t width, uint32_t height);

        // Getters
        VkAccelerationStructureKHR getTLAS() const { return mTopLevelAS; }
        VkPipeline getPipeline() const { return mRTPipeline; }
        VkDeviceAddress getBLASAddress(uint32_t meshIdx) const { return mMeshBLASes[meshIdx].blasAddress; }
        VkDeviceAddress getMeshVertexAddress(uint32_t meshIdx) const { return mMeshBLASes[meshIdx].vertexAddress; }
        VkDeviceAddress getMeshIndexAddress(uint32_t meshIdx) const { return mMeshBLASes[meshIdx].indexAddress; }
        VkDescriptorSet getDescriptorSet() const { return mDescriptorSet; }
        VkPipelineLayout getPipelineLayout() const { return mPipelineLayout; }
        VkImage getStorageImage() const { return mStorageImage; }
        uint32_t getRTWidth() const { return mRTWidth; }
        uint32_t getRTHeight() const { return mRTHeight; }

        // 매 프레임 카메라 UBO 업데이트
        void updateCameraUBO(const Ogre::Matrix4& view, const Ogre::Matrix4& proj,
                             const Ogre::Vector3& camPos, int frameCount);

        // 씬 버퍼 생성 (buildTLAS 이후 호출)
        void createSceneBuffers(const std::vector<InstanceMaterial>& materials,
                                const std::vector<ObjDesc>& objDescs);

        // Descriptor Set 구성 (createSceneBuffers 이후 호출)
        void createDescriptorSet();

    private:
        Ogre::VulkanRenderSystem* mRenderSystem;
        Ogre::VulkanDevice* mDevice;

        VkCommandPool mCommandPool = VK_NULL_HANDLE;

        VkPipeline mRTPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        // 다중 BLAS (메시 캐시)
        std::unordered_map<std::string, uint32_t> mMeshKeyToIndex;
        std::vector<MeshBlas> mMeshBLASes;

        // TLAS
        VkAccelerationStructureKHR mTopLevelAS = VK_NULL_HANDLE;
        VkBuffer mTLASBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mTLASMemory = VK_NULL_HANDLE;
        VkBuffer mInstanceBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mInstanceMemory = VK_NULL_HANDLE;

        // Camera UBO (Binding 2)
        VkBuffer       mCameraUBOBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mCameraUBOMemory = VK_NULL_HANDLE;
        CameraUBO      mCameraUBOData{};

        // ObjDesc Buffer (Binding 4)
        VkBuffer       mObjDescBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mObjDescMemory = VK_NULL_HANDLE;

        // Material Buffer (Binding 3)
        VkBuffer       mMaterialBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mMaterialMemory = VK_NULL_HANDLE;

        // Descriptor
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet  mDescriptorSet  = VK_NULL_HANDLE;
        VkDescriptorSetLayout mDescriptorSetLayout = VK_NULL_HANDLE;

        // Depth dummy (Binding 5)
        VkImage        mDummyDepthImage  = VK_NULL_HANDLE;
        VkDeviceMemory mDummyDepthMemory = VK_NULL_HANDLE;
        VkImageView    mDummyDepthView   = VK_NULL_HANDLE;
        VkSampler      mDummySampler     = VK_NULL_HANDLE;

        // RT Output Images
        VkImage        mStorageImage = VK_NULL_HANDLE;
        VkDeviceMemory mStorageImageMemory = VK_NULL_HANDLE;
        VkImageView    mStorageImageView = VK_NULL_HANDLE;

        // Accum Image (Binding 6)
        VkImage        mAccumImage = VK_NULL_HANDLE;
        VkDeviceMemory mAccumImageMemory = VK_NULL_HANDLE;
        VkImageView    mAccumImageView = VK_NULL_HANDLE;

        uint32_t mRTWidth = 1280;
        uint32_t mRTHeight = 720;

        // Shader Binding Table
        VkBuffer       mSBTBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mSBTMemory = VK_NULL_HANDLE;
        VkStridedDeviceAddressRegionKHR mRaygenRegion{};
        VkStridedDeviceAddressRegionKHR mMissRegion{};
        VkStridedDeviceAddressRegionKHR mHitRegion{};
        VkStridedDeviceAddressRegionKHR mCallableRegion{};

        void createRTPipeline();
        void createSBT();
        void createRTImages();

        // Single-time command helpers
        VkCommandBuffer beginSingleTimeCommands();
        void endSingleTimeCommands(VkCommandBuffer cmdBuf);

        // Buffer utility
        void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties, VkBuffer& buffer,
                         VkDeviceMemory& bufferMemory);
        uint64_t getBufferDeviceAddress(VkBuffer buffer);
    };

}

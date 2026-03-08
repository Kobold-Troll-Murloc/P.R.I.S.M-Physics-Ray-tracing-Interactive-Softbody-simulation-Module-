#pragma once

#include <vulkan/vulkan.h>
#include <OgreVulkanRenderSystem.h>
#include <OgreVulkanDevice.h>
#include <OgreVector3.h>
#include <OgreMatrix4.h>
#include <array>

namespace Prism {

    // raygenbsdf.rgen / closesthitbsdf.rchit 과 레이아웃 일치 (std140)
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
        float pad0;             // vec3 alignment padding (offset 140)
        int   frameCount;       // (offset 144)
        float pad1[3];          // struct alignment padding (offset 148-159)
        GpuLight lights[3];     // (offset 160)
        int   lightCount;       // (offset 256)
        float padding[3];       // (offset 260)
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
        float albedo[4];    // rgb: 색상, a: 투명도
        float pbrParams1[4]; // x: emissive강도, y: roughness, z: metallic, w: padding
        float pbrParams2[4]; // x: specTrans, y: ior, z,w: padding
    };

    struct RTObject {
        VkAccelerationStructureKHR blas;
        VkDeviceAddress blasAddress;
        Ogre::Matrix4 transform;
        uint32_t customIndex;
    };

    class RTPipeline {
    public:
        RTPipeline(Ogre::VulkanRenderSystem* rs);
        ~RTPipeline();

        void initialize();
        void cleanup();

        // Acceleration Structure Management
        void buildBLAS(Ogre::MeshPtr mesh);
        void buildTLAS(const std::vector<RTObject>& objects);

        // RT Execution
        void recordRayTracingCommands(VkCommandBuffer cmdBuf,
                                    VkDescriptorSet descriptorSet,
                                    uint32_t width, uint32_t height);

        // Getters
        VkAccelerationStructureKHR getTLAS() const { return mTopLevelAS; }
        VkPipeline getPipeline() const { return mRTPipeline; }
        VkAccelerationStructureKHR getBLAS() const { return mBLAS; }
        VkDeviceAddress getBLASAddress() const { return mBLASAddress; }
        VkDescriptorSet getDescriptorSet() const { return mDescriptorSet; }
        VkPipelineLayout getPipelineLayout() const { return mPipelineLayout; }
        VkImage getStorageImage() const { return mStorageImage; }
        uint32_t getRTWidth() const { return mRTWidth; }
        uint32_t getRTHeight() const { return mRTHeight; }

        // 매 프레임 카메라 UBO 업데이트
        void updateCameraUBO(const Ogre::Matrix4& view, const Ogre::Matrix4& proj,
                             const Ogre::Vector3& camPos, int frameCount);

        // 씬 버퍼 생성 (BLAS/TLAS 이후 호출)
        void createSceneBuffers(Ogre::MeshPtr mesh);

        // Descriptor Set 구성 (createSceneBuffers 이후 호출)
        void createDescriptorSet();

    private:
        Ogre::VulkanRenderSystem* mRenderSystem;
        Ogre::VulkanDevice* mDevice;

        VkCommandPool mCommandPool = VK_NULL_HANDLE;

        VkPipeline mRTPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        // BLAS
        VkAccelerationStructureKHR mBLAS = VK_NULL_HANDLE;
        VkBuffer mBLASBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mBLASMemory = VK_NULL_HANDLE;
        VkDeviceAddress mBLASAddress = 0;

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

        // Vertex/Index GPU 주소 (ObjDesc용)
        VkBuffer       mRTVertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mRTVertexMemory = VK_NULL_HANDLE;
        VkBuffer       mRTIndexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mRTIndexMemory = VK_NULL_HANDLE;

        // Descriptor
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet  mDescriptorSet  = VK_NULL_HANDLE;
        VkDescriptorSetLayout mDescriptorSetLayout = VK_NULL_HANDLE;

        // Depth dummy (Binding 5 placeholder)
        VkImage        mDummyDepthImage  = VK_NULL_HANDLE;
        VkDeviceMemory mDummyDepthMemory = VK_NULL_HANDLE;
        VkImageView    mDummyDepthView   = VK_NULL_HANDLE;
        VkSampler      mDummySampler     = VK_NULL_HANDLE;

        // RT Output Images
        VkImage        mStorageImage = VK_NULL_HANDLE;
        VkDeviceMemory mStorageImageMemory = VK_NULL_HANDLE;
        VkImageView    mStorageImageView = VK_NULL_HANDLE;

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

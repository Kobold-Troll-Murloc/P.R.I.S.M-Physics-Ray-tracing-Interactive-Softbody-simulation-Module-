#pragma once

#include <vulkan/vulkan.h>
#include <OgreVulkanRenderSystem.h>
#include <OgreVulkanDevice.h>
#include <OgreVector3.h>
#include <OgreMatrix4.h>

namespace Prism {

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

        // Getters for Vulkan handles
        VkAccelerationStructureKHR getTLAS() const { return mTopLevelAS; }
        VkPipeline getPipeline() const { return mRTPipeline; }
        
    private:
        Ogre::VulkanRenderSystem* mRenderSystem;
        Ogre::VulkanDevice* mDevice;

        VkPipeline mRTPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        
        VkAccelerationStructureKHR mBLAS = VK_NULL_HANDLE;
        VkBuffer mBLASBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mBLASMemory = VK_NULL_HANDLE;

        VkAccelerationStructureKHR mTopLevelAS = VK_NULL_HANDLE;
        VkBuffer mTLASBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mTLASMemory = VK_NULL_HANDLE;

        // Shader Binding Table regions
        VkStridedDeviceAddressRegionKHR mRaygenRegion{};
        VkStridedDeviceAddressRegionKHR mMissRegion{};
        VkStridedDeviceAddressRegionKHR mHitRegion{};
        VkStridedDeviceAddressRegionKHR mCallableRegion{};

        void createRTPipeline();
        void createSBT();
        
        // Utility for buffer creation (bridging Ogre and Raw Vulkan)
        void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                         VkMemoryPropertyFlags properties, VkBuffer& buffer, 
                         VkDeviceMemory& bufferMemory);
        uint64_t getBufferDeviceAddress(VkBuffer buffer);
    };

}

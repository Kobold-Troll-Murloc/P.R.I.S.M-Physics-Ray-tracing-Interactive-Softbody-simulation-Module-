#include "PrismRTPipeline.h"
#include <OgreVulkanRenderSystem.h>
#include <OgreVulkanDevice.h>
#include <OgreLogManager.h>
#include <OgreMesh2.h>
#include <OgreSubMesh2.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>
#include <Vao/OgreVertexBufferPacked.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreAsyncTicket.h>
#include <fstream>
#include <vector>

namespace Prism {

    // RT Extension Function Pointers
    PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetASBuildSizes = nullptr;
    PFN_vkCreateAccelerationStructureKHR pfnCreateAS = nullptr;
    PFN_vkDestroyAccelerationStructureKHR pfnDestroyAS = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuildAS = nullptr;
    PFN_vkGetBufferDeviceAddressKHR pfnGetBufferAddress = nullptr;

    RTPipeline::RTPipeline(Ogre::VulkanRenderSystem* rs) 
        : mRenderSystem(rs), mDevice(nullptr) 
    {
    }

    RTPipeline::~RTPipeline() {
        cleanup();
    }

    void RTPipeline::initialize() {
        if (!mRenderSystem) return;
        mDevice = mRenderSystem->getVulkanDevice();

        if (mDevice) {
            // [PRISM] Verify that the RT extensions were successfully enabled
            if (!mDevice->mDeviceExtraFeatures.rayTracingPipeline || 
                !mDevice->mDeviceExtraFeatures.accelerationStructure) 
            {
                Ogre::LogManager::getSingleton().logMessage("PRISM ERROR: RT features NOT supported/enabled on this device.");
                return;
            }

            VkDevice device = mDevice->mDevice;
            pfnGetASBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
            pfnCreateAS = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
            pfnDestroyAS = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR");
            pfnCmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR");
            pfnGetBufferAddress = (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR");

            if (!pfnGetASBuildSizes || !pfnCreateAS || !pfnDestroyAS || !pfnCmdBuildAS || !pfnGetBufferAddress) {
                Ogre::LogManager::getSingleton().logMessage("PRISM ERROR: Some Vulkan RT function pointers failed to load!");
            } else {
                Ogre::LogManager::getSingleton().logMessage("PRISM: All Vulkan RT Function Pointers loaded.");
            }
            try {
                createRTPipeline();
            } catch (const std::exception& e) {
                Ogre::LogManager::getSingleton().logMessage("PRISM ERROR: Failed to initialize RT Pipeline: " + Ogre::String(e.what()));
            }
        }
    }

    void RTPipeline::cleanup() {
        if (mDevice && mDevice->mDevice != VK_NULL_HANDLE) {
            VkDevice device = mDevice->mDevice;
            if (mPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, mPipelineLayout, nullptr);
            if (mRTPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, mRTPipeline, nullptr);
            
            if (pfnDestroyAS) {
                if (mBLAS != VK_NULL_HANDLE) pfnDestroyAS(device, mBLAS, nullptr);
                if (mTopLevelAS != VK_NULL_HANDLE) pfnDestroyAS(device, mTopLevelAS, nullptr);
            }
            
            if (mBLASBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, mBLASBuffer, nullptr);
            if (mBLASMemory != VK_NULL_HANDLE) vkFreeMemory(device, mBLASMemory, nullptr);
            if (mTLASBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, mTLASBuffer, nullptr);
            if (mTLASMemory != VK_NULL_HANDLE) vkFreeMemory(device, mTLASMemory, nullptr);
        }
    }

    uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("PRISM: Failed to find suitable memory type!");
    }

    void RTPipeline::createRTPipeline() {
        if (!mDevice) return;
        VkDevice device = mDevice->mDevice;

        auto loadShader = [&](const std::string& path) -> VkShaderModule {
            std::ifstream file(path, std::ios::ate | std::ios::binary);
            if (!file.is_open()) {
                Ogre::LogManager::getSingleton().logMessage("PRISM ERROR: Could not open shader file: " + path);
                return VK_NULL_HANDLE;
            }
            size_t size = (size_t)file.tellg();
            std::vector<char> buffer(size);
            file.seekg(0); file.read(buffer.data(), size);

            VkShaderModuleCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            ci.codeSize = buffer.size();
            ci.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
            VkShaderModule sm;
            if (vkCreateShaderModule(device, &ci, nullptr, &sm) != VK_SUCCESS) {
                Ogre::LogManager::getSingleton().logMessage("PRISM ERROR: Failed to create ShaderModule for: " + path);
                return VK_NULL_HANDLE;
            }
            return sm;
        };

        VkShaderModule rgen = loadShader("shaders/raygenbsdf.rgen.spv");
        VkShaderModule miss = loadShader("shaders/miss.rmiss.spv");
        VkShaderModule chit = loadShader("shaders/closesthitbsdf.rchit.spv");

        if (rgen == VK_NULL_HANDLE || miss == VK_NULL_HANDLE || chit == VK_NULL_HANDLE) {
            Ogre::LogManager::getSingleton().logMessage("PRISM ERROR: Skipping RT Pipeline creation due to shader failures.");
            if (rgen) vkDestroyShaderModule(device, rgen, nullptr);
            if (miss) vkDestroyShaderModule(device, miss, nullptr);
            if (chit) vkDestroyShaderModule(device, chit, nullptr);
            return;
        }

        std::vector<VkDescriptorSetLayoutBinding> b;
        auto add = [&](uint32_t i, VkDescriptorType t, VkShaderStageFlags s) {
            VkDescriptorSetLayoutBinding bind{};
            bind.binding = i; bind.descriptorType = t; bind.descriptorCount = 1; bind.stageFlags = s;
            b.push_back(bind);
        };

        add(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        add(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        add(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        add(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        add(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        add(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        add(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR);

        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = (uint32_t)b.size(); lci.pBindings = b.data();
        VkDescriptorSetLayout dsl;
        vkCreateDescriptorSetLayout(device, &lci, nullptr, &dsl);

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
        vkCreatePipelineLayout(device, &plci, nullptr, &mPipelineLayout);

        Ogre::LogManager::getSingleton().logMessage("PRISM: RT Pipeline Layout created.");

        // RT Pipeline Creation
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_RAYGEN_BIT_KHR, rgen, "main"});
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MISS_BIT_KHR, miss, "main"});
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, chit, "main"});

        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        // Raygen group
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR});
        // Miss group
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR});
        // Hit group
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 2, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR});

        VkRayTracingPipelineCreateInfoKHR pipeInfo{};
        pipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        pipeInfo.stageCount = (uint32_t)stages.size();
        pipeInfo.pStages = stages.data();
        pipeInfo.groupCount = (uint32_t)groups.size();
        pipeInfo.pGroups = groups.data();
        pipeInfo.maxPipelineRayRecursionDepth = 1;
        pipeInfo.layout = mPipelineLayout;

        auto vkCreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR");
        if (vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &mRTPipeline) != VK_SUCCESS) {
            throw std::runtime_error("PRISM: Failed to create RT pipeline");
        }

        vkDestroyShaderModule(device, rgen, nullptr);
        vkDestroyShaderModule(device, miss, nullptr);
        vkDestroyShaderModule(device, chit, nullptr);
    }

    void RTPipeline::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                                 VkMemoryPropertyFlags properties, VkBuffer& buffer, 
                                 VkDeviceMemory& bufferMemory) {
        if (!mDevice) return;
        VkDevice device = mDevice->mDevice;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) throw std::runtime_error("PRISM: failed to create buffer!");

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, buffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(mDevice->mPhysicalDevice, memReqs.memoryTypeBits, properties);

        // [PRISM] CRITICAL: Must use VkMemoryAllocateFlagsInfo for BufferDeviceAddress
        VkMemoryAllocateFlagsInfo allocFlags{};
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            allocInfo.pNext = &allocFlags;
        }

        if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) throw std::runtime_error("PRISM: failed to allocate memory!");
        vkBindBufferMemory(device, buffer, bufferMemory, 0);
    }

    uint64_t RTPipeline::getBufferDeviceAddress(VkBuffer buffer) {
        VkBufferDeviceAddressInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer;
        return pfnGetBufferAddress(mDevice->mDevice, &info);
    }

    void RTPipeline::buildBLAS(Ogre::MeshPtr mesh) {
        if (!mDevice || !mesh) return;
        
        if (!pfnGetASBuildSizes || !pfnCreateAS || !pfnCmdBuildAS || !pfnGetBufferAddress) {
            Ogre::LogManager::getSingleton().logMessage("PRISM: BLAS Building skipped - RT extensions not available on this device/driver.");
            return;
        }

        Ogre::LogManager::getSingleton().logMessage("PRISM: Building BLAS for " + mesh->getName());

        Ogre::SubMesh* subMesh = mesh->getSubMesh(0);
        Ogre::VertexArrayObject* vao = subMesh->mVao[0][0];
        Ogre::VertexBufferPacked* vBuf = vao->getVertexBuffers()[0];
        Ogre::IndexBufferPacked* iBuf = vao->getIndexBuffer();

        VkBuffer vBuffer, iBuffer;
        VkDeviceMemory vMem, iMem;
        
        createBuffer(vBuf->getTotalSizeBytes(), 
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 
                     vBuffer, vMem);
        
        createBuffer(iBuf->getTotalSizeBytes(), 
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 
                     iBuffer, iMem);

        // Upload Data
        void* data;
        Ogre::AsyncTicketPtr vTicket = vBuf->readRequest(0, vBuf->getNumElements());
        const void* ogreVData = vTicket->map();
        vkMapMemory(mDevice->mDevice, vMem, 0, vBuf->getTotalSizeBytes(), 0, &data);
        memcpy(data, ogreVData, vBuf->getTotalSizeBytes());
        vkUnmapMemory(mDevice->mDevice, vMem);
        vTicket->unmap();

        Ogre::AsyncTicketPtr iTicket = iBuf->readRequest(0, iBuf->getNumElements());
        const void* ogreIData = iTicket->map();
        vkMapMemory(mDevice->mDevice, iMem, 0, iBuf->getTotalSizeBytes(), 0, &data);
        memcpy(data, ogreIData, iBuf->getTotalSizeBytes());
        vkUnmapMemory(mDevice->mDevice, iMem);
        iTicket->unmap();

        // BLAS Build Geometry
        VkAccelerationStructureGeometryKHR geo{};
        geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress = getBufferDeviceAddress(vBuffer);
        geo.geometry.triangles.vertexStride = 6 * sizeof(float);
        geo.geometry.triangles.maxVertex = (uint32_t)vBuf->getNumElements();
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress = getBufferDeviceAddress(iBuffer);

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geo;

        uint32_t maxPrimCount = (uint32_t)iBuf->getNumElements() / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        pfnGetASBuildSizes(mDevice->mDevice, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxPrimCount, &sizes);

        // Create AS
        createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mBLASBuffer, mBLASMemory);

        VkAccelerationStructureCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = mBLASBuffer;
        createInfo.size = sizes.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        pfnCreateAS(mDevice->mDevice, &createInfo, nullptr, &mBLAS);

        Ogre::LogManager::getSingleton().logMessage("PRISM: BLAS Object created for " + mesh->getName());
    }

    void RTPipeline::buildTLAS(const std::vector<RTObject>& objects) {
    }

    void RTPipeline::recordRayTracingCommands(VkCommandBuffer cmdBuf, 
                                            VkDescriptorSet descriptorSet,
                                            uint32_t width, uint32_t height) {
        if (!mDevice || mRTPipeline == VK_NULL_HANDLE || descriptorSet == VK_NULL_HANDLE) return;

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mRTPipeline);
        vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
                                mPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    }

}

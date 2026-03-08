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
#include <SDL.h>
#include <fstream>
#include <vector>

namespace Prism {

    PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetASBuildSizes = nullptr;
    PFN_vkCreateAccelerationStructureKHR pfnCreateAS = nullptr;
    PFN_vkDestroyAccelerationStructureKHR pfnDestroyAS = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuildAS = nullptr;
    PFN_vkGetBufferDeviceAddressKHR pfnGetBufferAddress = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetASDeviceAddress = nullptr;

    RTPipeline::RTPipeline(Ogre::VulkanRenderSystem* rs) : mRenderSystem(rs), mDevice(nullptr) {}
    RTPipeline::~RTPipeline() { cleanup(); }

    void RTPipeline::initialize() {
        if (!mRenderSystem) return;
        mDevice = mRenderSystem->getVulkanDevice();
        if (mDevice) {
            VkDevice device = mDevice->mDevice;
            pfnGetASBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
            pfnCreateAS = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
            pfnDestroyAS = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR");
            pfnCmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR");
            pfnGetBufferAddress = (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR");
            pfnGetASDeviceAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR");

            VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = mDevice->mGraphicsQueue.mFamilyIdx;
            vkCreateCommandPool(device, &poolInfo, nullptr, &mCommandPool);

            createRTPipeline();
            createSBT();
            createRTImages();
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
            if (mInstanceBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, mInstanceBuffer, nullptr);
            if (mInstanceMemory != VK_NULL_HANDLE) vkFreeMemory(device, mInstanceMemory, nullptr);
            if (mSBTBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, mSBTBuffer, nullptr);
            if (mSBTMemory != VK_NULL_HANDLE) vkFreeMemory(device, mSBTMemory, nullptr);
            if (mDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, mDescriptorPool, nullptr);
            if (mStorageImageView != VK_NULL_HANDLE) vkDestroyImageView(device, mStorageImageView, nullptr);
            if (mStorageImage != VK_NULL_HANDLE) vkDestroyImage(device, mStorageImage, nullptr);
            if (mStorageImageMemory != VK_NULL_HANDLE) vkFreeMemory(device, mStorageImageMemory, nullptr);
            if (mCommandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, mCommandPool, nullptr);
            if (mDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, mDescriptorSetLayout, nullptr);
            if (mCameraUBOBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, mCameraUBOBuffer, nullptr);
            if (mCameraUBOMemory != VK_NULL_HANDLE) vkFreeMemory(device, mCameraUBOMemory, nullptr);
        }
    }

    uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
        }
        return 0;
    }

    void RTPipeline::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& targetBuffer, VkDeviceMemory& targetMemory) {
        VkDevice device = mDevice->mDevice;
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufferInfo, nullptr, &targetBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, targetBuffer, &memReqs);

        static VkMemoryAllocateFlagsInfo flagsInfo;
        flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flagsInfo.pNext = nullptr;
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &flagsInfo; 
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(mDevice->mPhysicalDevice, memReqs.memoryTypeBits, properties);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &targetMemory) != VK_SUCCESS) {
            throw std::runtime_error("PRISM: Memory allocation failed!");
        }
        vkBindBufferMemory(device, targetBuffer, targetMemory, 0);
    }

    uint64_t RTPipeline::getBufferDeviceAddress(VkBuffer buffer) {
        VkBufferDeviceAddressInfoKHR info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        info.buffer = buffer;
        return pfnGetBufferAddress(mDevice->mDevice, &info);
    }

    void RTPipeline::createRTPipeline() {
        VkDevice device = mDevice->mDevice;
        auto loadShader = [&](const std::string& path) -> VkShaderModule {
            std::ifstream file(path, std::ios::ate | std::ios::binary);
            if (!file.is_open()) return VK_NULL_HANDLE;
            size_t size = (size_t)file.tellg();
            std::vector<char> buf(size); file.seekg(0); file.read(buf.data(), size);
            VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = buf.size(); ci.pCode = reinterpret_cast<const uint32_t*>(buf.data());
            VkShaderModule sm; vkCreateShaderModule(device, &ci, nullptr, &sm); return sm;
        };
        VkShaderModule rgen = loadShader("shaders/raygenbsdf.rgen.spv");
        VkShaderModule miss = loadShader("shaders/miss.rmiss.spv");
        VkShaderModule chit = loadShader("shaders/closesthitbsdf.rchit.spv");
        std::vector<VkDescriptorSetLayoutBinding> b;
        auto add = [&](uint32_t i, VkDescriptorType t, VkShaderStageFlags s) {
            VkDescriptorSetLayoutBinding bind{}; bind.binding = i; bind.descriptorType = t; bind.descriptorCount = 1; bind.stageFlags = s; b.push_back(bind);
        };
        add(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        add(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        add(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lci.bindingCount = (uint32_t)b.size(); lci.pBindings = b.data();
        vkCreateDescriptorSetLayout(device, &lci, nullptr, &mDescriptorSetLayout);
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &mDescriptorSetLayout;
        vkCreatePipelineLayout(device, &plci, nullptr, &mPipelineLayout);
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_RAYGEN_BIT_KHR, rgen, "main"});
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MISS_BIT_KHR, miss, "main"});
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, chit, "main"});
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR});
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR});
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 2, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR});
        VkRayTracingPipelineCreateInfoKHR pipeInfo = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        pipeInfo.stageCount = (uint32_t)stages.size(); pipeInfo.pStages = stages.data();
        pipeInfo.groupCount = (uint32_t)groups.size(); pipeInfo.pGroups = groups.data();
        pipeInfo.maxPipelineRayRecursionDepth = 1; pipeInfo.layout = mPipelineLayout;
        auto vkCreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR");
        vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &mRTPipeline);
        vkDestroyShaderModule(device, rgen, nullptr); vkDestroyShaderModule(device, miss, nullptr); vkDestroyShaderModule(device, chit, nullptr);
    }

    void RTPipeline::buildBLAS(Ogre::MeshPtr mesh) {
        Ogre::SubMesh* subMesh = mesh->getSubMesh(0); Ogre::VertexArrayObject* vao = subMesh->mVao[0][0];
        Ogre::VertexBufferPacked* vBuf = vao->getVertexBuffers()[0]; Ogre::IndexBufferPacked* iBuf = vao->getIndexBuffer();
        
        size_t vertexCount = vBuf->getNumElements();
        size_t rtBufferSize = vertexCount * sizeof(RTVertex);
        size_t ogreStride = vBuf->getBytesPerElement() / sizeof(float); // Float 단위 스트라이드

        VkBuffer vBuffer, iBuffer; VkDeviceMemory vMem, iMem; 
        createBuffer(rtBufferSize, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, vBuffer, vMem);
        createBuffer(iBuf->getTotalSizeBytes(), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, iBuffer, iMem);
        
        Ogre::AsyncTicketPtr vTicket = vBuf->readRequest(0, vertexCount);
        const float* ogreVData = static_cast<const float*>(vTicket->map());
        void* data; vkMapMemory(mDevice->mDevice, vMem, 0, rtBufferSize, 0, &data);
        RTVertex* rtVData = static_cast<RTVertex*>(data);
        
        Ogre::LogManager::getSingleton().logMessage("[PRISM] Bunny Mesh Data Debug (First 3 vertices):");
        for (size_t i = 0; i < vertexCount; ++i) {
            rtVData[i].pos[0] = ogreVData[i * ogreStride + 0]; 
            rtVData[i].pos[1] = ogreVData[i * ogreStride + 1]; 
            rtVData[i].pos[2] = ogreVData[i * ogreStride + 2]; 
            rtVData[i].pad1 = 1.0f;
            rtVData[i].normal[0] = ogreVData[i * ogreStride + 3]; 
            rtVData[i].normal[1] = ogreVData[i * ogreStride + 4]; 
            rtVData[i].normal[2] = ogreVData[i * ogreStride + 5]; 
            rtVData[i].pad2 = 1.0f;

            if (i < 3) {
                char b[256];
                sprintf(b, "  V[%zu]: P(%.2f, %.2f, %.2f) N(%.2f, %.2f, %.2f) S:%zu", 
                        i, rtVData[i].pos[0], rtVData[i].pos[1], rtVData[i].pos[2],
                        rtVData[i].normal[0], rtVData[i].normal[1], rtVData[i].normal[2], ogreStride);
                Ogre::LogManager::getSingleton().logMessage(b);
            }
        }
        vkUnmapMemory(mDevice->mDevice, vMem); vTicket->unmap();

        Ogre::AsyncTicketPtr iTicket = iBuf->readRequest(0, iBuf->getNumElements());
        const void* ogreIData = iTicket->map();
        vkMapMemory(mDevice->mDevice, iMem, 0, iBuf->getTotalSizeBytes(), 0, &data);
        memcpy(data, ogreIData, iBuf->getTotalSizeBytes());
        vkUnmapMemory(mDevice->mDevice, iMem); iTicket->unmap();

        VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR }; 
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; geo.geometry.triangles.vertexData.deviceAddress = getBufferDeviceAddress(vBuffer); 
        geo.geometry.triangles.vertexStride = sizeof(RTVertex); geo.geometry.triangles.maxVertex = (uint32_t)vertexCount; 
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32; geo.geometry.triangles.indexData.deviceAddress = getBufferDeviceAddress(iBuffer);
        
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR; buildInfo.geometryCount = 1; buildInfo.pGeometries = &geo;
        
        uint32_t maxPrimCount = (uint32_t)iBuf->getNumElements() / 3; VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pfnGetASBuildSizes(mDevice->mDevice, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxPrimCount, &sizes);
        
        createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mBLASBuffer, mBLASMemory);
        VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR }; ci.buffer = mBLASBuffer; ci.size = sizes.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        pfnCreateAS(mDevice->mDevice, &ci, nullptr, &mBLAS);
        
        VkBuffer scratch; VkDeviceMemory scratchMem; createBuffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratch, scratchMem);
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; buildInfo.dstAccelerationStructure = mBLAS; buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(scratch);
        VkAccelerationStructureBuildRangeInfoKHR range = { maxPrimCount, 0, 0, 0 }; const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        VkCommandBuffer cmd = beginSingleTimeCommands(); pfnCmdBuildAS(cmd, 1, &buildInfo, &pRange); endSingleTimeCommands(cmd);
        
        vkDestroyBuffer(mDevice->mDevice, scratch, nullptr); vkFreeMemory(mDevice->mDevice, scratchMem, nullptr);
        vkDestroyBuffer(mDevice->mDevice, vBuffer, nullptr); vkFreeMemory(mDevice->mDevice, vMem, nullptr);
        vkDestroyBuffer(mDevice->mDevice, iBuffer, nullptr); vkFreeMemory(mDevice->mDevice, iMem, nullptr);
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addrInfo.accelerationStructure = mBLAS; mBLASAddress = pfnGetASDeviceAddress(mDevice->mDevice, &addrInfo);
    }

    void RTPipeline::buildTLAS(const std::vector<RTObject>& objects) {
        VkDevice device = mDevice->mDevice; VkAccelerationStructureInstanceKHR instance{};
        { const Ogre::Matrix4& m = objects[0].transform; for (int r=0; r<3; r++) for (int c=0; c<4; c++) instance.transform.matrix[r][c] = m[r][c]; }
        instance.mask = 0xFF; instance.accelerationStructureReference = objects[0].blasAddress; instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        createBuffer(sizeof(instance), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, mInstanceBuffer, mInstanceMemory);
        void* data; vkMapMemory(device, mInstanceMemory, 0, sizeof(instance), 0, &data); memcpy(data, &instance, sizeof(instance)); vkUnmapMemory(device, mInstanceMemory);
        VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR }; geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geo.geometry.instances.data.deviceAddress = getBufferDeviceAddress(mInstanceBuffer);
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR }; buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; buildInfo.geometryCount = 1; buildInfo.pGeometries = &geo;
        uint32_t count = 1; VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pfnGetASBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &count, &sizes);
        createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mTLASBuffer, mTLASMemory);
        VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR }; ci.buffer = mTLASBuffer; ci.size = sizes.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        pfnCreateAS(device, &ci, nullptr, &mTopLevelAS);
        VkBuffer scratch; VkDeviceMemory scratchMem; createBuffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratch, scratchMem);
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; buildInfo.dstAccelerationStructure = mTopLevelAS; buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(scratch);
        VkAccelerationStructureBuildRangeInfoKHR range = { count, 0, 0, 0 }; const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        VkCommandBuffer cmd = beginSingleTimeCommands(); pfnCmdBuildAS(cmd, 1, &buildInfo, &pRange); endSingleTimeCommands(cmd);
        vkDestroyBuffer(device, scratch, nullptr); vkFreeMemory(device, scratchMem, nullptr);
    }

    void RTPipeline::createSBT() {
        VkDevice device = mDevice->mDevice; VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &rtProps }; vkGetPhysicalDeviceProperties2(mDevice->mPhysicalDevice, &props2);
        uint32_t hSize = rtProps.shaderGroupHandleSize; uint32_t hStride = (hSize + rtProps.shaderGroupHandleAlignment - 1) & ~(rtProps.shaderGroupHandleAlignment - 1);
        uint32_t bAlign = rtProps.shaderGroupBaseAlignment; uint32_t rgSize = (hStride + bAlign - 1) & ~(bAlign - 1); uint32_t mSize = (hStride + bAlign - 1) & ~(bAlign - 1); uint32_t htSize = (hStride + bAlign - 1) & ~(bAlign - 1);
        createBuffer(rgSize + mSize + htSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, mSBTBuffer, mSBTMemory);
        std::vector<uint8_t> handles(hSize * 3); auto vkGetRTHandles = (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR");
        vkGetRTHandles(device, mRTPipeline, 0, 3, handles.size(), handles.data());
        uint8_t* mapped; vkMapMemory(device, mSBTMemory, 0, rgSize + mSize + htSize, 0, (void**)&mapped);
        memcpy(mapped, handles.data() + 0 * hSize, hSize); memcpy(mapped + rgSize, handles.data() + 1 * hSize, hSize); memcpy(mapped + rgSize + mSize, handles.data() + 2 * hSize, hSize);
        vkUnmapMemory(device, mSBTMemory); VkDeviceAddress sbtBase = getBufferDeviceAddress(mSBTBuffer);
        mRaygenRegion = { sbtBase, rgSize, rgSize }; mMissRegion = { sbtBase + rgSize, hStride, mSize }; mHitRegion = { sbtBase + rgSize + mSize, hStride, htSize };
    }

    void RTPipeline::createRTImages() {
        VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imgInfo.imageType = VK_IMAGE_TYPE_2D; imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM; imgInfo.extent = { 1280, 720, 1 }; imgInfo.mipLevels = 1; imgInfo.arrayLayers = 1; imgInfo.samples = VK_SAMPLE_COUNT_1_BIT; imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL; imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCreateImage(mDevice->mDevice, &imgInfo, nullptr, &mStorageImage);
        VkMemoryRequirements memReqs; vkGetImageMemoryRequirements(mDevice->mDevice, mStorageImage, &memReqs);
        VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = memReqs.size; allocInfo.memoryTypeIndex = findMemoryType(mDevice->mPhysicalDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(mDevice->mDevice, &allocInfo, nullptr, &mStorageImageMemory); vkBindImageMemory(mDevice->mDevice, mStorageImage, mStorageImageMemory, 0);
        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = mStorageImage; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM; viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(mDevice->mDevice, &viewInfo, nullptr, &mStorageImageView);
        VkCommandBuffer cmd = beginSingleTimeCommands();
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.image = mStorageImage; barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        endSingleTimeCommands(cmd);
    }

    void RTPipeline::createDescriptorSet() {
        VkDevice device = mDevice->mDevice;
        if (mCameraUBOBuffer == VK_NULL_HANDLE) createBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, mCameraUBOBuffer, mCameraUBOMemory);
        std::vector<VkDescriptorPoolSize> poolSizes = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; pci.maxSets = 1; pci.poolSizeCount = (uint32_t)poolSizes.size(); pci.pPoolSizes = poolSizes.data();
        vkCreateDescriptorPool(device, &pci, nullptr, &mDescriptorPool);
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; ai.descriptorPool = mDescriptorPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &mDescriptorSetLayout;
        vkAllocateDescriptorSets(device, &ai, &mDescriptorSet);
        VkWriteDescriptorSetAccelerationStructureKHR asInfo = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR }; asInfo.accelerationStructureCount = 1; asInfo.pAccelerationStructures = &mTopLevelAS;
        VkDescriptorImageInfo imgInfo{}; imgInfo.imageView = mStorageImageView; imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorBufferInfo uboInfo{}; uboInfo.buffer = mCameraUBOBuffer; uboInfo.offset = 0; uboInfo.range = sizeof(CameraUBO);
        std::vector<VkWriteDescriptorSet> writes;
        VkWriteDescriptorSet w0 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w0.dstSet = mDescriptorSet; w0.dstBinding = 0; w0.descriptorCount = 1; w0.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w0.pNext = &asInfo; writes.push_back(w0);
        VkWriteDescriptorSet w1 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w1.dstSet = mDescriptorSet; w1.dstBinding = 1; w1.descriptorCount = 1; w1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w1.pImageInfo = &imgInfo; writes.push_back(w1);
        VkWriteDescriptorSet w2 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w2.dstSet = mDescriptorSet; w2.dstBinding = 2; w2.descriptorCount = 1; w2.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w2.pBufferInfo = &uboInfo; writes.push_back(w2);
        vkUpdateDescriptorSets(device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }

    void RTPipeline::createSceneBuffers(Ogre::MeshPtr mesh) {}

    void RTPipeline::updateCameraUBO(const Ogre::Matrix4& view, const Ogre::Matrix4& proj, const Ogre::Vector3& camPos, int frameCount) {
        if (mCameraUBOBuffer == VK_NULL_HANDLE) createDescriptorSet();
        Ogre::Matrix4 vInv = view.inverse().transpose(); Ogre::Matrix4 pInv = proj.inverse().transpose();
        CameraUBO ubo{}; memcpy(ubo.viewInverse, &vInv[0][0], 64); memcpy(ubo.projInverse, &pInv[0][0], 64);
        ubo.cameraPos[0] = camPos.x; ubo.cameraPos[1] = camPos.y; ubo.cameraPos[2] = camPos.z; ubo.frameCount = frameCount;
        void* data; vkMapMemory(mDevice->mDevice, mCameraUBOMemory, 0, sizeof(CameraUBO), 0, &data); memcpy(data, &ubo, sizeof(CameraUBO)); vkUnmapMemory(mDevice->mDevice, mCameraUBOMemory);
    }

    VkCommandBuffer RTPipeline::beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; ai.commandPool = mCommandPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cmd; vkAllocateCommandBuffers(mDevice->mDevice, &ai, &cmd);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi); return cmd;
    }

    void RTPipeline::endSingleTimeCommands(VkCommandBuffer cmd) {
        vkEndCommandBuffer(cmd); VkSubmitInfo s = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
        vkQueueSubmit(mDevice->mGraphicsQueue.mQueue, 1, &s, VK_NULL_HANDLE); vkQueueWaitIdle(mDevice->mGraphicsQueue.mQueue);
        vkFreeCommandBuffers(mDevice->mDevice, mCommandPool, 1, &cmd);
    }

    void RTPipeline::recordRayTracingCommands(VkCommandBuffer cmd, VkDescriptorSet ds, uint32_t w, uint32_t h) {
        if (mRTPipeline == VK_NULL_HANDLE) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mRTPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mPipelineLayout, 0, 1, &ds, 0, nullptr);
        auto f = (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(mDevice->mDevice, "vkCmdTraceRaysKHR");
        f(cmd, &mRaygenRegion, &mMissRegion, &mHitRegion, &mCallableRegion, w, h, 1);
    }
}

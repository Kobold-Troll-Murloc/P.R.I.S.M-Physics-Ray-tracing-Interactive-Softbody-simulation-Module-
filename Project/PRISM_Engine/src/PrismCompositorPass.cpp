#include "PrismCompositorPass.h"
#include <OgreVulkanRenderSystem.h>
#include <OgreVulkanDevice.h>
#include <OgreVulkanRenderPassDescriptor.h>
#include <OgreVulkanTextureGpuWindow.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>
#include <OgreLogManager.h>

namespace Prism {

    Ogre::CompositorPassDef* RTCompositorPassProvider::addPassDef(Ogre::CompositorPassType passType,
                                                               Ogre::IdString customId,
                                                               Ogre::CompositorTargetDef* parentTargetDef,
                                                               Ogre::CompositorNodeDef* parentNodeDef) 
    {
        if (customId == "ray_tracing") {
            return OGRE_NEW RTPassDef(Ogre::PASS_CUSTOM, parentTargetDef);
        }
        return nullptr;
    }

    Ogre::CompositorPass* RTCompositorPassProvider::addPass(const Ogre::CompositorPassDef* definition,
                                                           Ogre::Camera* defaultCamera,
                                                           Ogre::CompositorNode* parentNode,
                                                           const Ogre::RenderTargetViewDef* rtvDef,
                                                           Ogre::SceneManager* sceneManager)
    {
        return OGRE_NEW RTPass(definition, parentNode, mRTPipeline, mWindow);
    }

    void RTPass::execute(const Ogre::Camera* lodCamera) {
        if (!mRTPipeline || mRTPipeline->getPipeline() == VK_NULL_HANDLE) return;
        if (mRTPipeline->getDescriptorSet() == VK_NULL_HANDLE) return;
        if (!mWindow) return;

        Ogre::VulkanRenderSystem* rs = static_cast<Ogre::VulkanRenderSystem*>(
            Ogre::Root::getSingleton().getRenderSystem());
        Ogre::VulkanDevice* device = rs->getVulkanDevice();
        if (!device) return;

        // OGRE 내부 RenderPass 종료 (필수)
        device->mGraphicsQueue.endAllEncoders();

        VkCommandBuffer cmdBuf = device->mGraphicsQueue.getCurrentCmdBuffer();
        if (cmdBuf == VK_NULL_HANDLE) return;

        uint32_t width  = mRTPipeline->getRTWidth();
        uint32_t height = mRTPipeline->getRTHeight();

        // ── G-Buffer 레이아웃 전환: COLOR_ATTACHMENT → SHADER_READ_ONLY ──────
        // PASS_SCENE(mrtGBuffer)이 쓰고 난 G-Buffer를 RT 셰이더가 샘플링하기 위해 전환.
        {
            VkImage gbuffers[3] = {
                mRTPipeline->getGBufferAlbedo(),
                mRTPipeline->getGBufferNormal(),
                mRTPipeline->getGBufferMaterial()
            };
            if (gbuffers[0] != VK_NULL_HANDLE) {
                VkImageMemoryBarrier barriers[3] = {};
                for (int i = 0; i < 3; i++) {
                    barriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barriers[i].oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    barriers[i].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[i].image               = gbuffers[i];
                    barriers[i].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    barriers[i].srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    barriers[i].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
                }
                vkCmdPipelineBarrier(cmdBuf,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    0, 0, nullptr, 0, nullptr, 3, barriers);
            }
        }

        // ── Phase 3: Ray Tracing 실행 ──────────────────────────────────────
        mRTPipeline->recordRayTracingCommands(cmdBuf, mRTPipeline->getDescriptorSet(), width, height);

        // ── Phase 4: StorageImage → SwapChain Blit ────────────────────────
        Ogre::TextureGpu* winTex = mWindow->getTexture();
        Ogre::VulkanTextureGpuWindow* vulkanWinTex =
            static_cast<Ogre::VulkanTextureGpuWindow*>(winTex);

        uint32_t swapIdx = vulkanWinTex->getCurrentSwapchainIdx();
        VkImage swapImage = vulkanWinTex->getWindowFinalTextureName(swapIdx);
        VkImage storageImage = mRTPipeline->getStorageImage();

        // StorageImage: GENERAL → TRANSFER_SRC
        VkImageMemoryBarrier srcBarrier{};
        srcBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        srcBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.image               = storageImage;
        srcBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        srcBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        srcBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;

        // SwapChain: 현재 레이아웃 무관하게 TRANSFER_DST로 강제 전환 (PASS_SCENE 후 layout 불확실)
        VkImageMemoryBarrier dstBarrier{};
        dstBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dstBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;  // 내용 버리고 전환
        dstBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.image               = swapImage;
        dstBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        dstBarrier.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
        dstBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

        VkImageMemoryBarrier preCopyBarriers[] = { srcBarrier, dstBarrier };
        vkCmdPipelineBarrier(cmdBuf,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, preCopyBarriers);

        // Blit (StorageImage RGBA8 → SwapChain)
        VkImageBlit blitRegion{};
        blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blitRegion.srcOffsets[0]  = { 0, 0, 0 };
        blitRegion.srcOffsets[1]  = { (int32_t)width, (int32_t)height, 1 };
        blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blitRegion.dstOffsets[0]  = { 0, 0, 0 };
        blitRegion.dstOffsets[1]  = { (int32_t)width, (int32_t)height, 1 };

        vkCmdBlitImage(cmdBuf,
            storageImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swapImage,    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blitRegion, VK_FILTER_NEAREST);

        // SwapChain: TRANSFER_DST → PRESENT_SRC (OGRE가 Present할 수 있도록 복원)
        VkImageMemoryBarrier presentBarrier{};
        presentBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        presentBarrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        presentBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.image               = swapImage;
        presentBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        presentBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        presentBarrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT;

        // StorageImage: TRANSFER_SRC → GENERAL (다음 프레임을 위해 복원)
        VkImageMemoryBarrier storageRestoreBarrier{};
        storageRestoreBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        storageRestoreBarrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        storageRestoreBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        storageRestoreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        storageRestoreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        storageRestoreBarrier.image               = storageImage;
        storageRestoreBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        storageRestoreBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        storageRestoreBarrier.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;

        VkImageMemoryBarrier postCopyBarriers[] = { presentBarrier, storageRestoreBarrier };
        vkCmdPipelineBarrier(cmdBuf,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 2, postCopyBarriers);

        // ── G-Buffer 레이아웃 복원: SHADER_READ_ONLY → COLOR_ATTACHMENT ──────
        // 다음 프레임 PASS_SCENE(mrtGBuffer)이 G-Buffer를 다시 쓰기 위해 복원.
        {
            VkImage gbuffers[3] = {
                mRTPipeline->getGBufferAlbedo(),
                mRTPipeline->getGBufferNormal(),
                mRTPipeline->getGBufferMaterial()
            };
            if (gbuffers[0] != VK_NULL_HANDLE) {
                VkImageMemoryBarrier barriers[3] = {};
                for (int i = 0; i < 3; i++) {
                    barriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barriers[i].oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barriers[i].newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[i].image               = gbuffers[i];
                    barriers[i].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    barriers[i].srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
                    barriers[i].dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                }
                vkCmdPipelineBarrier(cmdBuf,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0, 0, nullptr, 0, nullptr, 3, barriers);
            }
        }
    }

}

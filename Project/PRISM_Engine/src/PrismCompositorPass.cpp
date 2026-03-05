#include "PrismCompositorPass.h"
#include <OgreVulkanRenderSystem.h>
#include <OgreVulkanDevice.h>
#include <OgreVulkanRenderPassDescriptor.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>

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
        return OGRE_NEW RTPass(definition, parentNode, mRTPipeline);
    }

    void RTPass::execute(const Ogre::Camera* lodCamera) {
        if (!mRTPipeline || mRTPipeline->getPipeline() == VK_NULL_HANDLE) return;

        Ogre::VulkanRenderSystem* rs = static_cast<Ogre::VulkanRenderSystem*>(
            Ogre::Root::getSingleton().getRenderSystem());
        
        Ogre::VulkanDevice* device = rs->getVulkanDevice();
        if (!device) return;

        // Ensure we are in a valid state to record commands
        VkCommandBuffer cmdBuf = device->mGraphicsQueue.getCurrentCmdBuffer();
        if (cmdBuf == VK_NULL_HANDLE) return;

        uint32_t width = 1280; 
        uint32_t height = 720; 
        VkDescriptorSet rtDescriptorSet = VK_NULL_HANDLE; 

        mRTPipeline->recordRayTracingCommands(cmdBuf, rtDescriptorSet, width, height);
    }

}

#pragma once

#include <OgreCompositorPassProvider.h>
#include <OgreCompositorPass.h>
#include <OgreCompositorPassDef.h>
#include "PrismRTPipeline.h"

namespace Prism {

    class RTPassDef : public Ogre::CompositorPassDef {
    public:
        RTPassDef(Ogre::CompositorPassType type, Ogre::CompositorTargetDef* parentTargetDef)
            : Ogre::CompositorPassDef(type, parentTargetDef) {}
    };

    class RTPass : public Ogre::CompositorPass {
    public:
        RTPass(const Ogre::CompositorPassDef* definition, 
               Ogre::CompositorNode* parentNode, 
               RTPipeline* rtPipeline)
            : Ogre::CompositorPass(definition, parentNode)
            , mRTPipeline(rtPipeline) {}

        virtual void execute(const Ogre::Camera* lodCamera) override;

    private:
        RTPipeline* mRTPipeline;
    };

    class RTCompositorPassProvider : public Ogre::CompositorPassProvider {
    public:
        RTCompositorPassProvider(RTPipeline* rtPipeline) : mRTPipeline(rtPipeline) {}

        virtual Ogre::CompositorPassDef* addPassDef(Ogre::CompositorPassType passType,
                                                  Ogre::IdString customId,
                                                  Ogre::CompositorTargetDef* parentTargetDef,
                                                  Ogre::CompositorNodeDef* parentNodeDef) override;

        virtual Ogre::CompositorPass* addPass(const Ogre::CompositorPassDef* definition,
                                             Ogre::Camera* defaultCamera,
                                             Ogre::CompositorNode* parentNode,
                                             const Ogre::RenderTargetViewDef* rtvDef,
                                             Ogre::SceneManager* sceneManager) override;

    private:
        RTPipeline* mRTPipeline;
    };

}

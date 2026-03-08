#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <exception>
#include <cmath>
#include <algorithm>
#include <cfloat>
#include <sstream>

#include <windows.h>

#include <OgreRoot.h>
#include <OgreRenderSystem.h>
#include <OgreWindow.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreItem.h>
#include <OgreLight.h>
#include <OgreStringConverter.h>
#include <OgreArchiveManager.h>
#include <OgreHlmsManager.h>
#include <OgreMesh2.h>
#include <OgreSubMesh2.h>
#include <OgreMeshManager.h>
#include <OgreMeshManager2.h>
#include <OgreResourceGroupManager.h>
#include <OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreImage2.h>
#include <OgreManualObject2.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreHlmsPbs.h>

#include <OgreVulkanRenderSystem.h>
#include <OgreVulkanDevice.h>
#include "PrismRTPipeline.h"
#include "PrismCompositorPass.h"

#include <SDL.h>
#include <SDL_syswm.h>

LONG WINAPI PrismCrashHandler(EXCEPTION_POINTERS* p) {
    std::cerr << "\n!!! PRISM SEH CRASH !!! Code: 0x" << std::hex << p->ExceptionRecord->ExceptionCode << std::endl;
    return EXCEPTION_EXECUTE_HANDLER;
}

static Ogre::String GetBasePath() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    Ogre::String path(buffer);
    return path.substr(0, path.find_last_of("\\/") + 1);
}

struct RawObj {
    std::vector<Ogre::Vector3> pos;
    std::vector<uint32_t> indices;
};

static RawObj ParseObj(const std::string& path) {
    std::vector<Ogre::Vector3> v;
    std::vector<uint32_t> ind;
    std::ifstream file(path);
    if (!file.is_open()) return {v, ind};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line); std::string prefix; ss >> prefix;
        if (prefix == "v") { float x, y, z; ss >> x >> y >> z; v.push_back(Ogre::Vector3(x, y, z)); }
        else if (prefix == "f") {
            std::vector<uint32_t> faceIndices;
            std::string v1; while(ss >> v1) {
                size_t slash = v1.find('/');
                faceIndices.push_back(std::stoi(slash == std::string::npos ? v1 : v1.substr(0, slash)) - 1);
            }
            for(size_t i=1; i<faceIndices.size()-1; ++i) {
                ind.push_back(faceIndices[0]); ind.push_back(faceIndices[i]); ind.push_back(faceIndices[i+1]);
            }
        }
    }
    if (v.empty()) return {v, ind};
    Ogre::Vector3 minV(FLT_MAX), maxV(-FLT_MAX);
    for (auto& p : v) { minV.makeFloor(p); maxV.makeCeil(p); }
    Ogre::Vector3 center = (minV + maxV) * 0.5f; for (auto& p : v) p -= center;
    return {v, ind};
}

class PrismApp {
public:
    Ogre::Root* mRoot = nullptr; Ogre::Window* mWindow = nullptr; Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::Camera* mCamera = nullptr; Ogre::SceneNode* mCamNode = nullptr; SDL_Window* mSdlWin = nullptr;
    Ogre::SceneNode* mBunnyNode = nullptr; Prism::RTPipeline* mRTPipeline = nullptr;
    Prism::RTCompositorPassProvider* mPassProvider = nullptr;

    bool setup() {
        mRoot = new Ogre::Root(nullptr, "", "", "PRISM.log");
        mRoot->loadPlugin("RenderSystem_Vulkan_d", false, nullptr);
        Ogre::RenderSystem* rs = nullptr;
        for (auto r : mRoot->getAvailableRenderers()) if (r->getName().find("Vulkan") != std::string::npos) rs = r;
        if (!rs) return false;
        mRoot->setRenderSystem(rs); mRoot->initialise(false);
        mSdlWin = SDL_CreateWindow("PRISM", 100, 100, 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        SDL_SysWMinfo wm; SDL_VERSION(&wm.version); SDL_GetWindowWMInfo(mSdlWin, &wm);
        Ogre::NameValuePairList p; p["externalWindowHandle"] = Ogre::StringConverter::toString((size_t)wm.info.win.window);
        mWindow = mRoot->createRenderWindow("PRISM", 1280, 720, false, &p);

        mRTPipeline = new Prism::RTPipeline(static_cast<Ogre::VulkanRenderSystem*>(rs));
        mRTPipeline->initialize();
        mPassProvider = new Prism::RTCompositorPassProvider(mRTPipeline, mWindow);
        auto comp = mRoot->getCompositorManager2(); comp->setCompositorPassProvider(mPassProvider);

        registerHlms();
        mSceneMgr = mRoot->createSceneManager(Ogre::ST_GENERIC, 1u);
        mCamera = mSceneMgr->createCamera("PrismCam");
        mCamera->setNearClipDistance(0.1f); mCamera->setFarClipDistance(10000.0f);
        mCamNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        if (mCamera->getParentSceneNode()) mCamera->detachFromParent();
        mCamNode->attachObject(mCamera); mCamNode->setPosition(0, 0, 1000); mCamera->lookAt(Ogre::Vector3::ZERO);

        auto light = mSceneMgr->createLight(); light->setType(Ogre::Light::LT_DIRECTIONAL);
        auto lNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        if (light->getParentSceneNode()) light->detachFromParent();
        lNode->attachObject(light); lNode->setDirection(Ogre::Vector3(-1, -1, -1).normalisedCopy());

        createBunny();
        if (mRTPipeline && mBunnyNode) {
            Ogre::Item* item = static_cast<Ogre::Item*>(mBunnyNode->getAttachedObject(0));
            mRTPipeline->buildBLAS(item->getMesh());
            Prism::RTObject obj; obj.blasAddress = mRTPipeline->getBLASAddress();
            obj.transform = Ogre::Matrix4::IDENTITY; obj.transform.setScale(Ogre::Vector3(1000, 1000, 1000));
            mRTPipeline->buildTLAS({ obj }); mRTPipeline->createDescriptorSet();
        }

        {
            Ogre::CompositorNodeDef* nodeDef = comp->addNodeDefinition("RTPrismNode");
            nodeDef->addTextureSourceName("rt0", 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
            nodeDef->setNumTargetPass(1);
            Ogre::CompositorTargetDef* targetDef = nodeDef->addTargetPass("rt0");
            targetDef->setNumPasses(2);
            Ogre::CompositorPassSceneDef* sceneDef = static_cast<Ogre::CompositorPassSceneDef*>(targetDef->addPass(Ogre::PASS_SCENE));
            sceneDef->setAllClearColours(Ogre::ColourValue(0.1f, 0.1f, 0.1f)); sceneDef->setAllLoadActions(Ogre::LoadAction::Clear);
            targetDef->addPass(Ogre::PASS_CUSTOM, "ray_tracing");
            comp->addWorkspaceDefinition("MainWS")->connectExternal(0, nodeDef->getName(), 0);
        }
        comp->addWorkspace(mSceneMgr, mWindow->getTexture(), mCamera, "MainWS", true);
        return true;
    }

    void registerHlms() {
        Ogre::ArchiveManager& archMgr = Ogre::ArchiveManager::getSingleton();
        Ogre::String base = GetBasePath() + "Media/";
        auto reg = [&](Ogre::HlmsTypes type) {
            Ogre::String mainPath; Ogre::StringVector libPaths;
            if (type == Ogre::HLMS_PBS) Ogre::HlmsPbs::getDefaultPaths(mainPath, libPaths);
            else Ogre::HlmsUnlit::getDefaultPaths(mainPath, libPaths);
            auto archMain = archMgr.load(base + mainPath, "FileSystem", true);
            Ogre::ArchiveVec archLibs; for (const auto& l : libPaths) archLibs.push_back(archMgr.load(base + l, "FileSystem", true));
            auto hlms = (type == Ogre::HLMS_PBS) ? (Ogre::Hlms*)OGRE_NEW Ogre::HlmsPbs(archMain, &archLibs) : (Ogre::Hlms*)OGRE_NEW Ogre::HlmsUnlit(archMain, &archLibs);
            mRoot->getHlmsManager()->registerHlms(hlms);
        };
        reg(Ogre::HLMS_UNLIT); reg(Ogre::HLMS_PBS);
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(base + "Common", "FileSystem", "General");
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups(true);
    }

    void createBunny() {
        std::string objPath = GetBasePath() + "bunny.obj"; auto raw = ParseObj(objPath);
        if (raw.pos.empty()) return;
        Ogre::VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        Ogre::VertexElement2Vec vElements;
        vElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
        vElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));
        float* vData = static_cast<float*>(OGRE_MALLOC_SIMD(sizeof(float) * raw.pos.size() * 6, Ogre::MEMCATEGORY_GEOMETRY));
        for (size_t i = 0; i < raw.pos.size(); ++i) {
            vData[i*6+0]=raw.pos[i].x; vData[i*6+1]=raw.pos[i].y; vData[i*6+2]=raw.pos[i].z; vData[i*6+3]=0; vData[i*6+4]=1; vData[i*6+5]=0;
        }
        auto vBuf = vaoManager->createVertexBuffer(vElements, raw.pos.size(), Ogre::BT_DEFAULT, vData, true);
        uint32_t* iData = static_cast<uint32_t*>(OGRE_MALLOC_SIMD(sizeof(uint32_t) * raw.indices.size(), Ogre::MEMCATEGORY_GEOMETRY));
        memcpy(iData, raw.indices.data(), sizeof(uint32_t) * raw.indices.size());
        auto iBuf = vaoManager->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, raw.indices.size(), Ogre::BT_DEFAULT, iData, true);
        Ogre::VertexBufferPackedVec vBuffers; vBuffers.push_back(vBuf);
        auto vao = vaoManager->createVertexArrayObject(vBuffers, iBuf, Ogre::OT_TRIANGLE_LIST);
        auto mesh = Ogre::MeshManager::getSingleton().createManual("BunnyMesh", "General");
        auto sub = mesh->createSubMesh(); sub->mVao[0].push_back(vao); sub->mVao[1].push_back(vao);
        mesh->_setBounds(Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3(500, 500, 500)), true);
        auto item = mSceneMgr->createItem(mesh);
        mBunnyNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        if (item->getParentSceneNode()) item->detachFromParent();
        mBunnyNode->attachObject(item); mBunnyNode->setScale(1000, 1000, 1000);
    }

    void run() {
        bool bQuit = false; SDL_Event evt; Uint32 lastTime = SDL_GetTicks();
        float moveSpeed = 500.0f; float rotSpeed = 0.005f; bool bRightMouseDown = false;
        while (!bQuit) {
            Uint32 now = SDL_GetTicks(); float dt = (now - lastTime) / 1000.0f; lastTime = now;
            while (SDL_PollEvent(&evt)) {
                if (evt.type == SDL_QUIT || (evt.type == SDL_KEYDOWN && evt.key.keysym.sym == SDLK_ESCAPE)) bQuit = true;
                if (evt.type == SDL_MOUSEBUTTONDOWN && evt.button.button == SDL_BUTTON_RIGHT) bRightMouseDown = true;
                if (evt.type == SDL_MOUSEBUTTONUP && evt.button.button == SDL_BUTTON_RIGHT) bRightMouseDown = false;
                if (evt.type == SDL_MOUSEMOTION && bRightMouseDown) {
                    mCamNode->yaw(Ogre::Radian(-evt.motion.xrel * rotSpeed), Ogre::Node::TS_PARENT);
                    mCamNode->pitch(Ogre::Radian(-evt.motion.yrel * rotSpeed), Ogre::Node::TS_LOCAL);
                }
            }
            const Uint8* ks = SDL_GetKeyboardState(NULL); Ogre::Vector3 mv = Ogre::Vector3::ZERO;
            if (ks[SDL_SCANCODE_W]) mv.z -= moveSpeed * dt; if (ks[SDL_SCANCODE_S]) mv.z += moveSpeed * dt;
            if (ks[SDL_SCANCODE_A]) mv.x -= moveSpeed * dt; if (ks[SDL_SCANCODE_D]) mv.x += moveSpeed * dt;
            mCamNode->translate(mCamNode->getOrientation() * mv);
            if (mRTPipeline) mRTPipeline->updateCameraUBO(mCamera->getViewMatrix(), mCamera->getProjectionMatrixWithRSDepth(), mCamNode->getPosition(), 0);
            mSceneMgr->updateSceneGraph();
            if (!mRoot->renderOneFrame()) break;
        }
    }
};

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO);
    try { PrismApp app; if (app.setup()) app.run(); } catch (Ogre::Exception& e) { std::cerr << e.getFullDescription() << std::endl; }
    SDL_Quit(); return 0;
}

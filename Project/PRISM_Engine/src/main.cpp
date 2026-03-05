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
        std::stringstream ss(line);
        std::string prefix;
        ss >> prefix;
        if (prefix == "v") {
            float x, y, z;
            ss >> x >> y >> z;
            v.push_back(Ogre::Vector3(x, y, z));
        } else if (prefix == "f") {
            std::string vertStr;
            while (ss >> vertStr) {
                // Handle f 1/2/3 or f 1//3 or f 1
                size_t slashPos = vertStr.find('/');
                std::string idxStr = (slashPos == std::string::npos) ? vertStr : vertStr.substr(0, slashPos);
                int idx = std::stoi(idxStr);
                ind.push_back(idx > 0 ? (uint32_t)(idx - 1) : (uint32_t)(v.size() + idx));
            }
        }
    }
    if (v.empty()) return {v, ind};
    Ogre::Vector3 minV(FLT_MAX), maxV(-FLT_MAX);
    for (auto& p : v) { minV.makeFloor(p); maxV.makeCeil(p); }
    Ogre::Vector3 center = (minV + maxV) * 0.5f;
    for (auto& p : v) p -= center;
    return {v, ind};
}

class PrismApp {
public:
    Ogre::Root* mRoot = nullptr;
    Ogre::Window* mWindow = nullptr;
    Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::Camera* mCamera = nullptr;
    Ogre::SceneNode* mCamNode = nullptr;
    SDL_Window* mSdlWin = nullptr;
    Ogre::SceneNode* mBunnyNode = nullptr;
    Prism::RTPipeline* mRTPipeline = nullptr;
    Prism::RTCompositorPassProvider* mPassProvider = nullptr;

    bool setup() {
        mRoot = new Ogre::Root(nullptr, "", "", "PRISM.log");
        mRoot->loadPlugin("RenderSystem_Vulkan_d", false, nullptr);
        Ogre::RenderSystem* rs = nullptr;
        for (auto r : mRoot->getAvailableRenderers()) if (r->getName().find("Vulkan") != std::string::npos) rs = r;
        if (!rs) return false;

        // Debug: List all available options
        std::cout << "[PRISM] Available Vulkan Options:" << std::endl;
        for (auto opt : rs->getConfigOptions()) {
            std::cout << " - " << opt.first << std::endl;
        }

        mRoot->setRenderSystem(rs);
        mRoot->initialise(false);

        mSdlWin = SDL_CreateWindow("PRISM", 100, 100, 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        SDL_SysWMinfo wm; SDL_VERSION(&wm.version); SDL_GetWindowWMInfo(mSdlWin, &wm);
        Ogre::NameValuePairList p; p["externalWindowHandle"] = Ogre::StringConverter::toString((size_t)wm.info.win.window);
        mWindow = mRoot->createRenderWindow("PRISM", 1280, 720, false, &p);

        // Initialize PRISM Hybrid RT Pipeline
        mRTPipeline = new Prism::RTPipeline(static_cast<Ogre::VulkanRenderSystem*>(rs));
        mRTPipeline->initialize();

        // Register Custom Compositor Pass for RT
        mPassProvider = new Prism::RTCompositorPassProvider(mRTPipeline);
        auto comp = mRoot->getCompositorManager2();
        comp->setCompositorPassProvider(mPassProvider);

        registerHlms();
        mSceneMgr = mRoot->createSceneManager(Ogre::ST_GENERIC, 1u);
        
        mCamera = mSceneMgr->createCamera("PrismCam");
        mCamera->setNearClipDistance(0.1f); mCamera->setFarClipDistance(10000.0f);
        mCamNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        if (mCamera->isAttached()) mCamera->detachFromParent();
        if (mCamNode->numAttachedObjects() == 0) mCamNode->attachObject(mCamera);
        mCamNode->setPosition(0, 800, 3500); 
        // Instead of lookAt, set a fixed safe orientation to avoid derived transform errors
        mCamNode->setOrientation(Ogre::Quaternion(Ogre::Degree(-10), Ogre::Vector3::UNIT_X));

        auto light = mSceneMgr->createLight(); light->setType(Ogre::Light::LT_DIRECTIONAL);
        auto lNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        if (light->isAttached()) light->detachFromParent();
        if (lNode->numAttachedObjects() == 0) lNode->attachObject(light);
        light->setPowerScale(Ogre::Math::PI * 3.0f); 
        lNode->setDirection(Ogre::Vector3(-1, -1, -1).normalisedCopy());

        mSceneMgr->setAmbientLight(Ogre::ColourValue(0.2f, 0.2f, 0.2f), Ogre::ColourValue(0.2f, 0.2f, 0.2f), Ogre::Vector3::UNIT_Y);

        // [PRISM] Force sync everything before starting
        mSceneMgr->updateSceneGraph();

        // Create Workspace
        comp->createBasicWorkspaceDef("MainWS", Ogre::ColourValue(0.1f, 0.1f, 0.1f)); // Dark background
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
            Ogre::ArchiveVec archLibs;
            for (const auto& l : libPaths) archLibs.push_back(archMgr.load(base + l, "FileSystem", true));
            auto hlms = (type == Ogre::HLMS_PBS) ? (Ogre::Hlms*)OGRE_NEW Ogre::HlmsPbs(archMain, &archLibs) : (Ogre::Hlms*)OGRE_NEW Ogre::HlmsUnlit(archMain, &archLibs);
            mRoot->getHlmsManager()->registerHlms(hlms);
        };
        reg(Ogre::HLMS_UNLIT); reg(Ogre::HLMS_PBS);
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(base + "Common", "FileSystem", "General");
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups(true);
    }

    void createBunny() {
        std::string objPath = GetBasePath() + "bunny.obj";
        auto raw = ParseObj(objPath);
        if (raw.pos.empty()) {
            std::cerr << "[PRISM ERROR] Failed to load bunny.obj at: " << objPath << std::endl;
            return;
        }
        std::cout << "[PRISM] Bunny v2 Mesh 소환 시도... Vertices: " << raw.pos.size() << std::endl;

        Ogre::RenderSystem *renderSystem = mRoot->getRenderSystem();
        Ogre::VaoManager *vaoManager = renderSystem->getVaoManager();

        Ogre::VertexElement2Vec vertexElements;
        vertexElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
        vertexElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));

        size_t vertexCount = raw.pos.size();
        float* vData = static_cast<float*>(OGRE_MALLOC_SIMD(sizeof(float) * vertexCount * 6, Ogre::MEMCATEGORY_GEOMETRY));
        for (size_t i = 0; i < vertexCount; ++i) {
            vData[i * 6 + 0] = raw.pos[i].x; vData[i * 6 + 1] = raw.pos[i].y; vData[i * 6 + 2] = raw.pos[i].z;
            vData[i * 6 + 3] = 0; vData[i * 6 + 4] = 1; vData[i * 6 + 5] = 0;
        }
        Ogre::VertexBufferPacked *vertexBuffer = vaoManager->createVertexBuffer(vertexElements, vertexCount, Ogre::BT_DEFAULT, vData, true);

        size_t indexCount = raw.indices.size();
        uint32_t* iData = static_cast<uint32_t*>(OGRE_MALLOC_SIMD(sizeof(uint32_t) * indexCount, Ogre::MEMCATEGORY_GEOMETRY));
        memcpy(iData, raw.indices.data(), sizeof(uint32_t) * indexCount);
        Ogre::IndexBufferPacked *indexBuffer = vaoManager->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, indexCount, Ogre::BT_DEFAULT, iData, true);

        Ogre::VertexBufferPackedVec vertexBuffers;
        vertexBuffers.push_back(vertexBuffer);
        Ogre::VertexArrayObject *vao = vaoManager->createVertexArrayObject(vertexBuffers, indexBuffer, Ogre::OT_TRIANGLE_LIST);

        Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual("BunnyMesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        Ogre::SubMesh *subMesh = mesh->createSubMesh();
        subMesh->mVao[0].push_back(vao);
        subMesh->mVao[1].push_back(vao);
        
        mesh->_setBounds(Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3(500, 500, 500)), true);
        mesh->_setBoundingSphereRadius(800.0f);

        auto item = mSceneMgr->createItem(mesh);
        mBunnyNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        mBunnyNode->attachObject(item);
        mBunnyNode->setScale(500.0f, 500.0f, 500.0f);
        
        mSceneMgr->updateSceneGraph(); // Ensure node state is valid after creation
        std::cout << "[PRISM] Bunny 소환 성공!" << std::endl;
    }

    void run() {
        bool bQuit = false; SDL_Event evt; bool bReady = false;
        Uint32 lastTime = SDL_GetTicks();
        while (!bQuit) {
            Uint32 now = SDL_GetTicks(); float dt = (now - lastTime) / 1000.0f; lastTime = now;
            while (SDL_PollEvent(&evt)) {
                if (evt.type == SDL_QUIT || (evt.type == SDL_KEYDOWN && evt.key.keysym.sym == SDLK_ESCAPE)) bQuit = true;
                if (evt.type == SDL_WINDOWEVENT && evt.window.event == SDL_WINDOWEVENT_RESIZED) mWindow->windowMovedOrResized();
            }
            if (dt > 0.1f) dt = 0.1f;

            if (!bReady) {
                static int waitFrames = 0;
                if (waitFrames++ > 60) {
                    createBunny(); 
                    if (mRTPipeline && mBunnyNode && mBunnyNode->numAttachedObjects() > 0) {
                        Ogre::MovableObject* obj = mBunnyNode->getAttachedObject(0);
                        Ogre::Item* item = static_cast<Ogre::Item*>(obj);
                        if (item->getMesh()) {
                            std::cout << "[PRISM] 메쉬 리소스 준비 완료. BLAS 구축 시작..." << std::endl;
                            mRTPipeline->buildBLAS(item->getMesh());
                        }
                    }
                    mSceneMgr->updateSceneGraph(); // Final sync after initialization
                    bReady = true;
                }
            }

            if (mBunnyNode) {
                mBunnyNode->yaw(Ogre::Radian(dt));
            }
            
            // [PRISM] Single point of truth for node transforms before rendering
            mSceneMgr->updateSceneGraph();
            if (!mRoot->renderOneFrame()) break;
        }
    }
};

int main(int argc, char* argv[]) {
    SetUnhandledExceptionFilter(PrismCrashHandler);
    SDL_Init(SDL_INIT_VIDEO);
    try {
        PrismApp app;
        if (app.setup()) app.run();
    } catch (Ogre::Exception& e) { std::cerr << "OGRE EXCEPTION: " << e.getFullDescription() << std::endl; }
    SDL_Quit();
    return 0;
}

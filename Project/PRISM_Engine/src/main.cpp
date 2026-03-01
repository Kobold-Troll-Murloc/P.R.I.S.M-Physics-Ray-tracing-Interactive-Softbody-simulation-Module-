#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <exception>
#include <cmath>
#include <algorithm>
#include <cfloat>

// Windows SEH 크래시 진단
#include <windows.h>

// OGRE Next 헤더
#include <OgreRoot.h>
#include <OgreRenderSystem.h>
#include <OgreWindow.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreItem.h>
#include <OgreLight.h>
#include <OgreStringConverter.h>
#include <OgreAbiUtils.h>
#include <OgreArchiveManager.h>
#include <OgreHlmsManager.h>
#include <OgreMesh2.h>
#include <OgreSubMesh2.h>
#include <OgreMeshManager2.h>
#include <OgreResourceGroupManager.h>
#include <Compositor/OgreCompositorManager2.h>
#include <OgreImage2.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>
#include <Hlms/Pbs/OgreHlmsPbs.h>
#include <Hlms/Pbs/OgreHlmsPbsDatablock.h>
#include <Hlms/Unlit/OgreHlmsUnlit.h>
#include <Hlms/Unlit/OgreHlmsUnlitDatablock.h>

// SDL2 헤더
#include <SDL.h>
#include <SDL_syswm.h>

// ============================================================
// SEH 크래시 핸들러
// ============================================================
static LONG WINAPI PrismCrashHandler(EXCEPTION_POINTERS* pEx)
{
    fprintf(stderr, "\n========== [PRISM CRASH] ==========\n");
    fprintf(stderr, "Exception Code : 0x%08lX\n", pEx->ExceptionRecord->ExceptionCode);
    fprintf(stderr, "====================================\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================
// OBJ 파서
// ============================================================
struct ObjMeshData {
    std::vector<float> vertices;
    std::vector<uint16_t> indices;
    Ogre::Aabb bounds;
    float sphereRadius;
};

static ObjMeshData LoadObjFile(const std::string& path)
{
    std::vector<Ogre::Vector3> positions;
    std::vector<std::vector<int>> faces;
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("OBJ Open Fail");
    std::string line;
    while (std::getline(file, line)) {
        if (line.size() < 2) continue;
        if (line[0] == 'v' && line[1] == ' ') {
            Ogre::Vector3 p; sscanf(line.c_str(), "v %f %f %f", &p.x, &p.y, &p.z);
            positions.push_back(p);
        } else if (line[0] == 'f' && line[1] == ' ') {
            int a, b, c; sscanf(line.c_str(), "f %d %d %d", &a, &b, &c);
            faces.push_back({a-1, b-1, c-1});
        }
    }
    float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
    for (auto& p : positions) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
    }
    Ogre::Vector3 center((minX+maxX)*0.5f, (minY+maxY)*0.5f, (minZ+maxZ)*0.5f);
    Ogre::Vector3 halfSize((maxX-minX)*0.5f, (maxY-minY)*0.5f, (maxZ-minZ)*0.5f);
    
    ObjMeshData res;
    res.vertices.resize(positions.size() * 6);
    for (size_t i=0; i<positions.size(); ++i) {
        res.vertices[i*6+0] = positions[i].x - center.x;
        res.vertices[i*6+1] = positions[i].y - center.y;
        res.vertices[i*6+2] = positions[i].z - center.z;
        Ogre::Vector3 n = positions[i].normalisedCopy(); // 엉성한 노멀
        res.vertices[i*6+3] = n.x; res.vertices[i*6+4] = n.y; res.vertices[i*6+5] = n.z;
    }
    for (auto& f : faces) { res.indices.push_back((uint16_t)f[0]); res.indices.push_back((uint16_t)f[1]); res.indices.push_back((uint16_t)f[2]); }
    res.bounds = Ogre::Aabb(Ogre::Vector3::ZERO, halfSize);
    res.sphereRadius = halfSize.length();
    return res;
}

// ============================================================
// Mesh 생성
// ============================================================
static Ogre::MeshPtr CreateMesh(const ObjMeshData& data, Ogre::VaoManager* vaoMgr)
{
    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual("Bunny", "General");
    Ogre::SubMesh* sm = mesh->createSubMesh();
    Ogre::VertexElement2Vec elements = {Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION), Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL)};
    float* vb = (float*)OGRE_MALLOC_SIMD(sizeof(float)*data.vertices.size(), Ogre::MEMCATEGORY_GEOMETRY);
    memcpy(vb, data.vertices.data(), sizeof(float)*data.vertices.size());
    Ogre::VertexBufferPacked* vbo = vaoMgr->createVertexBuffer(elements, data.vertices.size()/6, Ogre::BT_IMMUTABLE, vb, true);
    uint16_t* ib = (uint16_t*)OGRE_MALLOC_SIMD(sizeof(uint16_t)*data.indices.size(), Ogre::MEMCATEGORY_GEOMETRY);
    memcpy(ib, data.indices.data(), sizeof(uint16_t)*data.indices.size());
    Ogre::IndexBufferPacked* ibo = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_16BIT, data.indices.size(), Ogre::BT_IMMUTABLE, ib, true);
    Ogre::VertexArrayObject* vao = vaoMgr->createVertexArrayObject({vbo}, ibo, Ogre::OT_TRIANGLE_LIST);
    sm->mVao[Ogre::VpNormal].push_back(vao); sm->mVao[Ogre::VpShadow].push_back(vao);
    mesh->_setBounds(data.bounds); mesh->_setBoundingSphereRadius(data.sphereRadius);
    return mesh;
}

// ============================================================
// HLMS
// ============================================================
static void RegisterHlms(Ogre::Root* root) {
    using namespace Ogre;
    RenderSystem* rs = root->getRenderSystem();
    String syntax = (rs->getName().find("Direct3D11") != String::npos) ? "HLSL" : "GLSL";
    ArchiveManager& archMgr = ArchiveManager::getSingleton();
    HlmsManager* hlmsMgr = root->getHlmsManager();

    auto reg = [&](HlmsTypes type, const String& name) {
        String mainPath;
        StringVector libPaths;
        if (type == HLMS_PBS) HlmsPbs::getDefaultPaths(mainPath, libPaths);
        else HlmsUnlit::getDefaultPaths(mainPath, libPaths);

        Archive* archMain = archMgr.load("./Media/" + mainPath, "FileSystem", true);
        ArchiveVec archLibs;
        for (const auto& p : libPaths)
            archLibs.push_back(archMgr.load("./Media/" + p, "FileSystem", true));

        Hlms* hlms = nullptr;
        if (type == HLMS_PBS) hlms = OGRE_NEW HlmsPbs(archMain, &archLibs);
        else hlms = OGRE_NEW HlmsUnlit(archMain, &archLibs);
        hlmsMgr->registerHlms(hlms);
    };

    reg(HLMS_UNLIT, "Unlit");
    reg(HLMS_PBS, "Pbs");

    ResourceGroupManager::getSingleton().addResourceLocation("./Media/Common", "FileSystem", "General");
    ResourceGroupManager::getSingleton().initialiseAllResourceGroups(true);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    SetUnhandledExceptionFilter(PrismCrashHandler);
    Ogre::Root* root = nullptr; SDL_Window* sdlWin = nullptr;
    try {
        SDL_Init(SDL_INIT_VIDEO);
        Ogre::AbiCookie cookie = Ogre::generateAbiCookie();
        root = new Ogre::Root(&cookie, "", "", "PRISM.log");
        root->loadPlugin("RenderSystem_Direct3D11_d", false, nullptr);
        root->setRenderSystem(root->getAvailableRenderers()[0]);
        root->initialise(false);
        sdlWin = SDL_CreateWindow("PRISM", 100, 100, 1280, 720, SDL_WINDOW_SHOWN);
        SDL_SysWMinfo wm; SDL_VERSION(&wm.version); SDL_GetWindowWMInfo(sdlWin, &wm);
        Ogre::NameValuePairList p; p["externalWindowHandle"] = Ogre::StringConverter::toString((size_t)wm.info.win.window);
        auto ogreWin = root->createRenderWindow("PRISM", 1280, 720, false, &p);
        ogreWin->_setVisible(true);
        RegisterHlms(root);
        auto sm = root->createSceneManager(Ogre::ST_GENERIC, 1u);
        auto cam = sm->createCamera("Cam"); cam->setNearClipDistance(0.1f); cam->setFarClipDistance(1000.0f);
        if (cam->isAttached()) cam->detachFromParent();
        auto camNode = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        camNode->attachObject(cam);
        
        auto obj = LoadObjFile("bunny.obj");
        auto mesh = CreateMesh(obj, root->getRenderSystem()->getVaoManager());
        auto item = sm->createItem(mesh);
        auto node = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        node->attachObject(item);
        node->setScale(500.0f, 500.0f, 500.0f);
        
        auto light = sm->createLight();
        auto lnode = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        lnode->attachObject(light); light->setType(Ogre::Light::LT_DIRECTIONAL);
        lnode->setDirection(Ogre::Vector3(1,-1,-1).normalisedCopy());
        
        auto pbs = static_cast<Ogre::HlmsPbs*>(root->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        auto db = static_cast<Ogre::HlmsPbsDatablock*>(pbs->createDatablock("Mat", "Mat", Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        db->setDiffuse(Ogre::Vector3(1,1,1)); item->setDatablock(db);
        
        camNode->setPosition(0, 0, 150); camNode->lookAt(Ogre::Vector3::ZERO, Ogre::Node::TS_WORLD);
        
        auto comp = root->getCompositorManager2();
        comp->createBasicWorkspaceDef("WS", Ogre::ColourValue(0.2f, 0.4f, 0.6f));
        comp->addWorkspace(sm, ogreWin->getTexture(), cam, "WS", true);
        
        int f = 0; while (f < 50) {
            if (!root->renderOneFrame()) break;
            if (f == 10) {
                Ogre::Image2 img; img.convertFromTexture(ogreWin->getTexture(), 0, 0);
                img.save("D:/Git/P.R.I.S.M/Project/PRISM_Engine/build/Debug/screenshot_final.png", 0, 1);
            }
            f++;
        }
    } catch (std::exception& e) { std::cerr << e.what() << std::endl; }
    if (root) delete root; if (sdlWin) SDL_DestroyWindow(sdlWin); SDL_Quit();
    return 0;
}

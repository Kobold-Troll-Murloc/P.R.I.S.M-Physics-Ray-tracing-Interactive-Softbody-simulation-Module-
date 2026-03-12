#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <exception>
#include <cmath>
#include <algorithm>
#include <cfloat>
#include <sstream>
#include <map>
#include <unordered_map>

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

// ── OBJ 파서 ──────────────────────────────────────────────

struct RawObj {
    std::vector<Ogre::Vector3> pos;
    std::vector<Ogre::Vector3> normals;
    std::vector<uint32_t>      indices;
};

// "v", "v/vt", "v//vn", "v/vt/vn" 토큰 파싱 → (vi, ni) (-1이면 없음)
static void parseFaceToken(const std::string& tok, int& vi, int& ni) {
    vi = -1; ni = -1;
    size_t slash1 = tok.find('/');
    if (slash1 == std::string::npos) {
        vi = std::stoi(tok) - 1;
        return;
    }
    vi = std::stoi(tok.substr(0, slash1)) - 1;
    size_t slash2 = tok.find('/', slash1 + 1);
    if (slash2 == std::string::npos) {
        // v/vt  (법선 없음)
    } else if (slash2 == slash1 + 1) {
        // v//vn
        if (slash2 + 1 < tok.size())
            ni = std::stoi(tok.substr(slash2 + 1)) - 1;
    } else {
        // v/vt/vn
        ni = std::stoi(tok.substr(slash2 + 1)) - 1;
    }
}

static RawObj ParseObj(const std::string& path) {
    std::vector<Ogre::Vector3> rawPos, rawNorm;

    struct FaceTri { int v[3]; int vn[3]; };
    std::vector<FaceTri> triangles;
    bool hasNormals = false;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[PRISM] OBJ not found: " << path << std::endl;
        return {};
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string prefix; ss >> prefix;

        if (prefix == "v") {
            float x, y, z; ss >> x >> y >> z;
            rawPos.push_back({x, y, z});
        } else if (prefix == "vn") {
            float x, y, z; ss >> x >> y >> z;
            rawNorm.push_back({x, y, z});
            hasNormals = true;
        } else if (prefix == "f") {
            std::vector<int> vis, vnis;
            std::string tok;
            while (ss >> tok) {
                int vi = -1, ni = -1;
                parseFaceToken(tok, vi, ni);
                vis.push_back(vi);
                vnis.push_back(ni);
            }
            // Fan triangulation
            for (size_t i = 1; i + 1 < vis.size(); i++) {
                FaceTri tri;
                tri.v[0]  = vis[0];  tri.v[1]  = vis[i];  tri.v[2]  = vis[i+1];
                tri.vn[0] = vnis[0]; tri.vn[1] = vnis[i]; tri.vn[2] = vnis[i+1];
                triangles.push_back(tri);
            }
        }
    }

    std::vector<Ogre::Vector3> finalPos, finalNorm;
    std::vector<uint32_t>      indices;

    if (hasNormals) {
        // (v_idx, vn_idx) 쌍으로 unique vertex 관리
        std::map<std::pair<int,int>, uint32_t> uniqueMap;
        for (auto& tri : triangles) {
            for (int j = 0; j < 3; j++) {
                auto key = std::make_pair(tri.v[j], tri.vn[j]);
                auto it = uniqueMap.find(key);
                if (it != uniqueMap.end()) {
                    indices.push_back(it->second);
                } else {
                    uint32_t idx = (uint32_t)finalPos.size();
                    uniqueMap[key] = idx;
                    finalPos.push_back(rawPos[tri.v[j]]);
                    Ogre::Vector3 n = (tri.vn[j] >= 0 && tri.vn[j] < (int)rawNorm.size())
                        ? rawNorm[tri.vn[j]] : Ogre::Vector3(0, 1, 0);
                    finalNorm.push_back(n);
                    indices.push_back(idx);
                }
            }
        }
    } else {
        // 법선 없음 → 각 삼각형마다 face normal 계산 (flat shading)
        for (auto& tri : triangles) {
            if (tri.v[0] < 0 || tri.v[1] < 0 || tri.v[2] < 0) continue;
            if (tri.v[0] >= (int)rawPos.size() || tri.v[1] >= (int)rawPos.size() || tri.v[2] >= (int)rawPos.size()) continue;
            Ogre::Vector3 a = rawPos[tri.v[0]];
            Ogre::Vector3 b = rawPos[tri.v[1]];
            Ogre::Vector3 c = rawPos[tri.v[2]];
            Ogre::Vector3 faceNorm = (b - a).crossProduct(c - a);
            float len = faceNorm.length();
            if (len > 1e-8f) faceNorm /= len;
            else faceNorm = Ogre::Vector3(0, 1, 0);
            for (int j = 0; j < 3; j++) {
                indices.push_back((uint32_t)finalPos.size());
                finalPos.push_back(rawPos[tri.v[j]]);
                finalNorm.push_back(faceNorm);
            }
        }
    }

    // 중심 정규화 (바운딩박스 center를 원점으로)
    if (!finalPos.empty()) {
        Ogre::Vector3 minV(FLT_MAX), maxV(-FLT_MAX);
        for (auto& p : finalPos) { minV.makeFloor(p); maxV.makeCeil(p); }
        Ogre::Vector3 center = (minV + maxV) * 0.5f;
        for (auto& p : finalPos) p -= center;
    }

    return { finalPos, finalNorm, indices };
}

// ── 씬 오브젝트 정의 ──────────────────────────────────────

struct SceneObject {
    std::string   modelPath;
    Ogre::Vector3 position;
    Ogre::Vector3 scale;
    Ogre::Vector3 albedo;
    float roughness = 0.5f;
    float metallic  = 0.0f;
    float specTrans = 0.0f;
    float ior       = 1.5f;
    float emissive  = 0.0f;
    bool  isRaster  = false; // true → RT 셰이더에서 래스터 스타일 Lambert 셰이딩 사용 (하이브리드 데모)
};

// ── OGRE 메시 헬퍼 (캐시 포함) ───────────────────────────

static std::unordered_map<std::string, Ogre::MeshPtr> sMeshCache;
static int sMeshCounter = 0;

static Ogre::MeshPtr loadMeshFromObj(const std::string& objPath, Ogre::VaoManager* vaoMgr) {
    // 캐시 확인
    auto it = sMeshCache.find(objPath);
    if (it != sMeshCache.end()) return it->second;

    auto raw = ParseObj(objPath);
    if (raw.pos.empty()) {
        std::cerr << "[PRISM] Empty mesh: " << objPath << std::endl;
        return Ogre::MeshPtr();
    }

    Ogre::VertexElement2Vec vElements;
    vElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    vElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));

    float* vData = static_cast<float*>(OGRE_MALLOC_SIMD(sizeof(float) * raw.pos.size() * 6, Ogre::MEMCATEGORY_GEOMETRY));
    for (size_t i = 0; i < raw.pos.size(); ++i) {
        vData[i*6+0] = raw.pos[i].x;     vData[i*6+1] = raw.pos[i].y;     vData[i*6+2] = raw.pos[i].z;
        vData[i*6+3] = raw.normals[i].x; vData[i*6+4] = raw.normals[i].y; vData[i*6+5] = raw.normals[i].z;
    }
    auto vBuf = vaoMgr->createVertexBuffer(vElements, raw.pos.size(), Ogre::BT_DEFAULT, vData, true);

    uint32_t* iData = static_cast<uint32_t*>(OGRE_MALLOC_SIMD(sizeof(uint32_t) * raw.indices.size(), Ogre::MEMCATEGORY_GEOMETRY));
    memcpy(iData, raw.indices.data(), sizeof(uint32_t) * raw.indices.size());
    auto iBuf = vaoMgr->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, raw.indices.size(), Ogre::BT_DEFAULT, iData, true);

    Ogre::VertexBufferPackedVec vBuffers; vBuffers.push_back(vBuf);
    auto vao = vaoMgr->createVertexArrayObject(vBuffers, iBuf, Ogre::OT_TRIANGLE_LIST);

    std::string meshName = "PrismMesh_" + std::to_string(sMeshCounter++);
    auto mesh = Ogre::MeshManager::getSingleton().createManual(meshName, "General");
    auto sub  = mesh->createSubMesh();
    sub->mVao[0].push_back(vao);
    sub->mVao[1].push_back(vao);
    mesh->_setBounds(Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3(1, 1, 1)), true);

    sMeshCache[objPath] = mesh;
    std::cout << "[PRISM] Loaded mesh: " << objPath << " (" << raw.pos.size() << " verts, " << raw.indices.size()/3 << " tris)" << std::endl;
    return mesh;
}

// ── PrismApp ─────────────────────────────────────────────

class PrismApp {
public:
    Ogre::Root*       mRoot     = nullptr;
    Ogre::Window*     mWindow   = nullptr;
    Ogre::SceneManager* mSceneMgr = nullptr;
    Ogre::Camera*     mCamera   = nullptr;
    Ogre::SceneNode*  mCamNode  = nullptr;
    SDL_Window*       mSdlWin   = nullptr;
    Prism::RTPipeline* mRTPipeline = nullptr;
    Prism::RTCompositorPassProvider* mPassProvider = nullptr;

    bool setup() {
        SetUnhandledExceptionFilter(PrismCrashHandler);

        mRoot = new Ogre::Root(nullptr, "", "", "PRISM.log");
        mRoot->loadPlugin("RenderSystem_Vulkan_d", false, nullptr);

        Ogre::RenderSystem* rs = nullptr;
        for (auto r : mRoot->getAvailableRenderers())
            if (r->getName().find("Vulkan") != std::string::npos) rs = r;
        if (!rs) return false;

        mRoot->setRenderSystem(rs);
        mRoot->initialise(false);

        mSdlWin = SDL_CreateWindow("PRISM - Cornell Box", 100, 100, 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        SDL_SysWMinfo wm; SDL_VERSION(&wm.version); SDL_GetWindowWMInfo(mSdlWin, &wm);
        Ogre::NameValuePairList p;
        p["externalWindowHandle"] = Ogre::StringConverter::toString((size_t)wm.info.win.window);
        mWindow = mRoot->createRenderWindow("PRISM", 1280, 720, false, &p);

        mRTPipeline = new Prism::RTPipeline(static_cast<Ogre::VulkanRenderSystem*>(rs));
        mRTPipeline->initialize();
        mPassProvider = new Prism::RTCompositorPassProvider(mRTPipeline, mWindow);
        auto comp = mRoot->getCompositorManager2();
        comp->setCompositorPassProvider(mPassProvider);

        registerHlms();

        mSceneMgr = mRoot->createSceneManager(Ogre::ST_GENERIC, 1u);
        mCamera = mSceneMgr->createCamera("PrismCam");
        mCamera->setNearClipDistance(0.01f);
        mCamera->setFarClipDistance(10000.0f);
        mCamera->setAutoAspectRatio(true);

        mCamNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        if (mCamera->getParentSceneNode()) mCamera->detachFromParent();
        mCamNode->attachObject(mCamera);

        // 더미 방향광 (래스터 패스용)
        auto light = mSceneMgr->createLight();
        light->setType(Ogre::Light::LT_DIRECTIONAL);
        auto lNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        if (light->getParentSceneNode()) light->detachFromParent();
        lNode->attachObject(light);
        lNode->setDirection(Ogre::Vector3(-1, -2, -1).normalisedCopy());

        setupScene();

        {
            Ogre::CompositorNodeDef* nodeDef = comp->addNodeDefinition("RTPrismNode");
            nodeDef->addTextureSourceName("rt0", 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
            nodeDef->setNumTargetPass(1);
            Ogre::CompositorTargetDef* targetDef = nodeDef->addTargetPass("rt0");
            targetDef->setNumPasses(2);
            Ogre::CompositorPassSceneDef* sceneDef = static_cast<Ogre::CompositorPassSceneDef*>(targetDef->addPass(Ogre::PASS_SCENE));
            sceneDef->setAllClearColours(Ogre::ColourValue(0.0f, 0.0f, 0.0f));
            sceneDef->setAllLoadActions(Ogre::LoadAction::Clear);
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
            Ogre::ArchiveVec archLibs;
            for (const auto& l : libPaths) archLibs.push_back(archMgr.load(base + l, "FileSystem", true));
            auto hlms = (type == Ogre::HLMS_PBS)
                ? (Ogre::Hlms*)OGRE_NEW Ogre::HlmsPbs(archMain, &archLibs)
                : (Ogre::Hlms*)OGRE_NEW Ogre::HlmsUnlit(archMain, &archLibs);
            mRoot->getHlmsManager()->registerHlms(hlms);
        };
        reg(Ogre::HLMS_UNLIT); reg(Ogre::HLMS_PBS);
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(base + "Common", "FileSystem", "General");
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups(true);
    }

    void setupScene() {
        std::string basePath = GetBasePath();
        Ogre::VaoManager* vaoMgr = mRoot->getRenderSystem()->getVaoManager();

        // ──────────────────────────────────────────────────────────────────
        // Cornell Box + 다양한 매질 박스 씬
        //
        // 좌표 기준:
        //   cube.obj 단위 큐브: [-0.5, +0.5] 범위, 중심 원점
        //   바닥 상단 y = -1 + 0.1 = -0.9
        //   박스 하단을 바닥에 맞추려면: center_y = -0.9 + scale_y / 2
        //
        // 박스 매질 목록:
        //   왼쪽 tall  : 맑은 유리 (Clear Glass)     specTrans=1.0, IOR=1.52, rough=0.0
        //   오른쪽 tall: 완전 거울 (Perfect Mirror)   metallic=1.0, rough=0.02
        //   중앙       : 토끼 받침대 (Diffuse White)
        //   왼앞 small : 황금 거친 금속 (Rough Gold)  metallic=1.0, rough=0.4
        //   오른앞 small: 서리 유리 (Frosted Glass)   specTrans=0.9, rough=0.3
        // ──────────────────────────────────────────────────────────────────
        std::vector<SceneObject> scene = {
            // modelPath              pos                      scale               albedo                   rough  metal  specTr  ior   emit
            // ── 방 구조 ──────────────────────────────────────────────────────────────────────────
            // 바닥 top=−0.9 / 천장 bottom=11.9 → 벽은 y=−1.1~12.1 범위로 만들어 틈 제거
            { basePath+"cube.obj", {  0,  -1,    0}, {20,    0.2f,  20  }, {0.8f, 0.8f, 0.8f}, 0.8f, 0.0f, 0.0f, 1.5f, 0.0f }, // 바닥
            { basePath+"cube.obj", {  0,  12,    0}, {20,    0.2f,  20  }, {1.0f, 1.0f, 1.0f}, 0.8f, 0.0f, 0.0f, 1.5f, 0.0f }, // 천장
            { basePath+"cube.obj", {  0,   5.5f,-10}, {20.4f,13.2f,  0.2f},{0.9f, 0.9f, 0.9f},0.8f, 0.0f, 0.0f, 1.5f, 0.0f }, // 뒷벽  (center_y=5.5, h=13.2 → y∈[−1.1,12.1])
            { basePath+"cube.obj", {-10,   5.5f,  0}, {0.2f, 13.2f, 20.4f},{0.8f, 0.1f, 0.1f},0.8f, 0.0f, 0.0f, 1.5f, 0.0f }, // 왼벽  (빨강)
            { basePath+"cube.obj", { 10,   5.5f,  0}, {0.2f, 13.2f, 20.4f},{0.1f, 0.8f, 0.1f},0.8f, 0.0f, 0.0f, 1.5f, 0.0f }, // 오른벽 (초록)
            { basePath+"cube.obj", {  0,  11.5f,  0}, {8,    0.1f,   8  }, {1.0f, 1.0f, 1.0f}, 0.5f, 0.0f, 0.0f, 1.5f, 4.0f }, // 면광원 (emissive)
            // ── 오브젝트 박스들 ─────────────────────────────────────────────────────────────────
            // 왼쪽 tall — 맑은 유리 (center_y = −0.9 + 6/2 = 2.1)
            { basePath+"cube.obj", {-4.5f, 2.1f,  -6}, {3.5f, 6.0f, 3.5f}, {0.95f,0.97f,1.0f}, 0.0f, 0.0f, 1.0f,1.52f, 0.0f },
            // 오른쪽 tall — 완전 거울 (metallic=1, rough≈0)
            { basePath+"cube.obj", { 4.5f, 2.1f,  -6}, {3.5f, 6.0f, 3.5f}, {0.9f, 0.9f, 0.9f}, 0.02f,1.0f, 0.0f, 1.5f, 0.0f },
            // 중앙 — 토끼 받침대 (center_y = −0.9+1 = 0.1, top=1.1)
            { basePath+"cube.obj", {  0,   0.1f,  -5}, {3.0f, 2.0f, 3.0f}, {0.9f, 0.9f, 0.9f}, 0.8f, 0.0f, 0.0f, 1.5f, 0.0f },
            // 왼쪽 앞 — 황금 거친 금속 [RASTER] isRaster=true: Lambert 셰이딩으로 표시
            { basePath+"cube.obj", { -6,  -0.15f,-2.5f},{2.0f, 1.5f, 2.0f},{1.0f,0.77f,0.34f}, 0.4f, 1.0f, 0.0f, 1.5f, 0.0f, true },
            // 오른쪽 앞 — 서리 유리 (Frosted Glass, specTrans=0.9, rough=0.3)
            { basePath+"cube.obj", {  6,  -0.15f,-2.5f},{2.0f, 1.5f, 2.0f},{0.9f, 0.9f, 1.0f}, 0.3f, 0.0f, 0.9f, 1.5f, 0.0f },
            // ── 토끼 (받침대 위) ───────────────────────────────────────────────────────────────
            // 받침대 top=1.1, bunny Y반높이(scale=8)≈0.62 → center_y≈1.72
            { basePath+"bunny.obj",{  0,   1.7f,  -5}, {8.0f, 8.0f, 8.0f}, {0.9f, 0.9f, 0.9f}, 0.1f, 0.0f, 0.0f, 1.5f, 0.0f },
        };

        std::vector<Prism::RTObject>         rtObjects;
        std::vector<Prism::InstanceMaterial> materials;
        std::vector<Prism::ObjDesc>          objDescs;

        for (size_t i = 0; i < scene.size(); i++) {
            auto& obj = scene[i];

            // 메시 로드 (같은 OBJ면 OGRE 메시 재사용)
            Ogre::MeshPtr mesh = loadMeshFromObj(obj.modelPath, vaoMgr);
            if (!mesh) continue;

            // OGRE SceneNode (래스터 패스용 - RT 결과로 덮어쓰므로 투명 처리)
            auto item = mSceneMgr->createItem(mesh);
            auto node = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
            if (item->getParentSceneNode()) item->detachFromParent();
            node->attachObject(item);
            node->setPosition(obj.position);
            node->setScale(obj.scale);

            // BLAS 빌드 (같은 OBJ 경로면 캐시)
            uint32_t meshIdx = mRTPipeline->buildBLAS(mesh, obj.modelPath);

            // TRS Transform → Ogre::Matrix4
            Ogre::Matrix4 transform = Ogre::Matrix4::IDENTITY;
            transform.makeTransform(obj.position, obj.scale, Ogre::Quaternion::IDENTITY);

            // RT 오브젝트
            Prism::RTObject rtObj;
            rtObj.blas        = VK_NULL_HANDLE; // 직접 쓰지 않음
            rtObj.blasAddress = mRTPipeline->getBLASAddress(meshIdx);
            rtObj.transform   = transform;
            rtObj.customIndex = (uint32_t)i;
            rtObjects.push_back(rtObj);

            // Material
            Prism::InstanceMaterial mat{};
            mat.albedo[0] = obj.albedo.x; mat.albedo[1] = obj.albedo.y;
            mat.albedo[2] = obj.albedo.z; mat.albedo[3] = 1.0f;
            mat.pbrParams1[0] = obj.emissive;  mat.pbrParams1[1] = obj.roughness;
            mat.pbrParams1[2] = obj.metallic;  mat.pbrParams1[3] = 0.0f;
            mat.pbrParams2[0] = obj.specTrans; mat.pbrParams2[1] = obj.ior;
            mat.pbrParams2[2] = obj.isRaster ? 1.0f : 0.0f; // 하이브리드: 1.0 → RT에서 래스터 Lambert 셰이딩
            materials.push_back(mat);

            // ObjDesc (셰이더에서 정점/인덱스 직접 접근)
            Prism::ObjDesc desc;
            desc.vertexAddress = mRTPipeline->getMeshVertexAddress(meshIdx);
            desc.indexAddress  = mRTPipeline->getMeshIndexAddress(meshIdx);
            objDescs.push_back(desc);
        }

        // TLAS, 씬 버퍼, Descriptor Set 구성
        mRTPipeline->buildTLAS(rtObjects);
        mRTPipeline->createSceneBuffers(materials, objDescs);
        mRTPipeline->createDescriptorSet();

        // 카메라 초기 위치 (Cornell Box 앞에서 바라보기)
        // OGRE Next 3.0: SceneNode::lookAt() = ASSERT, Camera::lookAt() = roll 뒤집힘
        // → 표준 lookAt 행렬로 Quaternion 직접 계산
        {
            Ogre::Vector3 eye(0, 7, 15), target(0, 5, 0), worldUp(0, 1, 0);
            Ogre::Vector3 zAxis = (eye - target).normalisedCopy(); // -forward (카메라는 -Z forward)
            Ogre::Vector3 xAxis = worldUp.crossProduct(zAxis).normalisedCopy();
            Ogre::Vector3 yAxis = zAxis.crossProduct(xAxis);
            Ogre::Matrix3 rotMat;
            rotMat.SetColumn(0, xAxis);
            rotMat.SetColumn(1, yAxis);
            rotMat.SetColumn(2, zAxis);
            Ogre::Quaternion q; q.FromRotationMatrix(rotMat);
            mCamNode->setPosition(eye);
            mCamNode->setOrientation(q);
        }
    }

    void run() {
        bool bQuit = false;
        SDL_Event evt;
        Uint32 lastTime = SDL_GetTicks();
        float moveSpeed   = 12.0f;  // Cornell Box 스케일에 맞게 조정
        float rotSpeed    = 0.005f;
        bool bRightMouseDown = false;
        int frameCount = 0;

        while (!bQuit) {
            Uint32 now = SDL_GetTicks();
            float dt = (now - lastTime) / 1000.0f;
            lastTime = now;
            bool bMoved = false;

            while (SDL_PollEvent(&evt)) {
                if (evt.type == SDL_QUIT || (evt.type == SDL_KEYDOWN && evt.key.keysym.sym == SDLK_ESCAPE))
                    bQuit = true;
                if (evt.type == SDL_MOUSEBUTTONDOWN && evt.button.button == SDL_BUTTON_RIGHT)
                    bRightMouseDown = true;
                if (evt.type == SDL_MOUSEBUTTONUP && evt.button.button == SDL_BUTTON_RIGHT)
                    bRightMouseDown = false;
                if (evt.type == SDL_MOUSEMOTION && bRightMouseDown) {
                    mCamNode->yaw(Ogre::Radian(-evt.motion.xrel * rotSpeed), Ogre::Node::TS_PARENT);
                    mCamNode->pitch(Ogre::Radian(-evt.motion.yrel * rotSpeed), Ogre::Node::TS_LOCAL);
                    bMoved = true;
                }
            }

            const Uint8* ks = SDL_GetKeyboardState(NULL);
            Ogre::Vector3 mv = Ogre::Vector3::ZERO;
            if (ks[SDL_SCANCODE_W]) mv.z -= moveSpeed * dt;
            if (ks[SDL_SCANCODE_S]) mv.z += moveSpeed * dt;
            if (ks[SDL_SCANCODE_A]) mv.x -= moveSpeed * dt;
            if (ks[SDL_SCANCODE_D]) mv.x += moveSpeed * dt;
            if (ks[SDL_SCANCODE_Q]) mv.y -= moveSpeed * dt;
            if (ks[SDL_SCANCODE_E]) mv.y += moveSpeed * dt;

            if (mv != Ogre::Vector3::ZERO) {
                mCamNode->translate(mCamNode->getOrientation() * mv);
                bMoved = true;
            }

            // 이동 시 누적 카운터 리셋 → 노이즈 누적 재시작
            if (bMoved) frameCount = 0;
            else frameCount++;

            if (mRTPipeline)
                mRTPipeline->updateCameraUBO(
                    mCamera->getViewMatrix(),
                    mCamera->getProjectionMatrixWithRSDepth(),
                    mCamNode->getPosition(),
                    frameCount);

            mSceneMgr->updateSceneGraph();
            if (!mRoot->renderOneFrame()) break;
        }
    }
};

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO);
    try {
        PrismApp app;
        if (app.setup()) app.run();
    } catch (Ogre::Exception& e) {
        std::cerr << "[PRISM] Ogre::Exception: " << e.getFullDescription() << std::endl;
    } catch (std::exception& e) {
        std::cerr << "[PRISM] std::exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[PRISM] Unknown exception caught" << std::endl;
    }
    SDL_Quit();
    return 0;
}

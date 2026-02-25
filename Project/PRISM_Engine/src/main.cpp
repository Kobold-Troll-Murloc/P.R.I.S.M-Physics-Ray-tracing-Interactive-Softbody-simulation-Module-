#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <exception>
#include <cmath>
#include <algorithm>
#include <cfloat>

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
#include <Compositor/OgreCompositorManager2.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>
#include <Hlms/Pbs/OgreHlmsPbs.h>
#include <Hlms/Unlit/OgreHlmsUnlit.h>

// SDL2 헤더
#include <SDL.h>
#include <SDL_syswm.h>

// ============================================================
// OBJ 파서 및 노멀 계산
// ============================================================
struct ObjMeshData
{
    std::vector<float>    vertices;  // [px,py,pz, nx,ny,nz] interleaved
    std::vector<uint16_t> indices;
    Ogre::Aabb            bounds;
    float                 sphereRadius;
};

static ObjMeshData LoadObjFile(const std::string& path)
{
    struct P3 { float x, y, z; };
    struct F3 { int   a, b, c; };
    struct N3 { float x, y, z; };

    std::vector<P3> positions;
    std::vector<F3> faces;

    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("OBJ 파일을 열 수 없습니다: " + path);

    std::string line;
    while (std::getline(file, line))
    {
        if (line.size() < 2) continue;
        if (line[0] == 'v' && line[1] == ' ')
        {
            P3 p;
            sscanf(line.c_str(), "v %f %f %f", &p.x, &p.y, &p.z);
            positions.push_back(p);
        }
        else if (line[0] == 'f' && line[1] == ' ')
        {
            F3 f;
            sscanf(line.c_str(), "f %d %d %d", &f.a, &f.b, &f.c);
            f.a--; f.b--; f.c--;   // 1-indexed → 0-indexed
            faces.push_back(f);
        }
    }

    // 면 노멀을 각 버텍스에 누적 → 부드러운 노멀
    int numVerts = (int)positions.size();
    std::vector<N3> normals(numVerts, N3{0.f, 0.f, 0.f});

    for (auto& f : faces)
    {
        auto& p0 = positions[f.a];
        auto& p1 = positions[f.b];
        auto& p2 = positions[f.c];
        float e1x = p1.x-p0.x, e1y = p1.y-p0.y, e1z = p1.z-p0.z;
        float e2x = p2.x-p0.x, e2y = p2.y-p0.y, e2z = p2.z-p0.z;
        float nx = e1y*e2z - e1z*e2y;
        float ny = e1z*e2x - e1x*e2z;
        float nz = e1x*e2y - e1y*e2x;
        normals[f.a].x += nx; normals[f.a].y += ny; normals[f.a].z += nz;
        normals[f.b].x += nx; normals[f.b].y += ny; normals[f.b].z += nz;
        normals[f.c].x += nx; normals[f.c].y += ny; normals[f.c].z += nz;
    }
    for (auto& n : normals)
    {
        float len = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
        if (len > 1e-6f) { n.x /= len; n.y /= len; n.z /= len; }
    }

    // 바운딩 박스
    float minX =  FLT_MAX, minY =  FLT_MAX, minZ =  FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
    for (auto& p : positions)
    {
        if (p.x < minX) minX = p.x; if (p.x > maxX) maxX = p.x;
        if (p.y < minY) minY = p.y; if (p.y > maxY) maxY = p.y;
        if (p.z < minZ) minZ = p.z; if (p.z > maxZ) maxZ = p.z;
    }
    float cx = (minX+maxX)*0.5f, cy = (minY+maxY)*0.5f, cz = (minZ+maxZ)*0.5f;
    float hx = (maxX-minX)*0.5f, hy = (maxY-minY)*0.5f, hz = (maxZ-minZ)*0.5f;

    // 버텍스 버퍼 구성 [px,py,pz, nx,ny,nz]
    ObjMeshData result;
    result.vertices.resize(numVerts * 6);
    for (int i = 0; i < numVerts; ++i)
    {
        result.vertices[i*6+0] = positions[i].x;
        result.vertices[i*6+1] = positions[i].y;
        result.vertices[i*6+2] = positions[i].z;
        result.vertices[i*6+3] = normals[i].x;
        result.vertices[i*6+4] = normals[i].y;
        result.vertices[i*6+5] = normals[i].z;
    }
    result.indices.reserve(faces.size() * 3);
    for (auto& f : faces)
    {
        result.indices.push_back((uint16_t)f.a);
        result.indices.push_back((uint16_t)f.b);
        result.indices.push_back((uint16_t)f.c);
    }
    result.bounds       = Ogre::Aabb(Ogre::Vector3(cx, cy, cz), Ogre::Vector3(hx, hy, hz));
    result.sphereRadius = std::sqrt(hx*hx + hy*hy + hz*hz);

    return result;
}

// ============================================================
// OGRE Next v2 Mesh 생성
// ============================================================
static Ogre::MeshPtr CreateMeshFromObj(const ObjMeshData& data, Ogre::VaoManager* vaoManager)
{
    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        "BunnyMesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::SubMesh* subMesh = mesh->createSubMesh();

    // 버텍스 버퍼 (position + normal)
    Ogre::VertexElement2Vec vertexElements;
    vertexElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    vertexElements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));

    size_t numVerts = data.vertices.size() / 6;
    float* vbData = reinterpret_cast<float*>(
        OGRE_MALLOC_SIMD(sizeof(float) * data.vertices.size(), Ogre::MEMCATEGORY_GEOMETRY));
    memcpy(vbData, data.vertices.data(), sizeof(float) * data.vertices.size());

    Ogre::VertexBufferPacked* vertexBuffer = vaoManager->createVertexBuffer(
        vertexElements, numVerts, Ogre::BT_IMMUTABLE, vbData, true);

    // 인덱스 버퍼 (uint16)
    size_t numIndices = data.indices.size();
    uint16_t* ibData = reinterpret_cast<uint16_t*>(
        OGRE_MALLOC_SIMD(sizeof(uint16_t) * numIndices, Ogre::MEMCATEGORY_GEOMETRY));
    memcpy(ibData, data.indices.data(), sizeof(uint16_t) * numIndices);

    Ogre::IndexBufferPacked* indexBuffer = vaoManager->createIndexBuffer(
        Ogre::IndexBufferPacked::IT_16BIT, numIndices, Ogre::BT_IMMUTABLE, ibData, true);

    // VAO 생성
    Ogre::VertexBufferPackedVec vbVec;
    vbVec.push_back(vertexBuffer);
    Ogre::VertexArrayObject* vao = vaoManager->createVertexArrayObject(
        vbVec, indexBuffer, Ogre::OT_TRIANGLE_LIST);

    subMesh->mVao[Ogre::VpNormal].push_back(vao);
    subMesh->mVao[Ogre::VpShadow].push_back(vao);  // 그림자 패스도 동일 지오메트리 사용

    mesh->_setBounds(data.bounds, false);
    mesh->_setBoundingSphereRadius(data.sphereRadius);

    return mesh;
}

// ============================================================
// HLMS 등록 (Media/Hlms/ 폴더 기준)
// ============================================================
static void RegisterHlms()
{
    using namespace Ogre;

    // exe 실행 위치 기준 상대 경로 (CMake가 빌드 후 복사)
    // getDefaultPaths()가 "Hlms/Pbs/" 등을 반환하므로 부모 폴더까지만 지정
    const String rootHlmsFolder = "./Media/";

    ArchiveManager& archMgr = ArchiveManager::getSingleton();

    // HlmsUnlit 등록
    {
        String mainPath;
        StringVector libraryPaths;
        HlmsUnlit::getDefaultPaths(mainPath, libraryPaths);

        Archive* archMain = archMgr.load(rootHlmsFolder + mainPath, "FileSystem", true);
        ArchiveVec archLibs;
        for (const auto& lib : libraryPaths)
            archLibs.push_back(archMgr.load(rootHlmsFolder + lib, "FileSystem", true));

        HlmsUnlit* hlmsUnlit = OGRE_NEW HlmsUnlit(archMain, &archLibs);
        Root::getSingleton().getHlmsManager()->registerHlms(hlmsUnlit);
    }

    // HlmsPbs 등록
    {
        String mainPath;
        StringVector libraryPaths;
        HlmsPbs::getDefaultPaths(mainPath, libraryPaths);

        Archive* archMain = archMgr.load(rootHlmsFolder + mainPath, "FileSystem", true);
        ArchiveVec archLibs;
        for (const auto& lib : libraryPaths)
            archLibs.push_back(archMgr.load(rootHlmsFolder + lib, "FileSystem", true));

        HlmsPbs* hlmsPbs = OGRE_NEW HlmsPbs(archMain, &archLibs);
        Root::getSingleton().getHlmsManager()->registerHlms(hlmsPbs);
    }
}

// ============================================================
// 렌더 시스템 플러그인 로드
// ============================================================
static void LoadRenderSystemPlugin(Ogre::Root* root)
{
    std::vector<std::string> plugins = {
        "RenderSystem_Vulkan",
        "RenderSystem_Direct3D11",
        "RenderSystem_GL3Plus"
    };
    bool loaded = false;
    for (const auto& plugin : plugins)
    {
        try
        {
#if defined(_DEBUG)
            root->loadPlugin(plugin + "_d", false, nullptr);
#else
            root->loadPlugin(plugin, false, nullptr);
#endif
            std::cout << "[OK] 렌더 시스템 로드: " << plugin << "\n";
            loaded = true;
            break;
        }
        catch (...)
        {
            std::cout << "[경고] 플러그인 로드 실패: " << plugin << "\n";
        }
    }
    if (!loaded)
        throw std::runtime_error("사용 가능한 렌더 시스템을 찾을 수 없습니다.");
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[])
{
    Ogre::Root*  root      = nullptr;
    SDL_Window*  sdlWindow = nullptr;

    try
    {
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            throw std::runtime_error(std::string("SDL 초기화 실패: ") + SDL_GetError());

        // OGRE Root 생성
        Ogre::AbiCookie abiCookie = Ogre::generateAbiCookie();
        root = new Ogre::Root(&abiCookie, "", "", "PRISM_Engine.log", "PRISM Engine");

        // 렌더 시스템 로드 및 선택
        LoadRenderSystemPlugin(root);
        const Ogre::RenderSystemList& rsList = root->getAvailableRenderers();
        if (rsList.empty()) throw std::runtime_error("렌더 시스템 없음");
        root->setRenderSystem(rsList[0]);
        root->initialise(false);

        // SDL2 창 생성
        const int width = 1280, height = 720;
        sdlWindow = SDL_CreateWindow(
            "PRISM Engine - bunny.obj",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!sdlWindow) throw std::runtime_error("SDL 창 생성 실패");

        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(sdlWindow, &wmInfo);

        Ogre::NameValuePairList params;
        params["externalWindowHandle"] = Ogre::StringConverter::toString(
            reinterpret_cast<size_t>(wmInfo.info.win.window));
        Ogre::Window* ogreWindow = root->createRenderWindow(
            "PRISM Window", width, height, false, &params);

        // HLMS (PBS, Unlit) 등록
        RegisterHlms();
        std::cout << "[DIAG] HLMS 등록 완료\n"; std::cout.flush();

        // SceneManager 생성
        Ogre::SceneManager* sceneManager =
            root->createSceneManager(Ogre::ST_GENERIC, 1u, "MainSM");
        std::cout << "[DIAG] SceneManager 생성 완료\n"; std::cout.flush();

        // Camera 생성
        Ogre::Camera* camera = sceneManager->createCamera("MainCamera");
        std::cout << "[DIAG] Camera 생성 완료\n"; std::cout.flush();
        camera->setNearClipDistance(0.001f);
        camera->setFarClipDistance(100.0f);
        camera->setAutoAspectRatio(true);

        // OBJ 로드 → OGRE Mesh 생성
        std::cout << "[DIAG] OBJ 로드 시작\n"; std::cout.flush();
        ObjMeshData objData = LoadObjFile(
            "D:/Git/P.R.I.S.M/Project/PRISM_Engine/bunny.obj");
        std::cout << "[DIAG] OBJ 로드 완료\n"; std::cout.flush();
        Ogre::VaoManager* vaoManager = root->getRenderSystem()->getVaoManager();
        Ogre::MeshPtr bunnyMesh = CreateMeshFromObj(objData, vaoManager);
        std::cout << "[DIAG] Mesh 생성 완료\n"; std::cout.flush();

        std::cout << "Bunny: " << objData.vertices.size() / 6 << " verts, "
                  << objData.indices.size() / 3 << " faces\n"; std::cout.flush();

        // 카메라를 토끼 중심 앞에 배치
        Ogre::Vector3 center = objData.bounds.mCenter;
        float dist = objData.sphereRadius * 3.5f;
        camera->setPosition(center + Ogre::Vector3(0, 0, dist));
        camera->lookAt(center);
        std::cout << "[DIAG] Camera 위치 설정 완료\n"; std::cout.flush();

        // Compositor (기본 배경색 = 짙은 회색)
        Ogre::CompositorManager2* compositorMgr = root->getCompositorManager2();
        const Ogre::String workspaceName = "PrismWorkspace";
        std::cout << "[DIAG] Compositor 시작\n"; std::cout.flush();
        compositorMgr->createBasicWorkspaceDef(
            workspaceName, Ogre::ColourValue(0.15f, 0.15f, 0.2f));
        std::cout << "[DIAG] createBasicWorkspaceDef 완료\n"; std::cout.flush();
        compositorMgr->addWorkspace(
            sceneManager, ogreWindow->getTexture(), camera, workspaceName, true);
        std::cout << "[DIAG] addWorkspace 완료\n"; std::cout.flush();

        // Bunny Item 생성
        Ogre::Item* bunnyItem = sceneManager->createItem(bunnyMesh, Ogre::SCENE_DYNAMIC);
        std::cout << "[DIAG] Item 생성 완료\n"; std::cout.flush();
        Ogre::SceneNode* bunnyNode =
            sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        bunnyNode->attachObject(bunnyItem);
        std::cout << "[DIAG] SceneNode 부착 완료\n"; std::cout.flush();

        // Directional Light
        Ogre::Light* light = sceneManager->createLight();
        Ogre::SceneNode* lightNode = sceneManager->getRootSceneNode()->createChildSceneNode();
        lightNode->attachObject(light);
        light->setPowerScale(Ogre::Math::PI);
        light->setType(Ogre::Light::LT_DIRECTIONAL);
        light->setDirection(Ogre::Vector3(-1, -1, -1).normalisedCopy());
        std::cout << "[DIAG] Light 설정 완료\n"; std::cout.flush();

        // 반구 환경광 (위=밝음, 아래=어두움)
        sceneManager->setAmbientLight(
            Ogre::ColourValue(0.3f, 0.3f, 0.3f),
            Ogre::ColourValue(0.05f, 0.05f, 0.08f),
            Ogre::Vector3::UNIT_Y);
        std::cout << "[DIAG] AmbientLight 설정 완료\n"; std::cout.flush();

        std::cout << "=== PRISM Engine: bunny.obj 렌더링 시작 (ESC로 종료) ===\n";
        std::cout.flush();

        // 렌더 루프
        bool bQuit = false;
        SDL_Event evt;
        int frameCount = 0;
        while (!bQuit)
        {
            while (SDL_PollEvent(&evt))
            {
                if (evt.type == SDL_QUIT) bQuit = true;
                if (evt.type == SDL_KEYDOWN && evt.key.keysym.sym == SDLK_ESCAPE) bQuit = true;
            }
            if (frameCount == 0) { std::cout << "[DIAG] renderOneFrame 첫 호출\n"; std::cout.flush(); }
            if (!root->renderOneFrame()) bQuit = true;
            if (frameCount == 0) { std::cout << "[DIAG] 첫 프레임 완료\n"; std::cout.flush(); }
            ++frameCount;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Error] " << e.what() << "\n";
    }

    if (root)      delete root;
    if (sdlWindow) SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
}

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
// HLMS (High Level Material System) 등록
// Ogre Next의 HLMS는 수천 가지 조합의 셰이더를 실시간으로 조립하는 시스템입니다.
// 이 함수는 셰이더를 조립하는 데 필요한 '부품(조각 파일)'들의 경로를 설정합니다.
// ============================================================
static void RegisterHlms(Ogre::Root* root) {
    using namespace Ogre;
    RenderSystem* rs = root->getRenderSystem();
    
    // 렌더 시스템에 따라 사용할 셰이더 언어(문법)를 결정합니다.
    String syntax = (rs->getName().find("Direct3D11") != String::npos) ? "HLSL" : "GLSL";
    ArchiveManager& archMgr = ArchiveManager::getSingleton();
    HlmsManager* hlmsMgr = root->getHlmsManager();

    // 개별 HLMS 타입을 등록하는 람다 함수입니다.
    auto reg = [&](HlmsTypes type, const String& name) {
        String mainPath;
        StringVector libPaths;
        
        // 1. 해당 HLMS 타입(PBS/Unlit)이 사용하는 엔진 표준 템플릿 경로를 가져옵니다.
        // mainPath: 셰이더의 입구(Entry point)가 있는 폴더
        // libPaths: 조명 연산, 수학 함수 등 공통 코드 조각들이 들어있는 폴더들
        if (type == HLMS_PBS) HlmsPbs::getDefaultPaths(mainPath, libPaths);
        else HlmsUnlit::getDefaultPaths(mainPath, libPaths);

        // 2. 실제 파일 시스템 폴더를 엔진용 아카이브(Archive) 데이터로 변환하여 로드합니다.
        Archive* archMain = archMgr.load("./Media/" + mainPath, "FileSystem", true);
        ArchiveVec archLibs;
        for (const auto& p : libPaths)
            archLibs.push_back(archMgr.load("./Media/" + p, "FileSystem", true));

        // 3. 실시간 셰이더 조립 엔진(HLMS 객체)을 생성합니다.
        // archMain은 셰이더 템플릿의 핵심 골격을, archLibs는 조립에 쓰일 부품들을 제공합니다.
        Hlms* hlms = nullptr;
        if (type == HLMS_PBS) hlms = OGRE_NEW HlmsPbs(archMain, &archLibs);
        else hlms = OGRE_NEW HlmsUnlit(archMain, &archLibs);
        
        // 4. 생성된 조립 엔진을 매니저에 등록하여 엔진 전체에서 사용 가능하게 합니다.
        hlmsMgr->registerHlms(hlms);
    };

    // 물리 기반 셰이딩(PBS)과 단순 색상 셰이딩(Unlit) 엔진을 각각 등록합니다.
    reg(HLMS_UNLIT, "Unlit");
    reg(HLMS_PBS, "Pbs");

    // LTC Matrix 등 PBS 연산에 필요한 공통 리소스 위치를 등록하고 초기화합니다.
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
        
        // -------------------------------------------------------
        // 광원 설정 (PBS는 매우 강한 빛이 필요합니다)
        // -------------------------------------------------------
        auto light = sm->createLight();
        auto lnode = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        lnode->attachObject(light);
        light->setType(Ogre::Light::LT_DIRECTIONAL);
        lnode->setDirection(Ogre::Vector3(1, -1, -1).normalisedCopy());
        light->setPowerScale(Ogre::Math::PI * 2.0f); // 빛의 세기를 강화 (에너지 보존 법칙 반영)

        // 보조 광원 추가 (반대편에서 비춤)
        auto light2 = sm->createLight();
        auto lnode2 = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        lnode2->attachObject(light2);
        light2->setType(Ogre::Light::LT_DIRECTIONAL);
        lnode2->setDirection(Ogre::Vector3(-1, -1, 1).normalisedCopy());
        light2->setPowerScale(Ogre::Math::PI * 1.0f);

        auto obj = LoadObjFile("bunny.obj");
        auto mesh = CreateMesh(obj, root->getRenderSystem()->getVaoManager());
        
        auto pbs = static_cast<Ogre::HlmsPbs*>(root->getHlmsManager()->getHlms(Ogre::HLMS_PBS));

        // -------------------------------------------------------
        // 토끼 1: 금속(Metal) 재질 설정
        // -------------------------------------------------------
        auto item1 = sm->createItem(mesh);
        auto node1 = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        node1->attachObject(item1);
        node1->setScale(500.0f, 500.0f, 500.0f);
        node1->setPosition(-40.0f, 0, 0);

        auto matMetal = static_cast<Ogre::HlmsPbsDatablock*>(
            pbs->createDatablock("MatMetal", "MatMetal", Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        
        matMetal->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
        // 반사 맵이 없으므로 금속성을 약간 낮춰서 형체가 보이게 조정 (0.8)
        matMetal->setMetalness(0.8f);
        matMetal->setRoughness(0.1f);
        matMetal->setDiffuse(Ogre::Vector3(0.7f, 0.7f, 0.8f)); // 은색 느낌의 기본색
        matMetal->setSpecular(Ogre::Vector3(1.0f, 1.0f, 1.0f)); // 반사광 강조
        item1->setDatablock(matMetal);

        // -------------------------------------------------------
        // 토끼 2: 주황색 플라스틱(Plastic) 재질 설정
        // -------------------------------------------------------
        auto item2 = sm->createItem(mesh);
        auto node2 = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode();
        node2->attachObject(item2);
        node2->setScale(500.0f, 500.0f, 500.0f);
        node2->setPosition(40.0f, 0, 0);

        auto matPlastic = static_cast<Ogre::HlmsPbsDatablock*>(
            pbs->createDatablock("MatPlastic", "MatPlastic", Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        
        matPlastic->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
        matPlastic->setMetalness(0.0f); // 비금속
        matPlastic->setRoughness(0.3f); // 매끄러운 플라스틱
        matPlastic->setDiffuse(Ogre::Vector3(1.0f, 0.4f, 0.0f)); // 주황색
        item2->setDatablock(matPlastic);
        
        // 카메라 위치 초기화
        camNode->setPosition(0, 0, 150); camNode->lookAt(Ogre::Vector3::ZERO, Ogre::Node::TS_WORLD);
        
        auto comp = root->getCompositorManager2();
        comp->createBasicWorkspaceDef("WS", Ogre::ColourValue(0.2f, 0.4f, 0.6f));
        comp->addWorkspace(sm, ogreWin->getTexture(), cam, "WS", true);
        
        std::cout << "=== PRISM Control Guide ===\n";
        std::cout << "[WASD] Move Camera, [Arrows] Rotate Camera, [ESC] Quit\n";
        std::cout.flush();

        bool bQuit = false;
        SDL_Event evt;
        int f = 0;
        Uint32 lastTime = SDL_GetTicks();

        while (!bQuit) {
            // 시간 계산 (deltaTime)
            Uint32 currentTime = SDL_GetTicks();
            float deltaTime = (currentTime - lastTime) / 1000.0f;
            lastTime = currentTime;

            while (SDL_PollEvent(&evt)) {
                if (evt.type == SDL_QUIT) bQuit = true;
                if (evt.type == SDL_KEYDOWN && evt.key.keysym.sym == SDLK_ESCAPE) bQuit = true;
            }

            // 키보드 상태 실시간 감지
            const Uint8* state = SDL_GetKeyboardState(NULL);
            float moveSpeed = 100.0f * deltaTime; // 초당 100 유닛 이동
            float rotSpeed = 1.5f * deltaTime;    // 초당 1.5 라디안 회전

            Ogre::Vector3 moveVec = Ogre::Vector3::ZERO;
            if (state[SDL_SCANCODE_W]) moveVec.z -= 1;
            if (state[SDL_SCANCODE_S]) moveVec.z += 1;
            if (state[SDL_SCANCODE_A]) moveVec.x -= 1;
            if (state[SDL_SCANCODE_D]) moveVec.x += 1;
            if (state[SDL_SCANCODE_Q]) moveVec.y -= 1;
            if (state[SDL_SCANCODE_E]) moveVec.y += 1;

            if (moveVec != Ogre::Vector3::ZERO) {
                camNode->translate(moveVec.normalisedCopy() * moveSpeed, Ogre::Node::TS_LOCAL);
            }

            // 방향키로 회전
            if (state[SDL_SCANCODE_LEFT])  camNode->yaw(Ogre::Radian(rotSpeed));
            if (state[SDL_SCANCODE_RIGHT]) camNode->yaw(Ogre::Radian(-rotSpeed));
            if (state[SDL_SCANCODE_UP])    camNode->pitch(Ogre::Radian(rotSpeed));
            if (state[SDL_SCANCODE_DOWN])  camNode->pitch(Ogre::Radian(-rotSpeed));

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

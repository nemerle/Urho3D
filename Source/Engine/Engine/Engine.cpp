//
// Copyright (c) 2008-2014 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Audio.h"
#include "Console.h"
#include "Context.h"
#include "CoreEvents.h"
#include "DebugHud.h"
#include "Engine.h"
#include "FileSystem.h"
#include "Graphics.h"
#include "Input.h"
#include "InputEvents.h"
#include "Log.h"
#ifdef URHO3D_NAVIGATION
#include "NavigationMesh.h"
#endif
#ifdef URHO3D_NETWORK
#include "Network.h"
#endif
#include "PackageFile.h"
#ifdef URHO3D_PHYSICS
#include "PhysicsWorld.h"
#endif
#include "ProcessUtils.h"
#include "Profiler.h"
#include "Renderer.h"
#include "ResourceCache.h"
#include "Scene.h"
#include "SceneEvents.h"
#include "UI.h"
#ifdef URHO3D_URHO2D
#include "Urho2D.h"
#endif
#include "WorkQueue.h"
#include "XMLFile.h"

#include "DebugNew.h"

#if defined(_MSC_VER) && defined(_DEBUG)
// From dbgint.h
#define nNoMansLandSize 4

typedef struct _CrtMemBlockHeader
{
    struct _CrtMemBlockHeader* pBlockHeaderNext;
    struct _CrtMemBlockHeader* pBlockHeaderPrev;
    char* szFileName;
    int nLine;
    size_t nDataSize;
    int nBlockUse;
    long lRequest;
    unsigned char gap[nNoMansLandSize];
} _CrtMemBlockHeader;
#endif

namespace Urho3D
{

extern const char* logLevelPrefixes[];

Engine::Engine(Context* context) :
    Object(context),
    timeStep_(0.0f),
    timeStepSmoothing_(2),
    minFps_(10),
    #if defined(ANDROID) || defined(IOS) || defined(RASPI)
    maxFps_(60),
    maxInactiveFps_(10),
    pauseMinimized_(true),
    #else
    maxFps_(200),
    maxInactiveFps_(60),
    pauseMinimized_(false),
    #endif
#ifdef URHO3D_TESTING
    timeOut_(0),
#endif
    autoExit_(true),
    initialized_(false),
    exiting_(false),
    headless_(false),
    audioPaused_(false)
{
    // Register self as a subsystem
    context_->RegisterSubsystem(this);

    // Create subsystems which do not depend on engine initialization or startup parameters
    context_->RegisterSubsystem(new Time(context_));
    context_->RegisterSubsystem(new WorkQueue(context_));
    #ifdef URHO3D_PROFILING
    context_->RegisterSubsystem(new Profiler(context_));
    #endif
    context_->RegisterSubsystem(new FileSystem(context_));
    #ifdef URHO3D_LOGGING
    context_->RegisterSubsystem(new Log(context_));
    #endif
    context_->RegisterSubsystem(new ResourceCache(context_));
    #ifdef URHO3D_NETWORK
    context_->RegisterSubsystem(new Network(context_));
    #endif
    context_->RegisterSubsystem(new Input(context_));
    context_->RegisterSubsystem(new Audio(context_));
    context_->RegisterSubsystem(new UI(context_));

    // Register object factories for libraries which are not automatically registered along with subsystem creation
    RegisterSceneLibrary(context_);

#ifdef URHO3D_PHYSICS
    RegisterPhysicsLibrary(context_);
#endif

#ifdef URHO3D_NAVIGATION
    RegisterNavigationLibrary(context_);
#endif

    SubscribeToEvent(E_EXITREQUESTED, HANDLER(Engine, HandleExitRequested));
}

Engine::~Engine()
{
}

bool Engine::Initialize(const VariantMap& parameters)
{
    if (initialized_)
        return true;

    PROFILE(InitEngine);

    // Set headless mode
    headless_ = GetParameter(parameters, "Headless", false).GetBool();

    // Register the rest of the subsystems
    if (!headless_)
    {
        context_->RegisterSubsystem(new Graphics(context_));
        context_->RegisterSubsystem(new Renderer(context_));
    }
    else
    {
        // Register graphics library objects explicitly in headless mode to allow them to work without using actual GPU resources
        RegisterGraphicsLibrary(context_);
    }

#ifdef URHO3D_URHO2D
    // 2D graphics library is dependent on 3D graphics library
    RegisterUrho2DLibrary(context_);
#endif

    // Start logging
    Log* log = GetSubsystem<Log>();
    if (log)
    {
        if (HasParameter(parameters, "LogLevel"))
            log->SetLevel(GetParameter(parameters, "LogLevel").GetInt());
        log->SetQuiet(GetParameter(parameters, "LogQuiet", false).GetBool());
        log->Open(GetParameter(parameters, "LogName", "Urho3D.log").GetString());
    }

    // Set maximally accurate low res timer
    GetSubsystem<Time>()->SetTimerPeriod(1);

    // Configure max FPS
    if (GetParameter(parameters, "FrameLimiter", true) == false)
        SetMaxFps(0);

    // Set amount of worker threads according to the available physical CPU cores. Using also hyperthreaded cores results in
    // unpredictable extra synchronization overhead. Also reserve one core for the main thread
    unsigned numThreads = GetParameter(parameters, "WorkerThreads", true).GetBool() ? GetNumPhysicalCPUs() - 1 : 0;
    if (numThreads)
    {
        GetSubsystem<WorkQueue>()->CreateThreads(numThreads);

        LOGINFOF("Created %u worker thread%s", numThreads, numThreads > 1 ? "s" : "");
    }

    // Add resource paths
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    FileSystem* fileSystem = GetSubsystem<FileSystem>();
    String exePath = fileSystem->GetProgramDir();

    Vector<String> resourcePaths = GetParameter(parameters, "ResourcePaths", "Data;CoreData").GetString().split(';');
    Vector<String> resourcePackages = GetParameter(parameters, "ResourcePackages").GetString().split(';');
    Vector<String> autoloadFolders = GetParameter(parameters, "AutoloadPaths", "Extra").GetString().split(';');

    for (unsigned i = 0; i < resourcePaths.size(); ++i)
    {
        bool success = false;

        // If path is not absolute, prefer to add it as a package if possible
        if (!IsAbsolutePath(resourcePaths[i]))
        {
            String packageName = exePath + resourcePaths[i] + ".pak";
            if (fileSystem->FileExists(packageName))
            {
                SharedPtr<PackageFile> package(new PackageFile(context_));
                if (package->Open(packageName))
                {
                    cache->AddPackageFile(package);
                    success = true;
                }
            }

            if (!success)
            {
                String pathName = exePath + resourcePaths[i];
                if (fileSystem->DirExists(pathName))
                    success = cache->AddResourceDir(pathName);
            }
        }
        else
        {
            String pathName = resourcePaths[i];
            if (fileSystem->DirExists(pathName))
                success = cache->AddResourceDir(pathName);
        }

        if (!success)
        {
            LOGERROR("Failed to add resource path " + resourcePaths[i]);
            return false;
        }
    }

    // Then add specified packages
    for (unsigned i = 0; i < resourcePackages.size(); ++i)
    {
        bool success = false;

        String packageName = exePath + resourcePackages[i];
        if (fileSystem->FileExists(packageName))
        {
            SharedPtr<PackageFile> package(new PackageFile(context_));
            if (package->Open(packageName))
            {
                cache->AddPackageFile(package);
                success = true;
            }
        }

        if (!success)
        {
            LOGERROR("Failed to add resource package " + resourcePackages[i]);
            return false;
        }
    }

    // Add auto load folders. Prioritize these (if exist) before the default folders
    for (unsigned i = 0; i < autoloadFolders.size(); ++i)
    {
        bool success = true;
        String autoloadFolder = autoloadFolders[i];
        String badResource;
        if (fileSystem->DirExists(autoloadFolder))
        {
            Vector<String> folders;
            fileSystem->ScanDir(folders, autoloadFolder, "*", SCAN_DIRS, false);
            for (unsigned y = 0; y < folders.size(); ++y)
            {
                String folder = folders[y];
                if (folder.startsWith("."))
                    continue;

                String autoResourceDir = exePath + autoloadFolder + "/" + folder;
                success = cache->AddResourceDir(autoResourceDir, 0);
                if (!success)
                {
                    badResource = folder;
                    break;
                }
            }

            if (success)
            {
                Vector<String> paks;
                fileSystem->ScanDir(paks, autoloadFolder, "*.pak", SCAN_FILES, false);
                for (unsigned y = 0; y < paks.size(); ++y)
                {
                    String pak = paks[y];
                    if (pak.startsWith("."))
                        continue;

                    String autoResourcePak = exePath + autoloadFolder + "/" + pak;
                    SharedPtr<PackageFile> package(new PackageFile(context_));
                    if (package->Open(autoResourcePak))
                        cache->AddPackageFile(package, 0);
                    else
                    {
                        badResource = autoResourcePak;
                        success = false;
                        break;
                    }
                }
            }
        }
        else
            LOGWARNING("Skipped autoload folder " + autoloadFolders[i] + " as it does not exist");

        if (!success)
        {
            LOGERROR("Failed to add resource " + badResource + " in autoload folder " + autoloadFolders[i]);
            return false;
        }
    }

    // Initialize graphics & audio output
    if (!headless_)
    {
        Graphics* graphics = GetSubsystem<Graphics>();
        Renderer* renderer = GetSubsystem<Renderer>();

        if (HasParameter(parameters, "ExternalWindow"))
            graphics->SetExternalWindow(GetParameter(parameters, "ExternalWindow").GetVoidPtr());
        graphics->SetForceSM2(GetParameter(parameters, "ForceSM2", false).GetBool());
        graphics->SetWindowTitle(GetParameter(parameters, "WindowTitle", "Urho3D").GetString());
        graphics->SetWindowIcon(cache->GetResource<Image>(GetParameter(parameters, "WindowIcon", String::EMPTY).GetString()));
        graphics->SetFlushGPU(GetParameter(parameters, "FlushGPU", false).GetBool());
        graphics->SetOrientations(GetParameter(parameters, "Orientations", "LandscapeLeft LandscapeRight").GetString());

        if (HasParameter(parameters, "WindowPositionX") && HasParameter(parameters, "WindowPositionY"))
            graphics->SetWindowPosition(GetParameter(parameters, "WindowPositionX").GetInt(), GetParameter(parameters, "WindowPositionY").GetInt());

        if (!graphics->SetMode(
            GetParameter(parameters, "WindowWidth", 0).GetInt(),
            GetParameter(parameters, "WindowHeight", 0).GetInt(),
            GetParameter(parameters, "FullScreen", true).GetBool(),
            GetParameter(parameters, "Borderless", false).GetBool(),
            GetParameter(parameters, "WindowResizable", false).GetBool(),
            GetParameter(parameters, "VSync", false).GetBool(),
            GetParameter(parameters, "TripleBuffer", false).GetBool(),
            GetParameter(parameters, "MultiSample", 1).GetInt()
        ))
            return false;

        if (HasParameter(parameters, "DumpShaders"))
            graphics->BeginDumpShaders(GetParameter(parameters, "DumpShaders", String::EMPTY).GetString());
        if (HasParameter(parameters, "RenderPath"))
            renderer->SetDefaultRenderPath(cache->GetResource<XMLFile>(GetParameter(parameters, "RenderPath").GetString()));

        renderer->SetDrawShadows(GetParameter(parameters, "Shadows", true).GetBool());
        if (renderer->GetDrawShadows() && GetParameter(parameters, "LowQualityShadows", false).GetBool())
            renderer->SetShadowQuality(SHADOWQUALITY_LOW_16BIT);
        renderer->SetMaterialQuality(GetParameter(parameters, "MaterialQuality", QUALITY_HIGH).GetInt());
        renderer->SetTextureQuality(GetParameter(parameters, "TextureQuality", QUALITY_HIGH).GetInt());
        renderer->SetTextureFilterMode((TextureFilterMode)GetParameter(parameters, "TextureFilterMode", FILTER_TRILINEAR).GetInt());
        renderer->SetTextureAnisotropy(GetParameter(parameters, "TextureAnisotropy", 4).GetInt());

        if (GetParameter(parameters, "Sound", true).GetBool())
        {
            GetSubsystem<Audio>()->SetMode(
                GetParameter(parameters, "SoundBuffer", 100).GetInt(),
                GetParameter(parameters, "SoundMixRate", 44100).GetInt(),
                GetParameter(parameters, "SoundStereo", true).GetBool(),
                GetParameter(parameters, "SoundInterpolation", true).GetBool()
            );
        }
    }

    // Init FPU state of main thread
    InitFPU();

    // Initialize input
    if (HasParameter(parameters, "TouchEmulation"))
        GetSubsystem<Input>()->SetTouchEmulation(GetParameter(parameters, "TouchEmulation").GetBool());

    #ifdef URHO3D_TESTING
    if (HasParameter(parameters, "TimeOut"))
        timeOut_ = GetParameter(parameters, "TimeOut", 0).GetInt() * 1000000LL;
    #endif

    // In debug mode, check now that all factory created objects can be created without crashing
    #ifdef _DEBUG
    const HashMap<StringHash, SharedPtr<ObjectFactory> >& factories = context_->GetObjectFactories();
    for (const auto & factorie : factories)
        SharedPtr<Object> object = factorie.second->CreateObject();
    #endif

    frameTimer_.Reset();

    LOGINFO("Initialized engine");
    initialized_ = true;
    return true;
}

void Engine::RunFrame()
{
    assert(initialized_);

    // If not headless, and the graphics subsystem no longer has a window open, assume we should exit
    if (!headless_ && !GetSubsystem<Graphics>()->IsInitialized())
        exiting_ = true;

    if (exiting_)
        return;

    // Note: there is a minimal performance cost to looking up subsystems (uses a hashmap); if they would be looked up several
    // times per frame it would be better to cache the pointers
    Time* time = GetSubsystem<Time>();
    Input* input = GetSubsystem<Input>();
    Audio* audio = GetSubsystem<Audio>();

    time->BeginFrame(timeStep_);

    // If pause when minimized -mode is in use, stop updates and audio as necessary
    if (pauseMinimized_ && input->IsMinimized())
    {
        if (audio->IsPlaying())
        {
            audio->Stop();
            audioPaused_ = true;
        }
    }
    else
    {
        // Only unpause when it was paused by the engine
        if (audioPaused_)
        {
            audio->Play();
            audioPaused_ = false;
        }

        Update();
    }

    Render();
    ApplyFrameLimit();

    time->EndFrame();
}

Console* Engine::CreateConsole()
{
    if (headless_ || !initialized_)
        return nullptr;

    // Return existing console if possible
    Console* console = GetSubsystem<Console>();
    if (!console)
    {
        console = new Console(context_);
        context_->RegisterSubsystem(console);
    }

    return console;
}

DebugHud* Engine::CreateDebugHud()
{
    if (headless_ || !initialized_)
        return nullptr;

     // Return existing debug HUD if possible
    DebugHud* debugHud = GetSubsystem<DebugHud>();
    if (!debugHud)
    {
        debugHud = new DebugHud(context_);
        context_->RegisterSubsystem(debugHud);
    }

    return debugHud;
}

void Engine::SetTimeStepSmoothing(int frames)
{
    timeStepSmoothing_ = Clamp(frames, 1, 20);
}

void Engine::SetMinFps(int fps)
{
    minFps_ = Max(fps, 0);
}

void Engine::SetMaxFps(int fps)
{
    maxFps_ = Max(fps, 0);
}

void Engine::SetMaxInactiveFps(int fps)
{
    maxInactiveFps_ = Max(fps, 0);
}

void Engine::SetPauseMinimized(bool enable)
{
    pauseMinimized_ = enable;
}

void Engine::SetAutoExit(bool enable)
{
    // On mobile platforms exit is mandatory if requested by the platform itself and should not be attempted to be disabled
#if defined(ANDROID) || defined(IOS)
    enable = true;
#endif
    autoExit_ = enable;
}

void Engine::SetNextTimeStep(float seconds)
{
    timeStep_ = Max(seconds, 0.0f);
}

void Engine::Exit()
{
#if defined(IOS)
    // On iOS it's not legal for the application to exit on its own, instead it will be minimized with the home key
#else
    DoExit();
#endif
}

void Engine::DumpProfiler()
{
    Profiler* profiler = GetSubsystem<Profiler>();
    if (profiler)
        LOGRAW(profiler->GetData(true, true) + "\n");
}

void Engine::DumpResources(bool dumpFileName)
{
    #ifdef URHO3D_LOGGING
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    const HashMap<StringHash, ResourceGroup>& resourceGroups = cache->GetAllResources();
    LOGRAW("\n");

    if (dumpFileName)
    {
        LOGRAW("Used resources:\n");
    }

    for (const auto & entry : resourceGroups)
    {
        const ResourceGroup & resourceGroup(entry.second);
        const HashMap<StringHash, SharedPtr<Resource> >& resources = resourceGroup.resources_;
        if (dumpFileName)
        {
            for (auto j : resources)
            {
                LOGRAW(ELEMENT_VALUE(j)->GetName() + "\n");
            }

        }
        else
        {
            unsigned num = resources.size();
            unsigned memoryUse = resourceGroup.memoryUse_;

            if (num)
            {
                LOGRAW("Resource type " + MAP_VALUE(resources.begin())->GetTypeName() +
                    ": count " + String(num) + " memory use " + String(memoryUse) + "\n");
            }
        }
    }

    if (!dumpFileName)
    {
        LOGRAW("Total memory use of all resources " + String(cache->GetTotalMemoryUse()) + "\n\n");
    }
    #endif
}

void Engine::DumpMemory()
{
    #ifdef URHO3D_LOGGING
    #if defined(_MSC_VER) && defined(_DEBUG)
    _CrtMemState state;
    _CrtMemCheckpoint(&state);
    _CrtMemBlockHeader* block = state.pBlockHeader;
    unsigned total = 0;
    unsigned blocks = 0;

    for (;;)
    {
        if (block && block->pBlockHeaderNext)
            block = block->pBlockHeaderNext;
        else
            break;
    }

    while (block)
    {
        if (block->nBlockUse > 0)
        {
            if (block->szFileName)
                LOGRAW("Block " + String((int)block->lRequest) + ": " + String(block->nDataSize) + " bytes, file " + String(block->szFileName) + " line " + String(block->nLine) + "\n");
            else
                LOGRAW("Block " + String((int)block->lRequest) + ": " + String(block->nDataSize) + " bytes\n");

            total += block->nDataSize;
            ++blocks;
        }
        block = block->pBlockHeaderPrev;
    }

    LOGRAW("Total allocated memory " + String(total) + " bytes in " + String(blocks) + " blocks\n\n");
    #else
    LOGRAW("DumpMemory() supported on MSVC debug mode only\n\n");
    #endif
    #endif
}

void Engine::Update()
{
    PROFILE(Update);

    // Logic update event
    using namespace Update;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_TIMESTEP] = timeStep_;
    SendEvent(E_UPDATE, eventData);

    // Logic post-update event
    SendEvent(E_POSTUPDATE, eventData);

    // Rendering update event
    SendEvent(E_RENDERUPDATE, eventData);

    // Post-render update event
    SendEvent(E_POSTRENDERUPDATE, eventData);
}

void Engine::Render()
{
    if (headless_)
        return;

    PROFILE(Render);

    // If device is lost, BeginFrame will fail and we skip rendering
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics->BeginFrame())
        return;

    GetSubsystem<Renderer>()->Render();
    GetSubsystem<UI>()->Render();
    graphics->EndFrame();
}

void Engine::ApplyFrameLimit()
{
    if (!initialized_)
        return;

    int maxFps = maxFps_;
    Input* input = GetSubsystem<Input>();
    if (input && !input->HasFocus())
        maxFps = Min(maxInactiveFps_, maxFps);

    long long elapsed = 0;

    // Perform waiting loop if maximum FPS set
    if (maxFps)
    {
        PROFILE(ApplyFrameLimit);

        long long targetMax = 1000000LL / maxFps;

        for (;;)
        {
            elapsed = frameTimer_.GetUSec();
            if (elapsed >= targetMax)
                break;

            // Sleep if 1 ms or more off the frame limiting goal
            if (targetMax - elapsed >= 1000LL)
            {
                unsigned sleepTime = (unsigned)((targetMax - elapsed) / 1000LL);
                Time::Sleep(sleepTime);
            }
        }
    }

    elapsed = frameTimer_.GetUSec();
    frameTimer_.Reset();
    #ifdef URHO3D_TESTING
    if (timeOut_ > 0)
    {
        timeOut_ -= elapsed;
        if (timeOut_ <= 0)
            Exit();
    }
    #endif

    // If FPS lower than minimum, clamp elapsed time
    if (minFps_)
    {
        long long targetMin = 1000000LL / minFps_;
        if (elapsed > targetMin)
            elapsed = targetMin;
    }

    // Perform timestep smoothing
    timeStep_ = 0.0f;
    lastTimeSteps_.push_back(elapsed / 1000000.0f);
    if (lastTimeSteps_.size() > timeStepSmoothing_)
    {
        // If the smoothing configuration was changed, ensure correct amount of samples
        lastTimeSteps_.erase(lastTimeSteps_.begin(), lastTimeSteps_.begin() + lastTimeSteps_.size() - timeStepSmoothing_);
        for (unsigned i = 0; i < lastTimeSteps_.size(); ++i)
            timeStep_ += lastTimeSteps_[i];
        timeStep_ /= lastTimeSteps_.size();
    }
    else
        timeStep_ = lastTimeSteps_.back();
}

VariantMap Engine::ParseParameters(const Vector<String>& arguments)
{
    VariantMap ret;

    for (unsigned i = 0; i < arguments.size(); ++i)
    {
        if (arguments[i].length() > 1 && arguments[i][0] == '-')
        {
            String argument = arguments[i].Substring(1).toLower();
            String value = i + 1 < arguments.size() ? arguments[i + 1] : String::EMPTY;

            if (argument == "headless")
                ret["Headless"] = true;
            else if (argument == "nolimit")
                ret["FrameLimiter"] = false;
            else if (argument == "flushgpu")
                ret["FlushGPU"] = true;
            else if (argument == "landscape")
                ret["Orientations"] = "LandscapeLeft LandscapeRight " + ret["Orientations"].GetString();
            else if (argument == "portrait")
                ret["Orientations"] = "Portrait PortraitUpsideDown " + ret["Orientations"].GetString();
            else if (argument == "nosound")
                ret["Sound"] = false;
            else if (argument == "noip")
                ret["SoundInterpolation"] = false;
            else if (argument == "mono")
                ret["SoundStereo"] = false;
            else if (argument == "prepass")
                ret["RenderPath"] = "RenderPaths/Prepass.xml";
            else if (argument == "deferred")
                ret["RenderPath"] = "RenderPaths/Deferred.xml";
            else if (argument == "noshadows")
                ret["Shadows"] = false;
            else if (argument == "lqshadows")
                ret["LowQualityShadows"] = true;
            else if (argument == "nothreads")
                ret["WorkerThreads"] = false;
            else if (argument == "sm2")
                ret["ForceSM2"] = true;
            else if (argument == "v")
                ret["VSync"] = true;
            else if (argument == "t")
                ret["TripleBuffer"] = true;
            else if (argument == "w")
                ret["FullScreen"] = false;
            else if (argument == "s")
                ret["WindowResizable"] = true;
            else if (argument == "borderless")
                ret["Borderless"] = true;
            else if (argument == "q")
                ret["LogQuiet"] = true;
            else if (argument == "log" && !value.isEmpty())
            {
                int logLevel = GetStringListIndex(value.CString(), logLevelPrefixes, -1);
                if (logLevel != -1)
                {
                    ret["LogLevel"] = logLevel;
                    ++i;
                }
            }
            else if (argument == "x" && !value.isEmpty())
            {
                ret["WindowWidth"] = ToInt(value);
                ++i;
            }
            else if (argument == "y" && !value.isEmpty())
            {
                ret["WindowHeight"] = ToInt(value);
                ++i;
            }
            else if (argument == "m" && !value.isEmpty())
            {
                ret["MultiSample"] = ToInt(value);
                ++i;
            }
            else if (argument == "b" && !value.isEmpty())
            {
                ret["SoundBuffer"] = ToInt(value);
                ++i;
            }
            else if (argument == "r" && !value.isEmpty())
            {
                ret["SoundMixRate"] = ToInt(value);
                ++i;
            }
            else if (argument == "p" && !value.isEmpty())
            {
                ret["ResourcePaths"] = value;
                ++i;
            }
            else if (argument == "ap" && !value.isEmpty())
            {
                ret["AutoloadPaths"] = value;
                ++i;
            }
            else if (argument == "ds" && !value.isEmpty())
            {
                ret["DumpShaders"] = value;
                ++i;
            }
            else if (argument == "mq" && !value.isEmpty())
            {
                ret["MaterialQuality"] = ToInt(value);
                ++i;
            }
            else if (argument == "tq" && !value.isEmpty())
            {
                ret["TextureQuality"] = ToInt(value);
                ++i;
            }
            else if (argument == "tf" && !value.isEmpty())
            {
                ret["TextureFilterMode"] = ToInt(value);
                ++i;
            }
            else if (argument == "af" && !value.isEmpty())
            {
                ret["TextureFilterMode"] = FILTER_ANISOTROPIC;
                ret["TextureAnisotropy"] = ToInt(value);
                ++i;
            }
            else if (argument == "touch")
                ret["TouchEmulation"] = true;
            #ifdef URHO3D_TESTING
            else if (argument == "timeout" && !value.Empty())
            {
                ret["TimeOut"] = ToInt(value);
                ++i;
            }
            #endif
        }
    }

    return ret;
}

bool Engine::HasParameter(const VariantMap& parameters, const String& parameter)
{
    StringHash nameHash(parameter);
    return parameters.find(nameHash) != parameters.end();
}

const Variant& Engine::GetParameter(const VariantMap& parameters, const String& parameter, const Variant& defaultValue)
{
    StringHash nameHash(parameter);
    VariantMap::const_iterator i = parameters.find(nameHash);
    return i != parameters.end() ? MAP_VALUE(i) : defaultValue;
}

void Engine::HandleExitRequested(StringHash eventType, VariantMap& eventData)
{
    if (autoExit_)
    {
        // Do not call Exit() here, as it contains mobile platform -specific tests to not exit.
        // If we do receive an exit request from the system on those platforms, we must comply
        DoExit();
    }
}

void Engine::DoExit()
{
    Graphics* graphics = GetSubsystem<Graphics>();
    if (graphics)
        graphics->Close();

    exiting_ = true;
}

}

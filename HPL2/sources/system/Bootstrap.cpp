#include <system/Bootstrap.h>

#include <cstdint>
#include <memory>

#include "Common_3/Utilities/Interfaces/IFileSystem.h"
#include "engine/IUpdateEventLoop.h"
#include "engine/Interface.h"
#include "graphics/GraphicsAllocator.h"
#include "gui/GuiSet.h"
#include "input/InputKeyboardDevice.h"
#include "input/InputManager.h"

#include <graphics/RendererDeferred.h>

#include "scene/Viewport.h"

#include <input/InputMouseDevice.h>
#include <windowing/NativeWindow.h>

namespace hpl {
    Bootstrap::Bootstrap() {
    }
    Bootstrap::~Bootstrap() {
    }

    void Bootstrap::BootstrapThreadHandler(void* userData) {
        auto bootstrap = reinterpret_cast<Bootstrap*>(userData);
        bootstrap->m_handler();
        bootstrap->m_isRunning = false;
    }

    void Bootstrap::Run(std::function<void()> handler) {
        m_isRunning = true;
        m_handler = handler;
        ThreadDesc threadDesc = {};
        threadDesc.pFunc = BootstrapThreadHandler;
        threadDesc.pData = this;
        initThread(&threadDesc, &m_thread);

        while (m_isRunning) {
            m_window.Process();
        }

    }

    void Bootstrap::Initialize(BootstrapConfiguration configuration) {
        if (!initMemAlloc("Amnesia"))
        {
            return;
        }

	    FileSystemInitDesc fsDesc = {};
        fsDesc.pAppName = "HPL2";

        if (!initFileSystem(&fsDesc)) {
            LOGF(eERROR, "Filesystem failed to initialize.");
        }


        initLog("HPL2", DEFAULT_LOG_LEVEL);
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_BINARIES, "./CompiledShaders");
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_LOG, "./log");
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_GPU_CONFIG, "./");
        //fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_SOURCES, "./Shaders");

        Interface<IUpdateEventLoop>::Register(&m_updateEventLoop);

        auto keyboardHandle = hpl::input::internal::keyboard::Initialize();
        auto mouseHandle = hpl::input::internal::mouse::Initialize();
        auto windowHandle = hpl::window::internal::Initialize(configuration.m_windowStyle);

        // set internal event handlers
        hpl::window::internal::ConnectInternalEventHandler(windowHandle,
            hpl::input::internal::keyboard::GetWindowEventHandle(keyboardHandle));
        hpl::window::internal::ConnectInternalEventHandler(windowHandle,
            hpl::input::internal::mouse::GetWindowEventHandle(mouseHandle));

        m_window = hpl::window::NativeWindowWrapper(std::move(windowHandle));
        m_window.SetWindowSize(cVector2l(1920, 1080));

        // core renderer initialization
        m_renderer.InitializeRenderer(&m_window);

        // initialize gui rendering
        initResourceLoaderInterface(m_renderer.Rend()); // initializes resources

        // graphics allocator
        m_graphicsAlloc = std::make_unique<hpl::GraphicsAllocator>(&m_renderer);

        gui::InitializeGui(m_renderer);

        // this is safe because the render target is scheduled on the api thread

        // register input devices
        m_inputManager.Register(input::InputManager::KeyboardDeviceID, std::make_shared<input::InputKeyboardDevice>(std::move(keyboardHandle)));
        m_inputManager.Register(input::InputManager::MouseDeviceID, std::make_shared<input::InputMouseDevice>(std::move(mouseHandle)));

        Interface<hpl::ForgeRenderer>::Register(&m_renderer);
        Interface<input::InputManager>::Register(&m_inputManager);
        Interface<window::NativeWindowWrapper>::Register(&m_window); // storing as a singleton means we can only have one window ...
        Interface<hpl::GraphicsAllocator>::Register(m_graphicsAlloc.get());
    }

    void Bootstrap::Shutdown() {
        Interface<hpl::ForgeRenderer>::UnRegister(&m_renderer);
        Interface<input::InputManager>::UnRegister(&m_inputManager);
        Interface<window::NativeWindowWrapper>::UnRegister(&m_window);
        Interface<IUpdateEventLoop>::UnRegister(&m_updateEventLoop);
        Interface<hpl::GraphicsAllocator>::UnRegister(m_graphicsAlloc.get());
        exitLog();
    }

}; // namespace hpl

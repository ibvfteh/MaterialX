//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteSession.h>

#include <nanogui/common.h>
#include <MaterialXRenderGlsl/GLUtil.h>

#include <filesystem>

#include <stdexcept>
#include <utility>
#include <exception>

MATERIALX_NAMESPACE_BEGIN

RemoteSession::~RemoteSession()
{
    try
    {
        stop();
    }
    catch (...)
    {
        // Suppress destructor exceptions.
    }
}

void RemoteSession::start()
{
    std::shared_ptr<std::promise<void>> startup;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_state != State::Idle)
        {
            throw std::logic_error("RemoteSession already running");
        }
        _state = State::Starting;
        _renderThreadException = nullptr;
        startup = std::make_shared<std::promise<void>>();
        _startupPromise = startup;
        _startupFuture = startup->get_future();
    }

    _renderThread = std::thread(&RemoteSession::renderLoop, this, startup);

    _startupFuture.get();

    std::exception_ptr exception;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        exception = _renderThreadException;
    }

    if (exception)
    {
        if (_renderThread.joinable())
        {
            _renderThread.join();
        }
        std::rethrow_exception(exception);
    }
}

void RemoteSession::stop()
{
    std::shared_ptr<RemoteViewer> viewer;
    {
        std::unique_lock<std::mutex> lock(_stateMutex);
        if (_state == State::Idle)
        {
            lock.unlock();
            if (_renderThread.joinable())
            {
                _renderThread.join();
            }
            return;
        }

        if (_state == State::Starting)
        {
            lock.unlock();
            if (_startupFuture.valid())
            {
                _startupFuture.wait();
            }
            lock.lock();
        }

        if (_state == State::Running || _state == State::Stopping)
        {
            viewer = _viewer;
            _state = State::Stopping;
        }
        else
        {
            return;
        }
    }

    if (viewer && nanogui::active())
    {
        nanogui::async([viewer = std::move(viewer)]() {
            viewer->requestExit();
            nanogui::leave();
        });
    }
    else
    {
        nanogui::leave();
    }

    if (_renderThread.joinable())
    {
        _renderThread.join();
    }

    std::lock_guard<std::mutex> lock(_stateMutex);
    _viewer.reset();
    _state = State::Idle;
    _startupPromise.reset();
}

// Session material storage (in-memory): only keep file path and name.
// No file copies are made.
SessionMaterial RemoteSession::selectMaterialFromPath(const std::string& filePath)
{
    SessionMaterial sm;
    sm.filePath = filePath;
    // Populate name from the file path base name for convenience
    try
    {
        std::filesystem::path p(filePath);
        sm.name = p.filename().u8string();
    }
    catch (...) { sm.name = std::string(); }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _sessionMaterial = sm;
    }

    return sm;
}

bool RemoteSession::hasSessionMaterial() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return !_sessionMaterial.filePath.empty() || !_sessionMaterial.name.empty();
}

SessionMaterial RemoteSession::currentSessionMaterial() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _sessionMaterial;
}

void RemoteSession::clearSessionMaterial()
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _sessionMaterial = SessionMaterial();
}

void RemoteSession::renderLoop(std::shared_ptr<std::promise<void>> startupPromise)
{
    bool nanoguiInitialized = false;
    bool startupDelivered = false;
    EglHeadlessContext eglCtx;
    const bool useEgl = _config.viewerOptions.backend == RemoteViewer::Options::Backend::EGLHeadless;

    try
    {
        if (useEgl)
        {
            if (!createEglHeadlessContext(eglCtx))
            {
                throw std::runtime_error("Failed to initialize EGL headless context");
            }
        }

        nanogui::init();
        nanoguiInitialized = true;

        auto viewer = std::make_shared<RemoteViewer>(_config.viewerOptions);
        viewer->initializeRemote();

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _viewer = viewer;
            _state = State::Running;
        }

        startupPromise->set_value();
        startupDelivered = true;

        const float refresh = _config.refreshPeriodMs;
        nanogui::mainloop(refresh);

        if (nanoguiInitialized)
        {
            nanogui::shutdown();
        }

        if (useEgl)
        {
            destroyEglHeadlessContext(eglCtx);
        }

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _viewer.reset();
            _state = State::Idle;
            _renderThreadException = nullptr;
        }
    }
    catch (...)
    {
        std::exception_ptr eptr = std::current_exception();

        if (!startupDelivered)
        {
            startupPromise->set_exception(eptr);
        }
        else
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _renderThreadException = eptr;
        }

        if (nanoguiInitialized)
        {
            if (nanogui::active())
            {
                nanogui::leave();
            }
            nanogui::shutdown();
        }

        if (useEgl)
        {
            destroyEglHeadlessContext(eglCtx);
        }

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _viewer.reset();
            _state = State::Idle;
        }

        _startupPromise.reset();

        return;
    }

    _startupPromise.reset();
}

MATERIALX_NAMESPACE_END

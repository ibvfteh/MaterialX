//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteSession.h>

#include <nanogui/common.h>

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

void RemoteSession::renderLoop(std::shared_ptr<std::promise<void>> startupPromise)
{
    bool nanoguiInitialized = false;
    bool startupDelivered = false;

    try
    {
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

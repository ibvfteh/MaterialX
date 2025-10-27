//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_REMOTESESSION_H
#define MATERIALXREMOTE_REMOTESESSION_H

#include <MaterialXRemote/RemoteViewer.h>
#include <MaterialXRemote/Types.h>

#include <nanogui/common.h>

#include <condition_variable>
#include <future>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <stdexcept>
#include <utility>
#include <vector>

MATERIALX_NAMESPACE_BEGIN

class RemoteSession
{
  public:
    struct Config
    {
        RemoteViewer::Options viewerOptions;
        float refreshPeriodMs = 16.0f;
    };

    RemoteSession();
    explicit RemoteSession(const Config& config);
    ~RemoteSession();

    RemoteSession(const RemoteSession&) = delete;
    RemoteSession& operator=(const RemoteSession&) = delete;

    void start();
    void stop();

    bool isRunning() const;

    // Session material APIs: select by existing filesystem path (catalog selection).
    // The session will only remember the file path (and name); it will not copy the file.
    SessionMaterial selectMaterialFromPath(const std::string& filePath);
    bool hasSessionMaterial() const;
    SessionMaterial currentSessionMaterial() const;
    void clearSessionMaterial();

    // Shader package helpers (stored in-session): simple vertex/fragment stage text.
    void setShaderPackage(const ShaderPackage& o);
    ShaderPackage getShaderPackage() const;
    void clearShaderPackage();


    template<typename Callable>
    auto enqueue(Callable&& callable)
        -> std::future<typename std::invoke_result_t<Callable, RemoteViewer&>>;

  private:
    enum class State
    {
        Idle,
        Starting,
        Running,
        Stopping
    };

    void renderLoop(std::shared_ptr<std::promise<void>> startupPromise);

    Config _config;
    mutable std::mutex _stateMutex;
    State _state;
    std::shared_ptr<RemoteViewer> _viewer;
    std::thread _renderThread;
    std::shared_ptr<std::promise<void>> _startupPromise;
    std::future<void> _startupFuture;
    std::exception_ptr _renderThreadException;
    SessionMaterial _sessionMaterial;
    ShaderPackage _shaderOverride;
};

inline RemoteSession::RemoteSession() : RemoteSession(Config{})
{
}

inline RemoteSession::RemoteSession(const Config& config) :
    _config(config),
    _state(State::Idle)
{
}

inline bool RemoteSession::isRunning() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _state == State::Running;
}

inline void RemoteSession::setShaderPackage(const ShaderPackage& o)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _shaderOverride = o;
}

inline ShaderPackage RemoteSession::getShaderPackage() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _shaderOverride;
}

inline void RemoteSession::clearShaderPackage()
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _shaderOverride = ShaderPackage();
}

template<typename Callable>
inline auto RemoteSession::enqueue(Callable&& callable)
    -> std::future<typename std::invoke_result_t<Callable, RemoteViewer&>>
{
    using Result = typename std::invoke_result_t<Callable, RemoteViewer&>;

    std::shared_ptr<RemoteViewer> viewer;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_state != State::Running || !_viewer)
        {
            throw std::runtime_error("RemoteSession is not running");
        }
        viewer = _viewer;
    }

    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();

    auto work = std::forward<Callable>(callable);

    nanogui::async([viewer = std::move(viewer), promise, work = std::move(work)]() mutable {
        try
        {
            if constexpr (std::is_void_v<Result>)
            {
                std::invoke(std::move(work), std::ref(*viewer));
                promise->set_value();
            }
            else
            {
                promise->set_value(std::invoke(std::move(work), std::ref(*viewer)));
            }
        }
        catch (...)
        {
            promise->set_exception(std::current_exception());
        }
    });

    return future;
}

MATERIALX_NAMESPACE_END

#endif

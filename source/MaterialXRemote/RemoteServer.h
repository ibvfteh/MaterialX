//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_REMOTESERVER_H
#define MATERIALXREMOTE_REMOTESERVER_H

#include <MaterialXRemote/RemoteSession.h>
#include <MaterialXRemote/Types.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

MATERIALX_NAMESPACE_BEGIN

class RemoteServer
{
  public:
    struct Config
    {
        std::string bindAddress = "0.0.0.0";
        int port = 2907;
        size_t maxMaterialBytes = 32u * 1024u * 1024u;
        int readTimeoutSeconds = 10;
        int writeTimeoutSeconds = 30;
        bool enableRequestLogging = false;
    std::string materialCatalogPath; // optional path to server-side materials
    };

    RemoteServer(std::shared_ptr<RemoteSession> session, Config config);
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    void start();
    void stop();

    bool isRunning() const;

    const Config& getConfig() const { return _config; }

  private:
    class Impl;

    std::shared_ptr<RemoteSession> _session;
    Config _config;
    std::unique_ptr<Impl> _impl;
};

using RemoteServerPtr = std::shared_ptr<RemoteServer>;

MATERIALX_NAMESPACE_END

#endif

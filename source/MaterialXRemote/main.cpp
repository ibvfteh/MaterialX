//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteServer.h>
#include <MaterialXRemote/RemoteSession.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace
{
std::atomic<bool> gShouldRun{true};

void handleSignal(int)
{
    gShouldRun.store(false);
}
}

int main(int argc, char** argv)
{
    using namespace MaterialX;

    try
    {
        bool headless = false;
        std::string bindAddress = "0.0.0.0";
        int port = 2907;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--headless")
            {
                headless = true;
            }
            else if ((arg == "--bind" || arg == "-b") && i + 1 < argc)
            {
                bindAddress = argv[++i];
            }
            else if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            {
                port = std::atoi(argv[++i]);
            }
            else if (arg == "--help" || arg == "-h")
            {
                std::cout << "Usage: MaterialXRemoteServer [options]\n"
                             "  --headless           Run without showing the viewer window\n"
                             "  --bind <address>     Bind address (default 0.0.0.0)\n"
                             "  --port <port>        Listen port (default 2907)\n";
                return EXIT_SUCCESS;
            }
        }

        RemoteSession::Config sessionConfig;
        sessionConfig.viewerOptions.headless = headless;

        std::cout << "Starting remote session (headless=" << std::boolalpha << headless << ")..." << std::endl;

        auto session = std::make_shared<RemoteSession>(sessionConfig);
        session->start();

        if (!session->isRunning())
        {
            std::cerr << "Failed to start remote session" << std::endl;
            return EXIT_FAILURE;
        }

        RemoteServer::Config serverConfig;
        serverConfig.bindAddress = bindAddress;
        serverConfig.port = port;
        RemoteServer server(session, serverConfig);
        server.start();

        if (!server.isRunning())
        {
            std::cerr << "Failed to start remote server" << std::endl;
            return EXIT_FAILURE;
        }

        std::signal(SIGINT, handleSignal);
#if defined(SIGTERM)
        std::signal(SIGTERM, handleSignal);
#endif

        std::cout << "Remote server listening on " << serverConfig.bindAddress << ':' << serverConfig.port << std::endl;
        std::cout << "Press Ctrl+C to exit." << std::endl;

        while (gShouldRun.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        server.stop();
        session->stop();
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

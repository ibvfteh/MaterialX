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

        std::string materialPath;
        float refreshMs = 16.0f;
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
            else if (arg == "--material" && i + 1 < argc)
            {
                materialPath = argv[++i];
            }
            else if (arg == "--refresh" && i + 1 < argc)
            {
                try { refreshMs = std::stof(argv[++i]); }
                catch (...) { /* ignore parse errors and keep default */ }
            }
            else if (arg == "--help" || arg == "-h")
            {
                std::cout << "Usage: MaterialXRemoteServer [options]\n"
                             "  --headless           Hide GLFW window (offscreen)\n"
                             "  --bind <address>     Bind address (default 0.0.0.0)\n"
                             "  --port <port>        Listen port (default 2907)\n";
                return EXIT_SUCCESS;
            }
        }

        RemoteSession::Config sessionConfig;
        sessionConfig.refreshPeriodMs = refreshMs;
        sessionConfig.viewerOptions.headless = headless;

        std::cout << "Starting remote session ("
    #if defined(MATERIALX_REMOTE_EGL_ONLY)
              << "EGLHeadless"
    #else
              << (headless ? "Headless" : "Windowed")
    #endif
              << ")..." << std::endl;

        auto session = std::make_shared<RemoteSession>(sessionConfig);
        try
        {
            session->start();
        }
        catch (const std::exception& ex)
        {
            std::cerr << "RemoteSession failed to start ("
#if defined(MATERIALX_REMOTE_EGL_ONLY)
                      << "EGLHeadless"
#else
                      << (headless ? "Headless" : "Windowed")
#endif
                      << "): " << ex.what() << std::endl;
            return EXIT_FAILURE;
        }

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

        // If a material path was provided on the command line, attempt to select
        // and compile it into the session so the server starts with that material.
        if (!materialPath.empty())
        {
            try
            {
                std::cout << "Material path requested: " << materialPath << std::endl;

                // Select material path in-session (stores path/name)
                SessionMaterial stored = session->selectMaterialFromPath(materialPath);

                // Enqueue a task on the render thread to load the document from file
                auto loadFuture = session->enqueue([stored](RemoteViewer& viewer) {
                    viewer.loadDocumentFromFile(mx::FilePath(stored.filePath));
                    return Json::Value();
                });
                loadFuture.get();

                // After loading, capture generated canonical shader stages and store them
                auto pkgFuture = session->enqueue([](RemoteViewer& viewer) {
                    ShaderPackage pkg;
                    mx::MaterialPtr material = viewer.getSelectedMaterial();
                    if (!material)
                    {
                        return pkg;
                    }
                    mx::ShaderPtr shader = material->getShader();
                    if (!shader)
                    {
                        return pkg;
                    }
                    pkg.vertex = shader->getSourceCode(mx::Stage::VERTEX);
                    pkg.fragment = shader->getSourceCode(mx::Stage::PIXEL);
                    return pkg;
                });
                ShaderPackage canonical = pkgFuture.get();
                session->setShaderPackage(canonical);

                std::cout << "Selected material: " << stored.name << " (" << stored.filePath << ")" << std::endl;
            }
            catch (const std::exception& ex)
            {
                std::cerr << "Warning: failed to load/compile material '" << materialPath << "': " << ex.what() << std::endl;
            }
        }

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

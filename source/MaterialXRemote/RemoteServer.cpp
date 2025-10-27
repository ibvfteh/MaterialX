//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteServer.h>

#include <MaterialXRemote/Types.h>
#include <MaterialXRemote/MaterialCatalog.h>
#include <fstream>
#include <MaterialXRemote/RemoteViewer.h>

#include <json/json.h>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
std::string toIso8601(const std::chrono::system_clock::time_point& timestamp)
{
    if (timestamp.time_since_epoch().count() == 0)
    {
        return std::string();
    }

    std::time_t timeValue = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &timeValue);
#else
    gmtime_r(&timeValue, &utc);
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
    {
        return std::string();
    }
    return std::string(buffer);
}

std::string trim(const std::string& value)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto begin = std::find_if_not(value.begin(), value.end(), isSpace);
    auto end = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (begin >= end)
    {
        return std::string();
    }
    return std::string(begin, end);
}
}

MATERIALX_NAMESPACE_BEGIN

class RemoteServer::Impl
{
  public:
    Impl(std::shared_ptr<RemoteSession> session, const Config& config);
    ~Impl();

    void start();
    void stop();

    bool isRunning() const { return _running.load(); }

  private:
    void configureServer();
    void registerRoutes();

    void handleHealthCheck(const httplib::Request& req, httplib::Response& res);
    // Upload handler removed — catalog selection only.

    static Json::Value buildMaterialMetadataJson(const SessionMaterial& stored);

    std::shared_ptr<RemoteSession> _session;
    Config _config;
    std::unique_ptr<MaterialCatalog> _catalog;

    std::unique_ptr<httplib::Server> _server;
    std::thread _serverThread;
    std::atomic<bool> _running;
    std::mutex _startMutex;
};

RemoteServer::Impl::Impl(std::shared_ptr<RemoteSession> session, const Config& config) :
    _session(std::move(session)),
    _config(config),
    _running(false)
{
    if (!_session)
    {
        throw std::invalid_argument("RemoteServer requires a valid RemoteSession instance");
    }
    // If no catalog path provided, default to repository `resources/Materials` folder
    if (_config.materialCatalogPath.empty())
    {
        // Default relative path inside the repo workspace
        _config.materialCatalogPath = std::string("resources/Materials");
    }
    if (!_config.materialCatalogPath.empty())
    {
        _catalog = std::make_unique<MaterialCatalog>(_config.materialCatalogPath);
        _catalog->scan();
    }
}

RemoteServer::Impl::~Impl()
{
    try
    {
        stop();
    }
    catch (...)
    {
        // Suppress exceptions during destruction.
    }
}

void RemoteServer::Impl::start()
{
    std::lock_guard<std::mutex> lock(_startMutex);
    if (_running.load())
    {
        throw std::logic_error("RemoteServer is already running");
    }

    if (!_session->isRunning())
    {
        _session->start();
    }

    _server = std::make_unique<httplib::Server>();
    configureServer();
    registerRoutes();

    if (!_server->bind_to_port(_config.bindAddress.c_str(), _config.port))
    {
        _server.reset();
        throw std::runtime_error("Failed to bind HTTP server to port");
    }

    _running.store(true);

    _serverThread = std::thread([this]() {
        try
        {
            _server->listen_after_bind();
        }
        catch (...)
        {
            // Swallow exceptions; stop() will clean up state.
        }
        _running.store(false);
    });
}

void RemoteServer::Impl::stop()
{
    std::lock_guard<std::mutex> lock(_startMutex);
    if (_server)
    {
        _server->stop();
    }

    if (_serverThread.joinable())
    {
        _serverThread.join();
    }

    _server.reset();
    _running.store(false);
}

void RemoteServer::Impl::configureServer()
{
    if (_config.readTimeoutSeconds > 0)
    {
        _server->set_read_timeout(std::chrono::seconds(_config.readTimeoutSeconds));
    }
    if (_config.writeTimeoutSeconds > 0)
    {
        _server->set_write_timeout(std::chrono::seconds(_config.writeTimeoutSeconds));
    }
    if (_config.maxMaterialBytes > 0)
    {
        _server->set_payload_max_length(_config.maxMaterialBytes);
    }
    if (!_config.enableRequestLogging)
    {
        _server->set_logger([](const httplib::Request&, const httplib::Response&) {});
    }

    _server->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr eptr) {
        try
        {
            if (eptr)
            {
                std::rethrow_exception(eptr);
            }
        }
        catch (const std::exception& ex)
        {
            res.status = 500;
            res.set_content(ex.what(), "text/plain");
        }
    });
}

void RemoteServer::Impl::registerRoutes()
{
    _server->Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        handleHealthCheck(req, res);
    });

    // Material catalog endpoints
    _server->Get("/materials", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_catalog)
        {
            res.status = 404;
            res.set_content("No material catalog configured", "text/plain");
            return;
        }
        Json::Value root(Json::arrayValue);
        for (const auto& e : _catalog->entries())
        {
            Json::Value item(Json::objectValue);
            item["name"] = e.name;
            item["filePath"] = e.filePath;
            item["verified"] = e.verified;
            root.append(item);
        }
        Json::StreamWriterBuilder builder; builder["indentation"] = "";
        res.status = 200; res.set_content(Json::writeString(builder, root), "application/json");
    });

    _server->Post("/materials/select", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_catalog)
        {
            res.status = 404; res.set_content("No material catalog configured", "text/plain"); return;
        }
        try
        {
            Json::CharReaderBuilder reader; Json::Value body; std::string errs; std::istringstream iss(req.body);
            if (!Json::parseFromStream(reader, iss, &body, &errs)) { res.status = 400; res.set_content(errs, "text/plain"); return; }
            if (!body.isMember("name")) { res.status = 400; res.set_content("Missing name", "text/plain"); return; }
            std::string name = body["name"].asString();
            if (!_catalog->hasEntry(name)) { res.status = 404; res.set_content("Material not found", "text/plain"); return; }

            const auto& entry = _catalog->getEntry(name);
            // Select material into the session without copying files.
            SessionMaterial stored = _session->selectMaterialFromPath(entry.filePath);
            // Enqueue viewer load with the original file path (viewer expects mx::FilePath)
            auto loadFuture = _session->enqueue([stored](RemoteViewer& viewer) {
                viewer.loadDocumentFromFile(mx::FilePath(stored.filePath));
                return Json::Value();
            });
            loadFuture.get();

            // After loading, capture the generated canonical shader stages on the render thread
            // and store them into the session as the canonical ShaderPackage.
            auto pkgFuture = _session->enqueue([](RemoteViewer& viewer) {
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
            _session->setShaderPackage(canonical);

            // Return a minimal selection response
            Json::Value resp(Json::objectValue);
            resp["name"] = stored.name;
            resp["filePath"] = stored.filePath;
            Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, resp), "application/json");
        }
        catch (const std::exception& ex) { res.status = 500; res.set_content(ex.what(), "text/plain"); }
    });

    _server->Get("/shader", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            // If a shader override was set in the session, merge it with any generated stages
            // so that an override for one stage doesn't blank out the other stage.
            ShaderPackage override = _session->getShaderPackage();
            if (!override.vertex.empty() || !override.fragment.empty())
            {
                // Enqueue to get generated shader stages and merge missing ones.
                auto future = _session->enqueue([&override](RemoteViewer& viewer) {
                    Json::Value out(Json::objectValue);

                    mx::MaterialPtr material = viewer.getSelectedMaterial();
                    if (!material)
                    {
                        out["error"] = "No selected material available";
                        return out;
                    }

                    mx::ShaderPtr shader = material->getShader();
                    if (!shader)
                    {
                        out["error"] = "Shader not generated for selected material";
                        return out;
                    }

                    const std::string genVertex = shader->getSourceCode(mx::Stage::VERTEX);
                    const std::string genFragment = shader->getSourceCode(mx::Stage::PIXEL);

                    Json::Value stages(Json::objectValue);
                    stages["vertex"] = override.vertex.empty() ? genVertex : override.vertex;
                    stages["fragment"] = override.fragment.empty() ? genFragment : override.fragment;
                    out["stages"] = stages;

                    return out;
                });

                Json::Value response = future.get();
                Json::StreamWriterBuilder builder; builder["indentation"] = "";
                res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
                return;
            }

            // Otherwise, synchronously enqueue a request on the render thread to obtain shader source
            auto future = _session->enqueue([&req](RemoteViewer& viewer) {
                Json::Value out(Json::objectValue);

                mx::MaterialPtr material = viewer.getSelectedMaterial();
                if (!material)
                {
                    out["error"] = "No selected material available";
                    return out;
                }

                mx::ShaderPtr shader = material->getShader();
                if (!shader)
                {
                    out["error"] = "Shader not generated for selected material";
                    return out;
                }

                const std::string vertex = shader->getSourceCode(mx::Stage::VERTEX);
                const std::string fragment = shader->getSourceCode(mx::Stage::PIXEL);

                Json::Value stages(Json::objectValue);
                stages["vertex"] = vertex;
                stages["fragment"] = fragment;
                out["stages"] = stages;

                return out;
            });

            Json::Value response = future.get();
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            const std::string payload = Json::writeString(builder, response);
            res.status = 200;
            res.set_content(payload, "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500;
            res.set_content(ex.what(), "text/plain");
        }
    });

    // Shader override endpoint: set or clear in-session shader source overrides.
    _server->Post("/shader", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            Json::CharReaderBuilder reader; Json::Value body; std::string errs; std::istringstream iss(req.body);
            if (!Json::parseFromStream(reader, iss, &body, &errs)) { res.status = 400; res.set_content(errs, "text/plain"); return; }

            if (body.isMember("clear") && body["clear"].asBool())
            {
                _session->clearShaderPackage();
                Json::Value out; out["status"] = "cleared"; Json::StreamWriterBuilder builder; builder["indentation"] = "";
                res.status = 200; res.set_content(Json::writeString(builder, out), "application/json");
                return;
            }

            // Merge updates into existing override, only updating non-empty trimmed fields.
                ShaderPackage current = _session->getShaderPackage();
            bool updated = false;

            if (body.isMember("vertex") && body["vertex"].isString())
            {
                const std::string newV = trim(body["vertex"].asString());
                if (!newV.empty())
                {
                    current.vertex = newV;
                    updated = true;
                }
            }

            if (body.isMember("fragment") && body["fragment"].isString())
            {
                const std::string newF = trim(body["fragment"].asString());
                if (!newF.empty())
                {
                    current.fragment = newF;
                    updated = true;
                }
            }

            if (!updated)
            {
                res.status = 400; res.set_content("Missing non-empty 'vertex' or 'fragment' or use 'clear' to reset", "text/plain");
                return;
            }

            // Persist package in session
            _session->setShaderPackage(current);
            // Persist only: do not compile/apply here. Clients should call /render to build and exercise the program.
            Json::Value out; Json::Value stages(Json::objectValue); stages["vertex"] = current.vertex; stages["fragment"] = current.fragment; out["stages"] = stages; out["status"] = "stored"; Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, out), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    // Render endpoint: compile current session shader package, render frames and return multipart/mixed
    _server->Post("/render", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            // Parse optional parameters: frames, width, height, warmup
            unsigned int frames = 1u;
            unsigned int width = 128u;
            unsigned int height = 128u;
            unsigned int warmup = 0u;

            if (!req.body.empty())
            {
                Json::CharReaderBuilder reader; Json::Value body; std::string errs; std::istringstream iss(req.body);
                if (!Json::parseFromStream(reader, iss, &body, &errs)) { res.status = 400; res.set_content(errs, "text/plain"); return; }
                if (body.isMember("frames")) frames = body["frames"].asUInt();
                if (body.isMember("width")) width = body["width"].asUInt();
                if (body.isMember("height")) height = body["height"].asUInt();
                if (body.isMember("warmup")) warmup = body["warmup"].asUInt();
            }

            // Capture the current shader package (may be partial); we'll merge missing stages on the render thread.
            ShaderPackage sessionPkg = _session->getShaderPackage();

            using RenderResult = std::pair<Json::Value, std::vector<std::string>>;

            auto future = _session->enqueue([sessionPkg, frames, width, height, warmup](RemoteViewer& viewer) -> RenderResult {
                return viewer.renderAndCapture(sessionPkg, frames, width, height, warmup);
            });

            RenderResult result = future.get();

            // Build multipart/mixed response body
            Json::StreamWriterBuilder jbuilder; jbuilder["indentation"] = "";
            const std::string metadataStr = Json::writeString(jbuilder, result.first);

            const std::string boundary = "MX_BOUNDARY_" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            std::string body;
            auto appendLine = [&body](const std::string& s) { body.append(s); body.append("\r\n"); };

            // JSON part
            appendLine("--" + boundary);
            appendLine("Content-Type: application/json; charset=utf-8");
            appendLine("");
            body.append(metadataStr);
            body.append("\r\n");

            // Image parts
            for (size_t i = 0; i < result.second.size(); ++i)
            {
                const std::string& img = result.second[i];
                appendLine("--" + boundary);
                appendLine("Content-Type: application/octet-stream");
                appendLine(std::string("Content-Disposition: attachment; filename=\"frame_") + std::to_string(i) + ".raw\"");
                appendLine(std::string("Content-Length: ") + std::to_string(img.size()));
                appendLine("");
                // Binary raw RGB data
                body.append(img.data(), img.size());
                body.append("\r\n");
            }

            // Final boundary
            appendLine("--" + boundary + "--");

            const std::string contentType = std::string("multipart/mixed; boundary=") + boundary;
            res.status = 200;
            res.set_content(body, contentType.c_str());
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    // Uniforms endpoints: bulk read/write of public uniforms on the selected material
    _server->Get("/uniforms", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto future = _session->enqueue([](RemoteViewer& viewer) {
                Json::Value out(Json::objectValue);
                Json::Value list(Json::arrayValue);

                mx::MaterialPtr material = viewer.getSelectedMaterial();
                if (!material)
                {
                    out["error"] = "No selected material available";
                    return out;
                }

                const mx::VariableBlock* publicUniforms = material->getPublicUniforms();
                if (!publicUniforms)
                {
                    out["uniforms"] = list;
                    return out;
                }

                for (const auto& uniform : publicUniforms->getVariableOrder())
                {
                    // Verify the uniform is actually present and editable in the compiled program
                    if (!material->findUniform(uniform->getPath()))
                    {
                        continue;
                    }
                    Json::Value item(Json::objectValue);
                    item["path"] = uniform->getPath();
                    item["name"] = uniform->getName();
                    item["variable"] = uniform->getVariable();
                    item["value"] = uniform->getValue() ? uniform->getValue()->getValueString() : Json::Value();
                    item["type"] = uniform->getType().getName();
                    if (!uniform->getUnit().empty()) item["unit"] = uniform->getUnit();
                    if (!uniform->getColorSpace().empty()) item["colorspace"] = uniform->getColorSpace();
                    list.append(item);
                }

                out["uniforms"] = list;
                return out;
            });

            Json::Value response = future.get();
            Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    _server->Post("/uniforms", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            Json::CharReaderBuilder reader; Json::Value body; std::string errs; std::istringstream iss(req.body);
            if (!Json::parseFromStream(reader, iss, &body, &errs)) { res.status = 400; res.set_content(errs, "text/plain"); return; }

            if (!body.isMember("uniforms") || !body["uniforms"].isArray())
            {
                res.status = 400; res.set_content("Request must contain a 'uniforms' array", "text/plain");
                return;
            }

            // Capture the payload locally, then apply on the render thread.
            Json::Value payload = body["uniforms"];

            auto future = _session->enqueue([payload](RemoteViewer& viewer) {
                Json::Value out(Json::objectValue);
                Json::Value results(Json::arrayValue);

                mx::MaterialPtr material = viewer.getSelectedMaterial();
                if (!material)
                {
                    out["error"] = "No selected material available";
                    return out;
                }

                for (const Json::Value& entry : payload)
                {
                    Json::Value item(Json::objectValue);
                    std::string path;
                    std::string valueStr;
                    if (entry.isMember("path")) path = entry["path"].asString();
                    else if (entry.isMember("name")) path = entry["name"].asString();
                    if (entry.isMember("value")) valueStr = entry["value"].asString();

                    item["path"] = path;

                    if (path.empty())
                    {
                        item["status"] = "error";
                        item["message"] = "Missing path";
                        results.append(item);
                        continue;
                    }

                    mx::ShaderPort* uniform = material->findUniform(path);
                    if (!uniform)
                    {
                        item["status"] = "error";
                        item["message"] = "Uniform not found or not active in program";
                        results.append(item);
                        continue;
                    }

                    try
                    {
                        // Construct a typed Value for the uniform and modify it.
                        mx::ValuePtr v = mx::Value::createValueFromStrings(valueStr, uniform->getType().getName());
                        material->modifyUniform(path, v, valueStr);
                        item["status"] = "ok";
                    }
                    catch (const std::exception& ex)
                    {
                        item["status"] = "error";
                        item["message"] = ex.what();
                    }
                    results.append(item);
                }

                out["results"] = results;
                return out;
            });

            Json::Value response = future.get(); Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    // Reset endpoint: restore session ShaderPackage from the currently selected SessionMaterial's canonical generated shader
    _server->Post("/shader/reset", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto pkgFuture = _session->enqueue([](RemoteViewer& viewer) {
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
            if (canonical.vertex.empty() && canonical.fragment.empty())
            {
                res.status = 400; res.set_content("No generated shader available to reset from", "text/plain");
                return;
            }
            _session->setShaderPackage(canonical);
            Json::Value out; out["status"] = "ok"; Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, out), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    // Session-wide reset: reload the selected session material (restores camera & uniforms)
    // and reset the session shader package to the generated canonical package.
    _server->Post("/reset", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            if (!_session->hasSessionMaterial())
            {
                res.status = 400;
                res.set_content("No session material selected", "text/plain");
                return;
            }

            SessionMaterial stored = _session->currentSessionMaterial();

            // Reload the material document on the render thread (this will restore document-level values and camera state)
            auto loadFuture = _session->enqueue([stored](RemoteViewer& viewer) {
                viewer.loadDocumentFromFile(mx::FilePath(stored.filePath));
                return Json::Value();
            });
            loadFuture.get();

            // After loading, capture the generated canonical shader stages on the render thread
            auto pkgFuture = _session->enqueue([](RemoteViewer& viewer) {
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
            _session->setShaderPackage(canonical);

            Json::Value out; out["status"] = "ok"; Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, out), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    // Geometry endpoints
    _server->Get("/geometry", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto future = _session->enqueue([](RemoteViewer& viewer) {
                Json::Value out(Json::objectValue);
                Json::Value list(Json::arrayValue);
                for (const std::string& id : viewer.listGeometry())
                {
                    list.append(id);
                }
                out["geometry"] = list;
                out["active"] = viewer.getActiveGeometryId();
                return out;
            });

            Json::Value response = future.get();
            Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200;
            res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500;
            res.set_content(ex.what(), "text/plain");
        }
    });

    _server->Post("/geometry", [this](const httplib::Request& req, httplib::Response& res) {
        // Not implemented: selection with path param; accept JSON { "id": "..." }
        try
        {
            Json::CharReaderBuilder reader;
            Json::Value body;
            std::string errs;
            std::istringstream iss(req.body);
            if (!Json::parseFromStream(reader, iss, &body, &errs))
            {
                res.status = 400;
                res.set_content(errs, "text/plain");
                return;
            }
            if (!body.isMember("id") || !body["id"].isString())
            {
                res.status = 400;
                res.set_content("Missing 'id' in body", "text/plain");
                return;
            }
            std::string id = body["id"].asString();
            auto future = _session->enqueue([id](RemoteViewer& viewer) {
                viewer.setActiveGeometryById(id);
                Json::Value out; out["status"] = "ok"; return out;
            });
            Json::Value response = future.get();
            Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    // Lights endpoints
    _server->Get("/lights", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto future = _session->enqueue([](RemoteViewer& viewer) {
                Json::Value out(Json::objectValue);
                out["envRadianceFilename"] = viewer.getEnvRadianceFilename();
                out["envLightIntensity"] = viewer.getEnvLightIntensity();
                out["lightRotation"] = viewer.getLightRotation();
                return out;
            });
            Json::Value response = future.get();
            Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    _server->Post("/lights", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            // Diagnostic log: print a line when a POST /lights request is received
            {
                auto now = std::chrono::system_clock::now();
                std::time_t t = std::chrono::system_clock::to_time_t(now);
                std::cerr << "[RemoteServer] POST /lights received at " << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ") << std::endl;
            }
            Json::CharReaderBuilder reader;
            Json::Value body; std::string errs; std::istringstream iss(req.body);
            if (!Json::parseFromStream(reader, iss, &body, &errs)) { res.status = 400; res.set_content(errs, "text/plain"); return; }
            std::string filename; float intensity = 0.0f; float rotation = 0.0f;
            if (body.isMember("envRadianceFilename")) filename = body["envRadianceFilename"].asString();
            if (body.isMember("envLightIntensity")) intensity = body["envLightIntensity"].asFloat();
            if (body.isMember("lightRotation")) rotation = body["lightRotation"].asFloat();

            auto future = _session->enqueue([filename,intensity,rotation](RemoteViewer& viewer) {
                if (!filename.empty()) viewer.setEnvRadianceFilename(mx::FilePath(filename));
                if (intensity != 0.0f) viewer.setEnvLightIntensity(intensity);
                viewer.setLightRotation(rotation);
                Json::Value out; out["status"] = "ok"; return out;
            });
            Json::Value response = future.get(); Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex)
        {
            res.status = 500; res.set_content(ex.what(), "text/plain");
        }
    });

    // Camera endpoints
    _server->Get("/camera", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto future = _session->enqueue([](RemoteViewer& viewer) {
                Json::Value out; out["position"] = Json::arrayValue; out["target"] = Json::arrayValue;
                auto pos = viewer.getCameraPosition(); auto tgt = viewer.getCameraTarget();
                out["position"].append(pos[0]); out["position"].append(pos[1]); out["position"].append(pos[2]);
                out["target"].append(tgt[0]); out["target"].append(tgt[1]); out["target"].append(tgt[2]);
                out["viewAngle"] = viewer.getCameraViewAngle(); out["zoom"] = viewer.getCameraZoom();
                return out;
            });
            Json::Value response = future.get(); Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex) { res.status = 500; res.set_content(ex.what(), "text/plain"); }
    });

    _server->Post("/camera", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            Json::CharReaderBuilder reader; Json::Value body; std::string errs; std::istringstream iss(req.body);
            if (!Json::parseFromStream(reader, iss, &body, &errs)) { res.status = 400; res.set_content(errs, "text/plain"); return; }
            mx::Vector3 pos, tgt; float viewAngle = 0.0f; float zoom = 0.0f;
            if (body.isMember("position") && body["position"].isArray()) { pos[0]=body["position"][0].asFloat(); pos[1]=body["position"][1].asFloat(); pos[2]=body["position"][2].asFloat(); }
            if (body.isMember("target") && body["target"].isArray()) { tgt[0]=body["target"][0].asFloat(); tgt[1]=body["target"][1].asFloat(); tgt[2]=body["target"][2].asFloat(); }
            if (body.isMember("viewAngle")) viewAngle = body["viewAngle"].asFloat();
            if (body.isMember("zoom")) zoom = body["zoom"].asFloat();

            auto future = _session->enqueue([pos,tgt,viewAngle,zoom](RemoteViewer& viewer) {
                viewer.setCameraPosition(pos); viewer.setCameraTarget(tgt); if (viewAngle != 0.0f) viewer.setCameraViewAngle(viewAngle); if (zoom != 0.0f) viewer.setCameraZoom(zoom);
                Json::Value out; out["status"] = "ok"; return out;
            });
            Json::Value response = future.get(); Json::StreamWriterBuilder builder; builder["indentation"] = "";
            res.status = 200; res.set_content(Json::writeString(builder, response), "application/json");
        }
        catch (const std::exception& ex) { res.status = 500; res.set_content(ex.what(), "text/plain"); }
    });
}

void RemoteServer::Impl::handleHealthCheck(const httplib::Request&, httplib::Response& res)
{
    Json::Value root(Json::objectValue);
    root["status"] = _session->isRunning() ? "ok" : "uninitialized";

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string payload = Json::writeString(builder, root);
    res.status = 200;
    res.set_content(payload, "application/json");
}

// Upload handler removed. Server is catalog-first: use GET /materials and POST /materials/select

// resolveMaterialName removed — upload support was deleted.

Json::Value RemoteServer::Impl::buildMaterialMetadataJson(const SessionMaterial& stored)
{
    Json::Value root(Json::objectValue);
    root["name"] = stored.name;
    root["filePath"] = stored.filePath;
    return root;
}

RemoteServer::RemoteServer(std::shared_ptr<RemoteSession> session, const Config& config) :
    _session(std::move(session)),
    _config(config),
    _impl(std::make_unique<Impl>(_session, _config))
{
}

RemoteServer::~RemoteServer() = default;

void RemoteServer::start()
{
    _impl->start();
}

void RemoteServer::stop()
{
    _impl->stop();
}

bool RemoteServer::isRunning() const
{
    return _impl->isRunning();
}

MATERIALX_NAMESPACE_END

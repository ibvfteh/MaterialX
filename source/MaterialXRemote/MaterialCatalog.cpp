// MaterialCatalog.cpp

#include <MaterialXRemote/MaterialCatalog.h>
#include <MaterialXRemote/Types.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

MATERIALX_NAMESPACE_BEGIN

MaterialCatalog::MaterialCatalog(const std::string& path) : _path(path)
{
}

void MaterialCatalog::scan()
{
    _entries.clear();
    _index.clear();

    fs::path p(_path);
    // If the configured path isn't an absolute path and doesn't exist from
    // the current working directory, try walking up parent directories to
    // find a matching path inside the repository workspace (common when the
    // server is started from a build folder).
    if (!p.is_absolute() && (!fs::exists(p) || !fs::is_directory(p)))
    {
        fs::path cwd = fs::current_path();
        bool found = false;
        for (int i = 0; i < 12; ++i)
        {
            fs::path candidate = cwd / _path;
            if (fs::exists(candidate) && fs::is_directory(candidate))
            {
                p = candidate;
                found = true;
                break;
            }
            if (cwd.has_parent_path()) cwd = cwd.parent_path(); else break;
        }
        if (!found)
        {
            // Give up if we couldn't locate the materials folder
            return;
        }
    }
    else if (!fs::exists(p) || !fs::is_directory(p))
    {
        return;
    }

    for (const auto& dirEntry : fs::recursive_directory_iterator(p))
    {
        if (!dirEntry.is_regular_file()) continue;
    std::string ext = dirEntry.path().extension().u8string();
    
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".mtlx") continue;
        Entry e;
        e.name = dirEntry.path().filename().u8string();
        e.filePath = dirEntry.path().u8string();
        
        e.verified = false;

        _index[e.name] = _entries.size();
        _entries.push_back(std::move(e));
    }
}

bool MaterialCatalog::hasEntry(const std::string& name) const
{
    return _index.find(name) != _index.end();
}

const MaterialCatalog::Entry& MaterialCatalog::getEntry(const std::string& name) const
{
    auto it = _index.find(name);
    if (it == _index.end())
    {
        throw std::out_of_range("MaterialCatalog entry not found: " + name);
    }
    return _entries[it->second];
}

MATERIALX_NAMESPACE_END

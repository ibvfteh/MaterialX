// MaterialCatalog.h
// Small in-repo catalog for server-hosted materials

#ifndef MATERIALXREMOTE_MATERIALCATALOG_H
#define MATERIALXREMOTE_MATERIALCATALOG_H

#include <MaterialXRemote/Types.h>

#include <string>
#include <vector>
#include <unordered_map>

MATERIALX_NAMESPACE_BEGIN

class MaterialCatalog
{
  public:
    // MaterialCatalog scans a filesystem path for .mtlx files. When used by
    // the server, if no explicit path is provided the server will default
    // to the workspace `resources/Materials` directory and scan recursively.
  struct Entry
  {
    std::string name;
    std::string filePath;
    bool verified = false;
  };

    explicit MaterialCatalog(const std::string& path);

    void scan();

    const std::vector<Entry>& entries() const { return _entries; }

    bool hasEntry(const std::string& name) const;
    const Entry& getEntry(const std::string& name) const;

  private:
    std::string _path;
    std::vector<Entry> _entries;
    std::unordered_map<std::string,size_t> _index;
};

MATERIALX_NAMESPACE_END

#endif

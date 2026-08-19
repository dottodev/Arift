#include "arift_fs.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

namespace arift {
namespace fs {

bool exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool mkdirs(const std::string& path, int mode) {
    if (path.empty()) return false;
    if (exists(path)) return isDir(path);
    std::string parent = dirName(path);
    if (!parent.empty() && parent != path && !exists(parent)) {
        mkdirs(parent, mode);
    }
    return mkdir(path.c_str(), mode) == 0;
}

bool removeFile(const std::string& path) {
    return unlink(path.c_str()) == 0;
}

bool removeDirRecursive(const std::string& path) {
    if (!exists(path)) return true;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    struct dirent* ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = join(path, name);
        if (ent->d_type == DT_DIR) {
            ok = removeDirRecursive(full) && ok;
        } else {
            ok = removeFile(full) && ok;
        }
    }
    closedir(dir);
    ok = rmdir(path.c_str()) == 0 && ok;
    return ok;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::string& path, const void* data, size_t len, bool append) {
    std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
    out.flush();
    return out.good();
}

bool writeFile(const std::string& path, const std::string& data, bool append) {
    return writeFile(path, data.data(), data.size(), append);
}

uint64_t fileSize(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

int64_t fileMtime(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<int64_t>(st.st_mtime);
}

std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> out;
    DIR* dir = opendir(path.c_str());
    if (!dir) return out;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(join(path, name));
    }
    closedir(dir);
    return out;
}

std::vector<std::string> listDirRecursive(const std::string& path, const std::string& ext) {
    std::vector<std::string> out;
    for (const auto& entry : listDir(path)) {
        if (isDir(entry)) {
            auto sub = listDirRecursive(entry, ext);
            out.insert(out.end(), sub.begin(), sub.end());
        } else if (ext.empty() || entry.size() >= ext.size() &&
                   entry.compare(entry.size() - ext.size(), ext.size(), ext) == 0) {
            out.push_back(entry);
        }
    }
    return out;
}

std::string baseName(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string dirName(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return "";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

}  // namespace fs
}  // namespace arift
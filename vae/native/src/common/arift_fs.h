#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arift {
namespace fs {

bool exists(const std::string& path);
bool isDir(const std::string& path);
bool mkdirs(const std::string& path, int mode = 0755);
bool removeFile(const std::string& path);
bool removeDirRecursive(const std::string& path);

bool readFile(const std::string& path, std::string& out);
bool readFile(const std::string& path, std::vector<uint8_t>& out);
bool writeFile(const std::string& path, const void* data, size_t len, bool append = false);
bool writeFile(const std::string& path, const std::string& data, bool append = false);

uint64_t fileSize(const std::string& path);
int64_t fileMtime(const std::string& path);

std::vector<std::string> listDir(const std::string& path);
std::vector<std::string> listDirRecursive(const std::string& path, const std::string& ext = "");

std::string baseName(const std::string& path);
std::string dirName(const std::string& path);
std::string join(const std::string& a, const std::string& b);

}  // namespace fs
}  // namespace arift
/**
 * Copyright (c) 2026 OceanBase
 *
 * OceanBase is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * OceanBase is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with OceanBase. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sql/engine/file_source/file_source_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#include <dirent.h>

namespace oceanbase {
namespace sql {
namespace file_source {

namespace {

const char *const kNullFilePriv = "NULL";

bool is_separator(char c) { return '/' == c || '\\' == c; }

#if defined(_WIN32)
/// Split the drive prefix ("C:") of a windows path from the rest of the path.
/// The drive prefix is only recognized on windows, on linux a colon is an
/// ordinary character of a file name.
bool split_drive_prefix(const std::string &path, std::string &drive, std::string &rest)
{
  drive.clear();
  rest = path;
  if (path.size() >= 2 && ':' == path[1]
      && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) {
    drive = path.substr(0, 2);
    rest = path.substr(2);
  }
  return !drive.empty();
}
#endif

/// Check whether the text contains a wildcard character.
bool has_wildcard(const std::string &text)
{
  return text.find('*') != std::string::npos || text.find('?') != std::string::npos;
}

/// Case insensitive comparison against a lower case literal.
bool path_equals_ignore_case(const std::string &text, const char *literal)
{
  const size_t len = ::strlen(literal);
  if (text.size() != len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    char a = text[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    char b = literal[i];
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

void split_path(const std::string &path, std::vector<std::string> &parts, bool &absolute)
{
  absolute = !path.empty() && is_separator(path[0]);
  parts.clear();
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && is_separator(path[i])) {
      ++i;
    }
    size_t begin = i;
    while (i < path.size() && !is_separator(path[i])) {
      ++i;
    }
    if (i > begin) {
      parts.push_back(path.substr(begin, i - begin));
    }
  }
}

bool get_cwd(std::string &cwd)
{
  char buffer[4096];
#if defined(_WIN32)
  if (NULL == _getcwd(buffer, sizeof(buffer))) {
    return false;
  }
#else
  if (NULL == getcwd(buffer, sizeof(buffer))) {
    return false;
  }
#endif
  cwd = buffer;
  return true;
}

/// Read the entries of a directory, returns false when the directory cannot
/// be opened.
bool list_directory(const std::string &path, std::vector<std::string> &entries)
{
  entries.clear();
  DIR *dir = opendir(path.c_str());
  if (NULL == dir) {
    return false;
  }
  struct dirent *entry = NULL;
  while (NULL != (entry = readdir(dir))) {
    const std::string name = entry->d_name;
    if ("." != name && ".." != name) {
      entries.push_back(name);
    }
  }
  closedir(dir);
  return true;
}

/// Recursively expand the directory components of a pattern.
std::string file_source_join_path(const std::string &dir, const std::string &name)
{
  if (dir.empty()) {
    return name;
  }
  if ('/' == dir[dir.size() - 1]) {
    return dir + name;
  }
  return dir + "/" + name;
}

void expand_directory(const std::vector<std::string> &components,
                       size_t index,
                       const std::string &current,
                       std::vector<std::string> &results)
{
  if (index >= components.size()) {
    results.push_back(current);
    return;
  }
  const std::string &component = components[index];
  if (!has_wildcard(component)) {
    const std::string next = file_source_join_path(current, component);
    expand_directory(components, index + 1, next, results);
    return;
  }
  std::vector<std::string> entries;
  if (!list_directory(current, entries)) {
    return;
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (match_glob_pattern(component, entries[i])) {
      const std::string next = file_source_join_path(current, entries[i]);
      expand_directory(components, index + 1, next, results);
    }
  }
}

}  // namespace

bool match_glob_pattern(const std::string &pattern, const std::string &text)
{
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string::npos;
  size_t match = 0;
  while (t < text.size()) {
    if (p < pattern.size() && ('?' == pattern[p] || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && '*' == pattern[p]) {
      star = p++;
      match = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++match;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && '*' == pattern[p]) {
    ++p;
  }
  return p == pattern.size();
}

bool normalize_path(const std::string &path, std::string &result)
{
  std::string drive;
  std::string body = path;
#if defined(_WIN32)
  split_drive_prefix(path, drive, body);
#endif
  std::vector<std::string> parts;
  bool absolute = false;
  split_path(body, parts, absolute);
  std::vector<std::string> stack;
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string &part = parts[i];
    if ("." == part) {
      continue;
    }
    if (".." == part) {
      if (!stack.empty() && ".." != stack.back()) {
        stack.pop_back();
        continue;
      }
      if (absolute) {
        continue;
      }
      stack.push_back(part);
      continue;
    }
    stack.push_back(part);
  }
  std::string normalized;
  if (absolute || !drive.empty()) {
    normalized = "/";
  }
  for (size_t i = 0; i < stack.size(); ++i) {
    if (i > 0) {
      normalized += "/";
    }
    normalized += stack[i];
  }
  if (!drive.empty()) {
    normalized = drive + normalized;
  }
  if (normalized.empty()) {
    normalized = ".";
  }
  result = normalized;
  return true;
}

bool absolute_path(const std::string &path, std::string &result)
{
  if (path.empty()) {
    return false;
  }
  if (is_separator(path[0])) {
    return normalize_path(path, result);
  }
#if defined(_WIN32)
  std::string drive;
  std::string body;
  if (split_drive_prefix(path, drive, body)) {
    // a windows path with a drive prefix is already absolute
    return normalize_path(path, result);
  }
#endif
  std::string cwd;
  if (!get_cwd(cwd)) {
    return false;
  }
  return normalize_path(cwd + "/" + path, result);
}

bool canonical_path(const std::string &path, std::string &result)
{
  if (path.empty() || path.find('\0') != std::string::npos) {
    return false;
  }
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (INVALID_HANDLE_VALUE == handle) { return false; }
  char buffer[32768];
  DWORD length = GetFinalPathNameByHandleA(handle, buffer, sizeof(buffer), FILE_NAME_NORMALIZED);
  CloseHandle(handle);
  if (length == 0 || length >= sizeof(buffer)) { return false; }
  result.assign(buffer, length);
  if (result.compare(0, 4, "\\\\?\\") == 0) { result.erase(0, 4); }
  std::replace(result.begin(), result.end(), '\\', '/');
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return ::tolower(c); });
#else
  char *resolved = ::realpath(path.c_str(), NULL);
  if (NULL == resolved) { return false; }
  result = resolved;
  ::free(resolved);
#endif
  return true;
}

bool check_secure_path(const std::string &secure_file_priv, const std::string &path, std::string &error)
{
  if (secure_file_priv.empty() || path_equals_ignore_case(secure_file_priv, kNullFilePriv)) {
    error = "Access denied, please set suitable variable 'secure_file_priv' first";
    return false;
  }
  std::string root;
  std::string target;
  // For patterns authorize the existing prefix only. Every actual glob match
  // MUST be checked separately; a wildcard can match an escaping symlink.
  std::string prefix = path;
  const size_t wildcard = prefix.find_first_of("*?");
  if (wildcard != std::string::npos) {
    const size_t slash = prefix.find_last_of("/\\", wildcard);
    prefix = slash == std::string::npos ? "." : prefix.substr(0, slash + 1);
  }
  if (!canonical_path(secure_file_priv, root) || !canonical_path(prefix, target)) {
    error = "Access denied, cannot resolve actual file path";
    return false;
  }
  if (root == target || root == "/"
      || (target.size() > root.size() && target.compare(0, root.size(), root) == 0
          && (root.back() == '/' || target[root.size()] == '/'))) {
    return true;
  }
  error = "Access denied for file path, please check the 'secure_file_priv' setting";
  return false;
}

bool get_open_file_signature(FILE *file, FileSignature &signature)
{
  struct stat st;
#if defined(_WIN32)
  if (NULL == file || 0 != ::fstat(_fileno(file), &st)) { return false; }
#else
  if (NULL == file || 0 != ::fstat(fileno(file), &st)) { return false; }
#endif
  if ((st.st_mode & S_IFMT) != S_IFREG) { return false; }
  signature.device = static_cast<uint64_t>(st.st_dev);
  signature.inode = static_cast<uint64_t>(st.st_ino);
  signature.size = st.st_size;
  signature.mtime_sec = st.st_mtime;
#if defined(__APPLE__)
  signature.mtime_nsec = st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
  signature.mtime_nsec = 0;
#else
  signature.mtime_nsec = st.st_mtim.tv_nsec;
#endif
  return true;
}

FileSourceError open_regular_file(const std::string &path, const std::string &secure_file_priv,
                                 FILE *&file, FileSignature &signature)
{
  file = NULL;
  std::string target;
  std::string error;
  if (!canonical_path(path, target)) {
    return FileSourceError(FileErrorCode::FILE_NOT_FOUND, "cannot resolve file '" + path + "'");
  }
  if (!secure_file_priv.empty() && !check_secure_path(secure_file_priv, target, error)) {
    return FileSourceError(FileErrorCode::PATH_NOT_ALLOWED, error);
  }
#if defined(_WIN32)
  file = std::fopen(target.c_str(), "rb");
#else
  // Walk the resolved absolute path from an anchored directory descriptor.
  // O_NONBLOCK prevents FIFO/device substitutions from blocking before fstat.
  int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  size_t begin = 1;
  while (fd >= 0 && begin < target.size()) {
    const size_t end = target.find('/', begin);
    const bool last = end == std::string::npos;
    const std::string component = target.substr(begin, last ? end : end - begin);
    const int next = ::openat(fd, component.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC
                             | (last ? O_NONBLOCK : O_DIRECTORY));
    ::close(fd);
    fd = next;
    if (last) { break; }
    begin = end + 1;
  }
  if (fd >= 0) {
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); }
    else { file = ::fdopen(fd, "rb"); if (NULL == file) { ::close(fd); } }
  }
#endif
  if (NULL == file || !get_open_file_signature(file, signature)) {
    if (NULL != file) { std::fclose(file); file = NULL; }
    return FileSourceError(FileErrorCode::IO_ERROR, "cannot open regular file '" + path + "'");
  }
  return FileSourceError();
}

bool get_file_signature(const std::string &path, FileSignature &signature)
{
  struct stat st;
  if (0 != ::stat(path.c_str(), &st)) {
    return false;
  }
  if ((st.st_mode & S_IFMT) != S_IFREG) { return false; }
  signature.device = static_cast<uint64_t>(st.st_dev);
  signature.inode = static_cast<uint64_t>(st.st_ino);
  signature.size = static_cast<int64_t>(st.st_size);
  signature.mtime_sec = static_cast<int64_t>(st.st_mtime);
#if defined(__APPLE__)
  signature.mtime_nsec = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#elif defined(_WIN32)
  signature.mtime_nsec = 0;
#else
  signature.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
  return true;
}

bool is_regular_file(const std::string &path)
{
  struct stat st;
  if (0 != ::stat(path.c_str(), &st)) {
    return false;
  }
  return (st.st_mode & S_IFMT) == S_IFREG;
}

std::string file_extension(const std::string &path)
{
  const size_t slash = path.find_last_of('/');
  const size_t begin = (std::string::npos == slash) ? 0 : slash + 1;
  const size_t pos = path.find_last_of('.');
  if (std::string::npos == pos || pos < begin || pos + 1 >= path.size()) {
    return std::string();
  }
  std::string ext = path.substr(pos + 1);
  for (size_t i = 0; i < ext.size(); ++i) {
    ext[i] = static_cast<char>(::tolower(ext[i]));
  }
  return ext;
}

FileFormat detect_format_by_extension(const std::string &path)
{
  const std::string ext = file_extension(path);
  if (ext.empty()) {
    return FileFormat::AUTO;
  }
  if ("csv" == ext) {
    return FileFormat::CSV;
  }
  if ("json" == ext) {
    return FileFormat::JSON;
  }
  if ("jsonl" == ext || "ndjson" == ext) {
    return FileFormat::JSONL;
  }
  if ("parquet" == ext) {
    return FileFormat::PARQUET;
  }
  return FileFormat::AUTO;
}

bool glob_files(const std::string &pattern, std::vector<std::string> &files)
{
  files.clear();
  if (pattern.empty()) {
    return false;
  }
  std::string normalized;
  if (!normalize_path(pattern, normalized)) {
    return false;
  }
  std::string drive;
  std::string body = normalized;
#if defined(_WIN32)
  split_drive_prefix(normalized, drive, body);
#endif
  std::vector<std::string> parts;
  bool absolute = false;
  split_path(body, parts, absolute);
  if (parts.empty()) {
    return false;
  }
  const std::string file_pattern = parts.back();
  parts.pop_back();
  std::vector<std::string> dirs;
  const std::string root = drive.empty() ? (absolute ? "/" : ".") : (drive + "/");
  expand_directory(parts, 0, root, dirs);
  for (size_t i = 0; i < dirs.size(); ++i) {
    const std::string &dir = dirs[i];
    if (!has_wildcard(file_pattern)) {
      const std::string candidate = file_source_join_path(dir, file_pattern);
      if (is_regular_file(candidate)) {
        files.push_back(candidate);
      }
      continue;
    }
    std::vector<std::string> entries;
    if (!list_directory(dir, entries)) {
      continue;
    }
    for (size_t j = 0; j < entries.size(); ++j) {
      if (match_glob_pattern(file_pattern, entries[j])) {
        const std::string candidate = dir + "/" + entries[j];
        if (is_regular_file(candidate)) {
          files.push_back(candidate);
        }
      }
    }
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  return true;
}

}  // namespace file_source
}  // namespace sql
}  // namespace oceanbase

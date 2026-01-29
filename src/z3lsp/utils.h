#ifndef Z3LSP_UTILS_H_
#define Z3LSP_UTILS_H_

#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace z3lsp {

using json = nlohmann::json;

std::string PathToUri(const std::string& path);
std::string UriToPath(const std::string& uri);
std::string UrlDecode(std::string_view text);
std::string ToHexString(uint32_t value, int width);
std::string Trim(std::string_view text);
std::string QuoteShellArg(const std::string& value);
std::string RunCommandCapture(const std::string& command);
std::string ToLower(std::string_view text);

std::filesystem::path NormalizePath(const std::filesystem::path& path);
std::filesystem::path ResolveConfigPath(const std::string& raw,
                                        const std::filesystem::path& config_dir,
                                        const std::filesystem::path& workspace_root);

bool IsSymbolChar(char c);
std::optional<std::string> ExtractTokenAt(const std::string& text, int line, int character);
std::optional<std::string> ExtractTokenPrefix(const std::string& text, int line, int character);

bool HasPrefixIgnoreCase(std::string_view text, std::string_view prefix);
bool ContainsIgnoreCase(std::string_view text, std::string_view query);

bool IsMainFileName(const std::filesystem::path& path);
bool IsPathUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root);

std::optional<std::filesystem::path> ResolveGitRoot(const std::filesystem::path& start_path);
std::unordered_set<std::string> LoadGitIgnoredPaths(const std::filesystem::path& git_root);

}  // namespace z3lsp

#endif  // Z3LSP_UTILS_H_

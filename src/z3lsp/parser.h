#ifndef Z3LSP_PARSER_H_
#define Z3LSP_PARSER_H_

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include "state.h"
#include "project_graph.h"
#include "z3dk_core/config.h"
#include "z3dk_core/lint.h"
#include "nlohmann/json.hpp"

namespace z3lsp {

using json = nlohmann::json;

extern std::unordered_map<std::string, CachedParse> g_parse_cache;
extern std::unordered_map<std::string, RomCacheEntry> g_rom_cache;

ParsedFile ParseFileText(const std::string& text, const std::string& uri);
std::string StripAsmComment(std::string_view line);

bool ParseIncludeDirective(const std::string& trimmed, std::string* out_path);
bool ParseIncdirDirective(const std::string& trimmed, std::string* out_path);

std::optional<std::string> ResolveIncdirPath(const std::string& raw,
                                             const std::filesystem::path& base_dir);
bool ResolveIncludePath(const std::string& raw,
                        const std::filesystem::path& base_dir,
                        const std::vector<std::string>& include_paths,
                        std::filesystem::path* out_path);

void IndexIncludeDependencies(ProjectGraph& graph,
                              const ParsedFile& parsed,
                              const std::filesystem::path& parent_path,
                              const std::vector<std::string>& include_paths);

void SeedMainCandidates(const std::filesystem::path& root,
                        std::unordered_set<std::string>* main_candidates);
bool AddMainCandidatesFromConfig(const z3dk::Config& config,
                                 const std::filesystem::path& config_dir,
                                 const std::filesystem::path& workspace_root,
                                 std::unordered_set<std::string>* out);

bool LoadRomData(const std::filesystem::path& path, std::vector<uint8_t>* out);

bool DiagnosticMatchesDocument(const z3dk::Diagnostic& diag,
                               const std::filesystem::path& doc_path,
                               const std::filesystem::path& analysis_root_dir,
                               const std::filesystem::path& workspace_root,
                               bool doc_is_root);
bool PathMatchesDocumentPath(const std::string& candidate_path,
                             const std::filesystem::path& doc_path,
                             const std::filesystem::path& analysis_root_dir,
                             const std::filesystem::path& workspace_root);

std::string ExtractMissingLabel(const std::string& message);

std::optional<WorkspaceState> BuildWorkspaceState(const json& params);
std::vector<std::string> ResolveIncludePaths(const z3dk::Config& config, const std::filesystem::path& config_dir);
bool IsGitIgnoredPath(const WorkspaceState& workspace, const std::filesystem::path& path);

bool ContainsOrgDirective(const std::string& text);
bool ParentIncludesChildAfterOrg(const std::filesystem::path& parent_path,
                                 const std::filesystem::path& child_path,
                                 const std::vector<std::string>& include_paths);

}  // namespace z3lsp

#endif  // Z3LSP_PARSER_H_

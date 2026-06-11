#include "lsp_handlers.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "z3dk_core/opcode_descriptions.h"
#include "z3dk_core/snes_knowledge_base.h"

#include "logging.h"
#include "mesen_client.h"
#include "parser.h"
#include "utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace z3lsp {

std::optional<json> HandleRename(const DocumentState& doc, WorkspaceState& workspace,
                                 std::unordered_map<std::string, DocumentState>& documents,
                                 const json& params) {
  if (!params.contains("textDocument") || !params.contains("position") || !params.contains("newName")) {
    return std::nullopt;
  }

  std::string new_name = params["newName"];
  if (new_name.empty()) return std::nullopt;

  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token_opt = z3lsp::ExtractTokenAt(doc.text, line, character);
  if (!token_opt.has_value()) {
    return std::nullopt;
  }
  std::string token = *token_opt;

  std::vector<fs::path> files_to_scan;
  if (!workspace.root.empty() && fs::exists(workspace.root) && fs::is_directory(workspace.root)) {
    for (const auto& entry : fs::recursive_directory_iterator(workspace.root)) {
      if (entry.is_regular_file()) {
        auto ext = entry.path().extension();
        if (ext == ".asm" || ext == ".s" || ext == ".inc" || ext == ".a") {
          if (z3lsp::IsGitIgnoredPath(workspace, entry.path())) {
            continue;
          }
          files_to_scan.push_back(entry.path());
        }
      }
    }
  } else {
    for (const auto& pair : documents) {
      files_to_scan.push_back(fs::path(pair.second.path));
    }
  }

  std::sort(files_to_scan.begin(), files_to_scan.end());
  files_to_scan.erase(std::unique(files_to_scan.begin(), files_to_scan.end()), files_to_scan.end());

  json changes = json::object();
  for (const auto& path : files_to_scan) {
    std::string text;
    std::string file_uri = z3lsp::PathToUri(path.string());

    if (documents.count(file_uri)) {
      text = documents.at(file_uri).text;
    } else {
      std::ifstream f(path);
      if (f) {
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        text = std::move(content);
      }
    }

    if (text.empty()) continue;

    json file_edits = json::array();
    auto refs = z3lsp::FindReferencesInText(text, token);
    for (const auto& ref : refs) {
      json edit = {
          {"range",
           {{"start", {{"line", ref.line}, {"character", ref.column}}},
            {"end", {{"line", ref.line}, {"character", ref.column + ref.length}}}}},
          {"newText", new_name}};
      file_edits.push_back(edit);
    }

    if (!file_edits.empty()) {
      changes[file_uri] = file_edits;
    }
  }

  json workspace_edit;
  workspace_edit["changes"] = changes;
  return workspace_edit;
}

std::optional<json> HandleDefinition(const DocumentState& doc, const json& params) {
  if (!params.contains("textDocument") || !params.contains("position")) {
    return std::nullopt;
  }
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token = z3lsp::ExtractTokenAt(doc.text, line, character);
  if (!token.has_value()) {
    return json(nullptr);
  }

  std::string line_text;
  size_t start = 0;
  for (int i = 0; i < line; ++i) {
    start = doc.text.find('\n', start);
    if (start == std::string::npos) break;
    start++;
  }
  if (start != std::string::npos) {
    size_t end = doc.text.find('\n', start);
    line_text = doc.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
  }

  std::string trimmed = z3lsp::Trim(z3lsp::StripAsmComment(line_text));
  std::string include_path;
  if (z3lsp::ParseIncludeDirective(trimmed, &include_path) || z3lsp::ParseIncdirDirective(trimmed, &include_path)) {
    size_t quote_start = line_text.find('"');
    size_t quote_end = line_text.find('"', quote_start + 1);
    if (quote_start != std::string::npos && quote_end != std::string::npos &&
        character >= static_cast<int>(quote_start) && character <= static_cast<int>(quote_end)) {
      fs::path base_dir = fs::path(doc.path).parent_path();
      fs::path target_path;
      std::vector<std::string> include_paths;
      include_paths.push_back(base_dir.string());

      if (z3lsp::ResolveIncludePath(include_path, base_dir, include_paths, &target_path)) {
        json location;
        location["uri"] = z3lsp::PathToUri(target_path.string());
        location["range"] = {
            {"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 0}}},
        };
        return json::array({location});
      }
    }
  }

  auto label_it = doc.label_map.find(*token);
  if (label_it == doc.label_map.end()) {
    return json(nullptr);
  }
  const z3dk::Label* label_ptr = label_it->second;

  std::unordered_map<int, std::string> file_map;
  for (const auto& file : doc.source_map.files) {
    file_map[file.id] = file.path;
  }

  for (const auto& entry : doc.source_map.entries) {
    if (entry.address != label_ptr->address) {
      continue;
    }
    auto file_it = file_map.find(entry.file_id);
    if (file_it == file_map.end()) {
      continue;
    }
    int target_line = std::max(0, entry.line - 1);
    json location;
    location["uri"] = z3lsp::PathToUri(file_it->second);
    location["range"] = {
        {"start", {{"line", target_line}, {"character", 0}}},
        {"end", {{"line", target_line}, {"character", 0}}},
    };
    return json::array({location});
  }

  return json(nullptr);
}

std::optional<json> HandleHover(const DocumentState& doc, const WorkspaceState& workspace, const json& params) {
  if (!params.contains("textDocument") || !params.contains("position")) {
    return std::nullopt;
  }
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token = z3lsp::ExtractTokenAt(doc.text, line, character);
  if (!token.has_value()) {
    return json(nullptr);
  }

  auto reg_info_name = z3dk::SnesKnowledgeBase::GetRegisterInfo(*token);
  if (reg_info_name.has_value()) {
    std::ostringstream hover_text;
    hover_text << "**" << reg_info_name->name << "** ($" << std::hex << std::uppercase << reg_info_name->address << ")\n\n";
    hover_text << reg_info_name->description;
    json hover;
    hover["contents"] = {{"kind", "markdown"}, {"value", hover_text.str()}};
    return hover;
  }

  auto label_it = doc.label_map.find(*token);
  if (label_it != doc.label_map.end()) {
    const z3dk::Label* label = label_it->second;
    std::ostringstream hover_text;
    hover_text << label->name << " = $" << std::hex << std::uppercase << label->address;

    auto reg_info_addr = z3dk::SnesKnowledgeBase::GetRegisterInfo(label->address);
    if (reg_info_addr.has_value()) {
      hover_text << "\n\n**SNES Register:** " << reg_info_addr->name;
      hover_text << "\n\n" << reg_info_addr->description;
    }

    auto zelda_it = workspace.knowledge_base.find(label->address);
    if (zelda_it != workspace.knowledge_base.end()) {
      hover_text << "\n\n**Zelda Routine:** " << zelda_it->second.name;
      hover_text << "\n\n" << zelda_it->second.description;
      hover_text << "\n\n**Expects:** " << zelda_it->second.expected_state;
    }

    uint32_t addr = label->address;
    bool is_ram = (addr >= 0x7E0000 && addr <= 0x7FFFFF) || ((addr & 0xFFFF) < 0x2000);
    if (is_ram) {
      auto val = z3lsp::g_mesen.ReadByte(addr);
      if (val.has_value()) {
        hover_text << "\n\n**Live Value:** $" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                   << static_cast<int>(*val);
      }
    }

    json hover;
    hover["contents"] = {{"kind", "markdown"}, {"value", hover_text.str()}};
    return hover;
  }

  if (token->size() >= 2 && (*token)[0] == '$') {
    try {
      uint32_t addr = std::stoul(token->substr(1), nullptr, 16);
      auto reg_info_addr = z3dk::SnesKnowledgeBase::GetRegisterInfo(addr);
      if (reg_info_addr.has_value()) {
        std::ostringstream hover_text;
        hover_text << "**" << reg_info_addr->name << "** ($" << std::hex << std::uppercase << addr << ")\n\n";
        hover_text << reg_info_addr->description;
        json hover;
        hover["contents"] = {{"kind", "markdown"}, {"value", hover_text.str()}};
        return hover;
      }

      auto zelda_it = workspace.knowledge_base.find(addr);
      if (zelda_it != workspace.knowledge_base.end()) {
        std::ostringstream hover_text;
        hover_text << "**" << zelda_it->second.name << "** - $" << std::hex << std::uppercase << addr << "\n\n";
        hover_text << zelda_it->second.description << "\n\n";
        hover_text << "**Expects:** " << zelda_it->second.expected_state;
        json hover;
        hover["contents"] = {{"kind", "markdown"}, {"value", hover_text.str()}};
        return hover;
      }
    } catch (const std::exception& e) {
      z3lsp::Log("LSP JSON error: " + std::string(e.what()));
    } catch (...) {
      z3lsp::Log("LSP JSON error: unknown exception");
    }
  }

  std::string upper_token = *token;
  for (char& c : upper_token) c = std::toupper(c);

  auto op_info = z3dk::SnesKnowledgeBase::GetOpcodeInfo(upper_token);
  if (op_info.has_value()) {
    std::ostringstream hover_text;
    hover_text << "**" << op_info->mnemonic << "** - " << op_info->full_name << "\n\n";
    hover_text << op_info->description << "\n\n";
    hover_text << "**Flags:** " << op_info->flags << "\n\n";
    if (op_info->timing_table && *op_info->timing_table) {
      hover_text << "**Addressing Modes & Cycles:**\n```text\n" << op_info->timing_table << "\n```";
    }
    json hover;
    hover["contents"] = {{"kind", "markdown"}, {"value", hover_text.str()}};
    return hover;
  }

  const auto& opcode_descs = z3dk::GetOpcodeDescriptions();
  auto opcode_it = opcode_descs.find(upper_token);
  if (opcode_it != opcode_descs.end()) {
    const auto& desc = opcode_it->second;
    std::ostringstream hover_text;
    hover_text << "**" << upper_token << "** - " << desc.full_name << "\n\n";
    hover_text << desc.description << "\n\n";
    hover_text << "**Flags:** " << desc.flags_affected << "\n\n";
    if (std::string(desc.cycles) != "None") {
      hover_text << "**Cycles:** " << desc.cycles;
    }

    json hover;
    hover["contents"] = {{"kind", "markdown"}, {"value", hover_text.str()}};
    return hover;
  }

  auto define_it = doc.define_map.find(*token);
  if (define_it != doc.define_map.end()) {
    const z3dk::Define* def = define_it->second;
    std::ostringstream hover_text;
    hover_text << "!" << def->name;
    if (!def->value.empty()) {
      hover_text << " = " << def->value;
    }
    json hover;
    hover["contents"] = {{"kind", "plaintext"}, {"value", hover_text.str()}};
    return hover;
  }

  return json(nullptr);
}

}  // namespace z3lsp

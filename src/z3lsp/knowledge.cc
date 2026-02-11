#include "knowledge.h"
#include "state.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <string>
#include "nlohmann/json.hpp"
#include "logging.h"
#include "utils.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace z3lsp {

const std::unordered_map<uint32_t, KnowledgeEntry> kVanillaZeldaKnowledge = {
    {0x008000, {"Reset", "ROM entry point. Initializes the CPU and starts the game engine.", "M=8, X=8"}},
    {0x0080C9, {"NMI_Handler", "V-Blank interrupt handler. Performs DMA transfers and updates PPU registers.", "M=8, X=8"}},
    {0x02C0C3, {"Overworld_SetCameraBounds", "Calculates the scroll boundaries for the current overworld screen based on Link's position.", "M=8, X=8"}},
    {0x099A50, {"Ancilla_AddDamageNumber", "Spawns a damage number ancilla at the specified coordinates.", "M=8, X=8"}},
    {0x0080B5, {"Music_PlayTrack", "Sets the current music track to be played by the APU.", "M=8, X=8"}},
    {0x0791B3, {"Link_ReceiveItem", "Triggers the item receiving sequence for Link, including animations and inventory updates.", "M=8, X=8"}},
    {0x028364, {"BedCutscene_ColorFix", "Initializes palette and screen state for the intro bed cutscene.", "M=8, X=8"}},
    {0x008891, {"APU_SyncWait", "Handshake routine for APU communication. Common point for soft-locks if APU hangs.", "M=8, X=8"}},
    {0x7E0020, {"LinkX", "Link's current X-coordinate in the room/overworld.", "RAM"}},
    {0x7E0022, {"LinkY", "Link's current Y-coordinate in the room/overworld.", "RAM"}},
    {0x7E036C, {"LinkHealth", "Current heart count (in halves).", "RAM"}},
    {0x7E00A0, {"RoomIndex", "The ID of the current dungeon room.", "RAM"}},
};

void LoadKnowledgeBase(WorkspaceState& workspace) {
  workspace.knowledge_base = kVanillaZeldaKnowledge;

  // Attempt to find z3dk.knowledge.json
  fs::path root = workspace.root.empty() ? fs::current_path() : workspace.root;
  
  // Try directly in root
  fs::path knowledge_path = root / "z3dk.knowledge.json";
  
  // Try near config file if it exists
  if (workspace.config_path) {
      fs::path config_parent = workspace.config_path->parent_path();
      fs::path config_knowledge = config_parent / "z3dk.knowledge.json";
      if (fs::exists(config_knowledge)) {
          knowledge_path = config_knowledge;
      }
  }

  if (!fs::exists(knowledge_path)) {
      Log("No external knowledge base found at " + knowledge_path.string());
      return;
  }

  try {
      std::ifstream f(knowledge_path);
      json j = json::parse(f);
      
      if (j.contains("routines") && j["routines"].is_array()) {
          for (const auto& entry : j["routines"]) {
              uint32_t addr = 0;
              std::string addr_str = entry.value("address", "");
              
              if (addr_str.empty()) continue;
              
              // Parse hex address
              try {
                  if (addr_str.size() > 2 && addr_str.substr(0, 2) == "0x") {
                      addr = std::stoul(addr_str.substr(2), nullptr, 16);
                  } else if (addr_str.size() > 1 && addr_str[0] == '$') {
                      addr = std::stoul(addr_str.substr(1), nullptr, 16);
                  } else {
                      addr = std::stoul(addr_str, nullptr, 16);
                  }
              } catch(...) {
                  continue; 
              }
              
              KnowledgeEntry k;
              k.name = entry.value("name", "");
              k.description = entry.value("description", "");
              k.expected_state = entry.value("expects", "");
              
              workspace.knowledge_base[addr] = k;
          }
          Log("Loaded " + std::to_string(j["routines"].size()) + " entries from knowledge base.");
      }
  } catch (const std::exception& e) {
      Log("Failed to load knowledge base: " + std::string(e.what()));
  }
}

} // namespace z3lsp

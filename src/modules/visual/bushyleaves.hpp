#pragma once

#include "../Module.hpp"

class BushyLeavesModule final : public Module {
public:
    BushyLeavesModule();
    ~BushyLeavesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool allLeaves = true;
    bool includeTransparentLeaves = true;
    int layers = 5;
    float spread = 0.10f;
    bool topBottom = true;
    bool sides = true;

private:
    bool m_hooked = false;
    void installHooks();
    void applySettings();
};

void BushyLeavesHandleClientInstanceUpdate(void* clientInstance);

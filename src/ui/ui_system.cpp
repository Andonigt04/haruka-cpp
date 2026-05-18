#include "ui/ui_system.h"

namespace Haruka::UI {

UISystem::UISystem()  = default;
UISystem::~UISystem() = default;

UIWorldPanel* UISystem::addPanel(UIWorldPanel::Desc desc) {
    std::string id = desc.id;
    auto panel = std::make_unique<UIWorldPanel>(std::move(desc));
    UIWorldPanel* ptr = panel.get();
    m_panels[id] = std::move(panel);
    return ptr;
}

UIWorldPanel* UISystem::panel(const std::string& id) {
    auto it = m_panels.find(id);
    return it != m_panels.end() ? it->second.get() : nullptr;
}

void UISystem::removePanel(const std::string& id) {
    m_panels.erase(id);
}

std::vector<std::string> UISystem::panelIds() const {
    std::vector<std::string> ids;
    ids.reserve(m_panels.size());
    for (const auto& [id, _] : m_panels)
        ids.push_back(id);
    return ids;
}

void UISystem::updateInteraction(const glm::vec3& camPos,
                                  const glm::vec3& camFwd,
                                  bool interactPressed)
{
    std::vector<UIWorldPanel*> ptrs;
    ptrs.reserve(m_panels.size());
    for (auto& [id, p] : m_panels)
        ptrs.push_back(p.get());

    m_interaction.update(camPos, camFwd, ptrs, interactPressed);
}

void UISystem::drawAll(Shader& shader, const glm::mat4& view,
                        const glm::mat4& proj, const glm::vec3& camPos) const
{
    for (const auto& [id, panel] : m_panels)
        panel->draw(shader, view, proj, camPos);
}

} // namespace Haruka::UI

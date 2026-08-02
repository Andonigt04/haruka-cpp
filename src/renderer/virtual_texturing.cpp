#include "virtual_texturing.h"

namespace Haruka { namespace Renderer {

VirtualTexturing::VirtualTexturing() {}
VirtualTexturing::~VirtualTexturing() {}
void VirtualTexturing::init(const VTConfig&) {}
void VirtualTexturing::addVirtualTexture(const std::string&, const std::string&, glm::uvec2) {}
void VirtualTexturing::processFeedback() {}
VirtualTexturing::VTStats VirtualTexturing::getStats(const std::string&) const { return {}; }

}} // namespace Haruka::Renderer
/**
 * @file        graphics/deko3d/graphics_system.cpp
 * @brief       GraphicsSystem for the deko3d backend
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/deko3d/command_processor.h>
#include <rex/graphics/deko3d/graphics_system.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/deko3d/deko3d_provider.h>

namespace rex::graphics::deko3d {

Deko3DGraphicsSystem::Deko3DGraphicsSystem() = default;
Deko3DGraphicsSystem::~Deko3DGraphicsSystem() = default;

std::string Deko3DGraphicsSystem::name() const {
  return "deko3d";
}

void Deko3DGraphicsSystem::CreateProvider(bool with_presentation) {
  (void)with_presentation;
  provider_ = std::make_unique<ui::deko3d::Deko3DProvider>();
}

std::unique_ptr<CommandProcessor> Deko3DGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<Deko3DCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::deko3d

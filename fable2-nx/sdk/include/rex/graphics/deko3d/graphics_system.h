/**
 * @file        graphics/deko3d/graphics_system.h
 * @brief       GraphicsSystem for the deko3d backend (Nintendo Switch)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/graphics/graphics_system.h>

namespace rex::graphics::deko3d {

class Deko3DGraphicsSystem final : public GraphicsSystem {
 public:
  Deko3DGraphicsSystem();
  ~Deko3DGraphicsSystem() override;

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::deko3d

//
// Copyright (C) 2019-2022 Pablo Delgado Krämer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

#include <unordered_map>
#include <string>
#include <vector>

#include <cgpu.h>

namespace sg
{
  struct TextureResource;
}

namespace gi
{
  class Stager;

  class TexSys
  {
  public:
    TexSys(cgpu_device device, Stager& stager);

    void destroy();

  public:
    bool loadTextures(const std::vector<sg::TextureResource>& textureResources,
                      std::vector<cgpu_image>& images2d,
                      std::vector<cgpu_image>& images3d,
                      std::vector<uint16_t>& imageMappings);

    void destroyUncachedImages(const std::vector<cgpu_image>& images);

  private:
    cgpu_device m_device;
    Stager& m_stager;
    // FIXME: implement a proper CPU and GPU-aware cache with eviction strategy
    std::unordered_map<std::string, cgpu_image> m_imageCache;
  };
}
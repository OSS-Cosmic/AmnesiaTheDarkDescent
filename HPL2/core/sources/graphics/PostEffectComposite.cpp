/*
 * Copyright © 2009-2020 Frictional Games
 * Copyright 2023 Michael Pollind
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#include "graphics/PostEffectComposite.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffect.h"
#include "graphics/RIPogoBuffer.h"
#include "system/LowLevelSystem.h"

#include <algorithm>

namespace hpl {

cPostEffectComposite::cPostEffectComposite(cGraphics *apGraphics)
    : mpGraphics(apGraphics) {}

cPostEffectComposite::~cPostEffectComposite() {}

//-----------------------------------------------------------------------

void cPostEffectComposite::Render(float afFrameTime, struct RICmd *cmd,
                                  struct RI_PogoBuffer *pogo, uint32_t width,
                                  uint32_t height, uint32_t frameIndex) {
  mfCurrentFrameTime = afFrameTime;

  // Walk priority-ordered effects once to identify the last active one,
  // so RenderEffect can be told `isLastEffect` (used by effects that
  // would otherwise emit an extra barrier transition).
  iPostEffect *lastEffect = nullptr;
  for (const auto &entry : m_postEffects) {
    if (entry._effect->IsActive())
      lastEffect = entry._effect;
  }
  if (lastEffect == nullptr)
    return;

  // Each effect samples the pogo's "read" half and writes into the
  // pogo's "attach" half. RI_PogoBufferToggle flips the roles and
  // emits the COLOR_ATTACHMENT_OUTPUT ↔ FRAGMENT_SHADER barrier pair
  // before the next effect runs.
  for (const auto &entry : m_postEffects) {
    iPostEffect *effect = entry._effect;
    if (!effect->IsActive())
      continue;

    PostEffectRenderCtx ctx{};
    ctx.cmd = cmd;
    ctx.inputSrv = RI_PogoBufferShaderResource(pogo);
    ctx.outputTexture = pogo->textures[pogo->attachmentIndex].Get();
    ctx.outputView = *pogo->attachmentView[pogo->attachmentIndex];
    ctx.width = width;
    ctx.height = height;
    ctx.frameIndex = frameIndex;
    ctx.frameTime = afFrameTime;
    ctx.isLastEffect = (effect == lastEffect);

    effect->RenderEffect(ctx);

    RI_PogoBufferToggle(&Interface<cGraphics>::Get()->device, pogo, cmd);
  }
}

//-----------------------------------------------------------------------

void cPostEffectComposite::AddPostEffect(iPostEffect *apPostEffect,
                                         int alPrio) {
  if (apPostEffect == NULL)
    return;

  m_postEffects.push_back({m_postEffects.size(), alPrio, apPostEffect});

  // Highest priority first; ties keep insertion order (matches the
  // previous std::multimap<int,…,std::greater<int>> behavior).
  std::stable_sort(m_postEffects.begin(), m_postEffects.end(),
                   [](const PostEffectEntry &a, const PostEffectEntry &b) {
                     return a._prio != b._prio ? a._prio > b._prio
                                               : a._id < b._id;
                   });
}

bool cPostEffectComposite::HasActiveEffects() {
  for (const auto &entry : m_postEffects) {
    if (entry._effect->IsActive())
      return true;
  }
  return false;
}

//-----------------------------------------------------------------------

} // namespace hpl

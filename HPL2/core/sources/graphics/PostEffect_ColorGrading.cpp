/*
 * Copyright © 2011-2020 Frictional Games
 *
 * This file is part of Amnesia: A Machine For Pigs.
 *
 * Amnesia: A Machine For Pigs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: A Machine For Pigs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: A Machine For Pigs.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "graphics/PostEffect_ColorGrading.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/Texture.h"
#include "math/Math.h"
#include "resources/FileSearcher.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"

namespace hpl {

namespace {
// Mirrors ColorGradingPC in posteffect_color_grading.frag.slang.
struct ColorGradingPushConstants {
  float crossFadeAlpha;
  float lutSize;  // N; 0 = no table, pass the image through
  float padding[2];
};

const char *kBaseLUT = "colorgrading_base.png";
} // namespace

cPostEffectType_ColorGrading::cPostEffectType_ColorGrading(
    cGraphics *apGraphics, cResources *apResources)
    : iPostEffectType("ColorGrading", apGraphics, apResources) {
  LoadSlangGraphics(&mpGraphics->device, m_program, apResources,
                    "posteffect_fullscreen.vert",
                    "posteffect_color_grading.frag");
}

cPostEffectType_ColorGrading::~cPostEffectType_ColorGrading() {
  m_program.dispose(&mpGraphics->device);
}

iPostEffect *
cPostEffectType_ColorGrading::CreatePostEffect(iPostEffectParams *apParams) {
  // cGraphics::CreatePostEffect calls SetParams on the result.
  (void)apParams;
  return hplNew(cPostEffect_ColorGrading, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_ColorGrading::cPostEffect_ColorGrading(cGraphics *apGraphics,
                                                   cResources *apResources,
                                                   iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpSpecificType(static_cast<cPostEffectType_ColorGrading *>(mpType)),
      mpLUT1(nullptr), mpLUT2(nullptr) {}

cPostEffect_ColorGrading::~cPostEffect_ColorGrading() { DestroyLUTs(); }

void cPostEffect_ColorGrading::DestroyLUTs() {
  for (auto &entry : mLUTMap) {
    if (entry.second)
      mpResources->GetTextureManager()->Destroy(entry.second);
  }
  mLUTMap.clear();
  mpLUT1 = nullptr;
  mpLUT2 = nullptr;
}

void cPostEffect_ColorGrading::OnSetParams() {
  if (mParams.mbIsReinitialisation)
    DestroyLUTs();

  mpLUT1 = LoadLUT(mParams.msTextureFile1);
  mpLUT2 = LoadLUT(mParams.msTextureFile2);
}

void cPostEffect_ColorGrading::SetCrossFadeAlpha(float afCrossFadeAlpha) {
  mParams.mfCrossFadeAlpha = afCrossFadeAlpha;
}

Image *cPostEffect_ColorGrading::LoadLUT(tString asLUTName) {
  if (asLUTName.empty() || IsDisabled())
    return nullptr;
  asLUTName = cString::GetFileName(asLUTName);

  auto it = mLUTMap.find(asLUTName);
  if (it != mLUTMap.end())
    return it->second;

  // Maps without their own table use the neutral base table.
  if (mpResources->GetFileSearcher()->GetFilePath(asLUTName).empty()) {
    if (asLUTName == kBaseLUT) {
      Error("Color grading base table '%s' not found!\n", kBaseLUT);
      return nullptr;
    }
    return LoadLUT(kBaseLUT);
  }

  Image *pLUT = mpResources->GetTextureManager()
                    ->Create2DImage(asLUTName, false)
                    .Release();
  if (pLUT) {
    const int lSize = pLUT->GetWidth();
    if (lSize <= 1 || pLUT->GetHeight() != lSize * lSize) {
      Error("Color grading table '%s' is %dx%d, expected N x N*N!\n",
            asLUTName.c_str(), (int)pLUT->GetWidth(), (int)pLUT->GetHeight());
      mpResources->GetTextureManager()->Destroy(pLUT);
      pLUT = nullptr;
    }
  }
  mLUTMap[asLUTName] = pLUT;
  return pLUT;
}

static bool LUTUsable(Image *apLUT) {
  return apLUT && apLUT->GetTexture() && !apLUT->GetTexture()->view.isEmpty();
}

void cPostEffect_ColorGrading::RenderEffect(const PostEffectRenderCtx &ctx) {
  const float fAlpha = cMath::Clamp(mParams.mfCrossFadeAlpha, 0.0f, 1.0f);

  // As in AMFP: at either end of the fade only one table is sampled.
  Image *pFrom = mpLUT1;
  Image *pTo = mpLUT2;
  if (fAlpha == 0.0f || !LUTUsable(pTo))
    pTo = pFrom;
  if (std::abs(fAlpha - 1.0f) < kEpsilonf && LUTUsable(mpLUT2))
    pFrom = pTo = mpLUT2;
  if (!LUTUsable(pFrom))
    pFrom = pTo;
  const bool bGrade = LUTUsable(pFrom) && LUTUsable(pTo) &&
                      pFrom->GetWidth() == pTo->GetWidth();

  // Always draw: the composite toggles its ping-pong buffer after every
  // active effect, so an early return would drop the frame.
  RIRenderingAttachment color = {};
  color.view = ctx.outputView;
  color.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

  RIBeginRenderingDesc beginDesc = {};
  beginDesc.renderArea.width = ctx.width;
  beginDesc.renderArea.height = ctx.height;
  beginDesc.colorCount = 1;
  beginDesc.colors = &color;
  ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

  RIViewport viewport = {};
  viewport.y = static_cast<float>(ctx.height);
  viewport.width = static_cast<float>(ctx.width);
  viewport.height = -static_cast<float>(ctx.height);
  viewport.depthMax = 1.0f;
  ctx.cmd->setViewport(&mpGraphics->device, viewport);

  RIRect scissor = {};
  scissor.width = ctx.width;
  scissor.height = ctx.height;
  ctx.cmd->setScissor(&mpGraphics->device, scissor);

  const RIGraphicsPipelineDesc pipelineDesc =
      MakePostEffectPipelineDesc(cGraphics::PogoColorFormat, false);

  const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
  mpSpecificType->m_program.bindPipeline(&mpGraphics->device, ctx.cmd,
                                         pipelineHash,
                                         "PostEffect_ColorGrading",
                                         pipelineDesc);

  auto samplerDesc = mpGraphics->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

  RIProgram::DescriptorBinding bindings[4] = {};
  bindings[0].descriptor = *samplerDesc;
  bindings[0].handle = DescriptorBindingID::Create("inputSampler");
  bindings[1].descriptor = ctx.inputSrv;
  bindings[1].handle = DescriptorBindingID::Create("sourceInput");
  // Unused table slots still need a valid view; the source image will do.
  bindings[2].descriptor =
      bGrade ? pFrom->GetTexture()->descriptor() : ctx.inputSrv;
  bindings[2].handle = DescriptorBindingID::Create("gradingMap");
  bindings[3].descriptor =
      bGrade ? pTo->GetTexture()->descriptor() : ctx.inputSrv;
  bindings[3].handle = DescriptorBindingID::Create("gradingMap2");
  mpSpecificType->m_program.bindDescriptors(&mpGraphics->device, ctx.cmd,
                                            ctx.frameIndex, bindings, 4);

  ColorGradingPushConstants pc{};
  pc.crossFadeAlpha = (pFrom == pTo) ? 0.0f : fAlpha;
  pc.lutSize = bGrade ? static_cast<float>(pFrom->GetWidth()) : 0.0f;
  ctx.cmd->vk_d3d12_setPushConstants(
      &mpGraphics->device, mpSpecificType->m_program, 0, sizeof(pc), &pc);

  ctx.cmd->draw(&mpGraphics->device, 3, 1, 0, 0);
  ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
}

} // namespace hpl

/*
 * Copyright © 2009-2020 Frictional Games
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

#include "resources/TextureManager.h"

#include <algorithm>

#include "graphics/Bitmap.h"
#include "graphics/Graphics.h"
#include "resources/BitmapLoaderHandler.h"
#include "resources/FileSearcher.h"
#include "resources/LowLevelResources.h"
#include "resources/Resources.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"

#include "graphics/GlobalManagedSets.h"
#include "graphics/Image.h"
#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
#include "graphics/RIResourceUploader.h" // RI_ResourceBeginCopyBuffer (gAnimTex write)
#include "graphics/Texture.h"

#include "Constants.h"

namespace hpl {

// File-scope pointer to the live texture manager so ReleaseImageBindlessSlot
// (called from ~Image, which has no manager reference for standalone Images)
// can reach the heap. One manager per engine; cleared on destruction.
static cTextureManager *g_textureManager = nullptr;

cTextureManager::cTextureManager(cGraphics *apGraphics, cResources *apResources)
    : iResourceManager(apResources->GetFileSearcher(),
                       apResources->GetLowLevel(),
                       apResources->GetLowLevelSystem()),
      m_texture2DPool(kTextureSlotCapacity),
      m_textureCubePool(kTextureSlotCapacity),
      m_texture2DArrayPool(kTexture2DArrayCapacity) {
  mpGraphics = apGraphics;
  mpResources = apResources;

  mpBitmapLoaderHandler = mpResources->GetBitmapLoaderHandler();

  // Reserve slot 0 of each pool as a never-handed-out safe default for any
  // errant/invalid bindless index (PARTIALLY_BOUND leaves it unwritten).
  m_texture2DPool.requestId();
  m_textureCubePool.requestId();
  m_texture2DArrayPool.requestId();

  mvCubeSideSuffixes.push_back("_pos_x");
  mvCubeSideSuffixes.push_back("_neg_x");
  mvCubeSideSuffixes.push_back("_pos_y");
  mvCubeSideSuffixes.push_back("_neg_y");
  mvCubeSideSuffixes.push_back("_pos_z");
  mvCubeSideSuffixes.push_back("_neg_z");

  g_textureManager = this;
}

cTextureManager::~cTextureManager() {
  DestroyAll();
  Log(" Destroyed all textures\n");
  g_textureManager = nullptr;
}

void cTextureManager::FreeResource(iResourceBase *apResource) {
  // Drop the parallel Image-resource tracking entry, if any.
  auto it =
      std::find(m_imageResources.begin(), m_imageResources.end(), apResource);
  if (it != m_imageResources.end())
    m_imageResources.erase(it);

  RemoveResource(apResource);
  hplDelete(apResource);
}

SharedResourceHandle<Image> cTextureManager::_wrapperImageResource(
    const tString &name,
    std::function<Image *(const tString &name, const tWString &path,
                          cBitmap *bitmap)>
        create_image_handler) {
  tWString sPath;
  BeginLoad(name);

  Image *resource = FindImageResource(name, sPath);
  if (resource == NULL && sPath != _W("")) {
    // pTexture = FindTexture2D(asName,sPath);
    cBitmap *pBmp = mpBitmapLoaderHandler->LoadBitmap(sPath, 0);
    if (!pBmp) {

      Error("Texture manager Couldn't load bitmap '%s'\n",
            cString::To8Char(sPath).c_str());
      EndLoad();
      return {};
    }
    resource = create_image_handler(name, sPath, pBmp);

    // Bitmap is no longer needed so delete it.
    hplDelete(pBmp);

    AddResource(resource);
    if (resource)
      m_imageResources.push_back(resource);
    if (resource)
      mpGraphics->graphicsDefer.push(PinResource(resource));
  }

  if (resource) {
    resource->AddReference();
  }
  EndLoad();
  return SharedResourceHandle<Image>(
      this, resource); // adopt the reference taken above
}

Image *cTextureManager::FindImageResource(const tString &asName,
                                          tWString &asFilePath) {
  Image *pTexture = NULL;

  if (cString::GetFileExt(asName) == "") {
    int lMaxCount = -1;

    ///////////////////////
    // Iterate the different formats
    tStringVec *apFileFormatsVec = mpBitmapLoaderHandler->GetSupportedTypes();
    for (tStringVecIt it = apFileFormatsVec->begin();
         it != apFileFormatsVec->end(); ++it) {
      tWString sTempPath = _W("");
      int lCount = 0;

      tString sNewName = cString::SetFileExt(asName, *it);
      auto resource = static_cast<Image *>(
          FindLoadedResource(sNewName, sTempPath, &lCount));

      ///////////////////////
      // Check if the image exists and then check if it has the hightest equal
      // count.
      if ((resource == nullptr && sTempPath != _W("")) || resource != nullptr) {
        if (lCount > lMaxCount) {
          lMaxCount = lCount;
          asFilePath = sTempPath;
          return resource;
        }
      }
    }
  }
  return static_cast<Image *>(FindLoadedResource(asName, asFilePath));
}

SharedResourceHandle<Image>
cTextureManager::Create1DImage(const tString &asName, bool abUseMipMaps,
                               eTextureUsage aUsage,
                               unsigned int alTextureSizeLevel, bool abSRGB) {
  return _wrapperImageResource(
      asName,
      [&abUseMipMaps, &abSRGB, this](const tString &asName,
                                     const tWString &path,
                                     cBitmap *pBmp) -> Image * {
        Image::SingleImage singleImage = {};
        hpl::cTexture::BitmapLoadOptions opts = {0};
        opts.use_mipmaps = abUseMipMaps;
        // 1D images are lookup data (light falloff/attenuation ramps, colour
        // grading LUTs), not surface appearance: a filtered coarse level is a
        // wrong answer, not a blurrier one. Authored mips in the source bitmap
        // are still honoured; only runtime generation is disabled.
        opts.generate_mipmaps = false;
        opts.sRGB = abSRGB;
        singleImage.image.emplace();
        if (!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                           RI_STAGE_FRAGMENT, *pBmp, opts)) {
          Error("Texture manager Couldn't load bitmap '%s'\n",
                cString::To8Char(path).c_str());
          return NULL;
        }
        auto resource = new Image(asName, path, std::move(singleImage));
        return resource;
      });
}

SharedResourceHandle<Image>
cTextureManager::Create2DImage(const tString &asName, bool abUseMipMaps,
                               eTextureType aType, eTextureUsage aUsage,
                               unsigned int alTextureSizeLevel, bool abSRGB) {
  auto handle = _wrapperImageResource(
      asName,
      [&abUseMipMaps, &abSRGB, this](const tString &asName,
                                     const tWString &path,
                                     cBitmap *pBmp) -> Image * {
        Image::SingleImage singleImage = {};
        cTexture::BitmapLoadOptions opts = {0};
        opts.use_mipmaps = abUseMipMaps;
        opts.sRGB = abSRGB;
        singleImage.image.emplace();
        if (!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                           RI_STAGE_FRAGMENT, *pBmp, opts)) {
          Error("Texture manager Couldn't load bitmap '%s'\n",
                cString::To8Char(path).c_str());
          return NULL;
        }
        return new Image(
            asName, path,
            std::move(singleImage)); //, &cTexture::cTexture_Delete);
      });
  AssignBindlessSlot(handle.Get(), /*cube*/ false);
  return handle;
}

SharedResourceHandle<Image>
cTextureManager::Create3DImage(const tString &asName, bool abUseMipMaps,
                               eTextureUsage aUsage,
                               unsigned int alTextureSizeLevel, bool abSRGB) {
  return _wrapperImageResource(
      asName,
      [&abUseMipMaps, &abSRGB, this](const tString &asName,
                                     const tWString &path,
                                     cBitmap *pBmp) -> Image * {
        Image::SingleImage singleImage = {};
        cTexture::BitmapLoadOptions opts = {0};
        opts.use_mipmaps = abUseMipMaps;
        opts.sRGB = abSRGB;
        singleImage.image.emplace();
        if (!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                           RI_STAGE_FRAGMENT, *pBmp, opts)) {
          Error("Texture manager Couldn't load bitmap '%s'\n",
                cString::To8Char(path).c_str());
          return NULL;
        }
        return new Image(
            asName, path,
            std::move(singleImage)); //, &cTexture::cTexture_Delete);
      });
}

SharedResourceHandle<Image> cTextureManager::CreateCubeMapImage(
    const tString &asPathName, bool abUseMipMaps, eTextureUsage aUsage,
    unsigned int alTextureSizeLevel, bool abSRGB) {

  tString sExt = cString::ToLowerCase(cString::GetFileExt(asPathName));

  // Single-file (DDS) cubemap: bitmap already contains 6 faces.
  if (sExt == "dds") {
    auto handle = _wrapperImageResource(
        asPathName,
        [&abUseMipMaps, &abSRGB](const tString &asName, const tWString &path,
                                 cBitmap *pBmp) -> Image * {
          Image::SingleImage singleImage = {};
          cTexture::BitmapLoadOptions opts = {0};
          opts.use_mipmaps = abUseMipMaps;
          opts.use_cubemap = true;
          opts.sRGB = abSRGB;
          singleImage.image.emplace();
          if (!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                             RI_STAGE_FRAGMENT, *pBmp, opts)) {
            Error("Texture manager Couldn't load cubemap '%s'\n",
                  cString::To8Char(path).c_str());
            return nullptr;
          }
          return new Image(asName, path, std::move(singleImage));
        });
    AssignBindlessSlot(handle.Get(), /*cube*/ true);
    return handle;
  }

  // Multi-file cubemap: discover 6 face files and aggregate them into a single
  // cBitmap.
  tString sName = cString::SetFileExt(asPathName, "");
  tWString sFakeFullPath = cString::To16Char(sName);
  Image *image = static_cast<Image *>(GetResource(sFakeFullPath, true));

  BeginLoad(asPathName);

  if (image == nullptr) {
    tWStringVec vPaths;
    tWString sPath = _W("");
    for (int i = 0; i < 6; ++i) {
      tStringVec *apFileFormatsVec = mpBitmapLoaderHandler->GetSupportedTypes();
      for (tStringVecIt it = apFileFormatsVec->begin();
           it != apFileFormatsVec->end(); ++it) {
        tString sNewName = sName + mvCubeSideSuffixes[i] + "." + *it;
        sPath = mpFileSearcher->GetFilePath(sNewName);
        if (sPath != _W(""))
          break;
      }

      if (sPath == _W("")) {
        tString sNewName = sName + mvCubeSideSuffixes[i];
        Error("Couldn't find %d-face '%s', for cubemap '%s' in path: '%s'\n", i,
              sNewName.c_str(), sName.c_str(), asPathName.c_str());
        EndLoad();
        return {};
      }
      vPaths.push_back(sPath);
    }

    std::vector<cBitmap *> vBitmaps;
    for (int i = 0; i < 6; ++i) {
      cBitmap *pBmp = mpBitmapLoaderHandler->LoadBitmap(vPaths[i], 0);
      if (pBmp == nullptr) {
        Error("Couldn't load bitmap '%s'!\n",
              cString::To8Char(vPaths[i]).c_str());
        for (cBitmap *pPrev : vBitmaps)
          hplDelete(pPrev);
        EndLoad();
        return {};
      }
      vBitmaps.push_back(pBmp);
    }

    // Aggregate the 6 face bitmaps into a single cBitmap with 6 images so that
    // cTexture::LoadBitmap (use_cubemap = true) sees the expected layout.
    cBitmap aggregate;
    aggregate.SetSize(vBitmaps[0]->GetSize());
    aggregate.SetPixelFormat(vBitmaps[0]->GetPixelFormat());
    aggregate.SetBytesPerPixel(vBitmaps[0]->GetBytesPerPixel());
    aggregate.SetIsCompressed(vBitmaps[0]->IsCompressed());
    aggregate.SetFileName(cString::To16Char(sName));
    const int lMips = vBitmaps[0]->GetNumOfMipMaps();
    aggregate.SetUpData(6, lMips);
    for (int face = 0; face < 6; ++face) {
      for (int mip = 0; mip < lMips; ++mip) {
        cBitmapData *src = vBitmaps[face]->GetData(0, mip);
        cBitmapData *dst = aggregate.GetData(face, mip);
        if (src && dst && src->mpData)
          dst->SetData(src->mpData, src->mlSize);
      }
    }

    Image::SingleImage singleImage = {};
    cTexture::BitmapLoadOptions opts = {0};
    opts.use_mipmaps = abUseMipMaps;
    opts.use_cubemap = true;
    opts.sRGB = abSRGB;
    singleImage.image.emplace();

    bool ok = singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                            RI_STAGE_FRAGMENT, aggregate, opts);

    for (cBitmap *pBmp : vBitmaps)
      hplDelete(pBmp);

    if (!ok) {
      Error("Couldn't create cubemap '%s'!\n", sName.c_str());
      EndLoad();
      return {};
    }

    image = new Image(sName, sFakeFullPath, std::move(singleImage));
    AddResource(image, true, true, true);
    m_imageResources.push_back(image);
    // Pin the just-uploaded Image for a frame (see _wrapperImageResource).
    mpGraphics->graphicsDefer.push(PinResource(image));
  }

  if (image)
    image->AddReference();
  EndLoad();
  AssignBindlessSlot(image, /*cube*/ true);
  return SharedResourceHandle<Image>(this,
                                     image); // adopt the reference taken above
}

SharedResourceHandle<Image>
cTextureManager::CreateAnimImage(const tString &asName, bool abUseMipMaps,
                                 eTextureType aType, eTextureUsage aUsage,
                                 unsigned int alTextureSizeLevel, bool abSRGB,
                                 eTextureAnimMode aAnimMode, float aFrameTime) {
  BeginLoad(asName);

  // First frame must contain "01" before the extension.
  int lPos = cString::GetFirstStringPos(asName, "01");
  if (lPos < 0) {
    Error("First frame of animation '%s' must contain '01'!\n", asName.c_str());
    EndLoad();
    return {};
  }

  tString sSub1 = cString::Sub(asName, 0, lPos);
  tString sSub2 = cString::Sub(asName, lPos + 2);
  tString sBaseName = sSub1 + sSub2;

  if (sSub2.size() == 0 || sSub2[0] != '.') {
    Error("First frame of animation '%s' must contain '01' before extension!\n",
          asName.c_str());
    EndLoad();
    return {};
  }

  tWString sFirstFramePath = mpFileSearcher->GetFilePath(asName);
  if (sFirstFramePath == _W("")) {
    Error("First frame of animation '%s' could not be found!\n",
          asName.c_str());
    EndLoad();
    return {};
  }
  tWString sFakeFullPath = cString::GetFilePathW(sFirstFramePath) +
                           cString::To16Char(cString::GetFileName(sBaseName));

  Image *image = static_cast<Image *>(GetResource(sFakeFullPath, true));

  if (image == nullptr) {
    tString sFileExt = cString::GetFileExt(sBaseName);
    tString sFileName =
        cString::SetFileExt(cString::GetFileName(sBaseName), "");

    tString sTest = sFileName + "01." + sFileExt;
    int lNum = 2;
    tWStringVec vPaths;

    while (true) {
      tWString sPath = mpFileSearcher->GetFilePath(sTest);
      if (sPath == _W(""))
        break;
      vPaths.push_back(sPath);
      if (lNum < 10)
        sTest = sFileName + "0" + cString::ToString(lNum) + "." + sFileExt;
      else
        sTest = sFileName + cString::ToString(lNum) + "." + sFileExt;
      ++lNum;
    }

    if (vPaths.empty()) {
      Error("No textures found for animation %s\n", sBaseName.c_str());
      EndLoad();
      return {};
    }

    std::vector<cBitmap *> vBitmaps;
    for (size_t i = 0; i < vPaths.size(); ++i) {
      cBitmap *pBmp = mpBitmapLoaderHandler->LoadBitmap(vPaths[i], 0);
      if (pBmp == nullptr) {
        Error("Couldn't load bitmap '%s'!\n",
              cString::To8Char(vPaths[i]).c_str());
        for (cBitmap *pPrev : vBitmaps)
          hplDelete(pPrev);
        EndLoad();
        return {};
      }
      vBitmaps.push_back(pBmp);
    }

    if (aType == eTextureType_CubeMap) {
      // Animated CUBE gobos are deferred: load only frame 0 as a static
      // cube (frozen — matching prior behaviour, since animation never
      // advanced). A TextureCubeArray follow-up would animate these.
      Image::SingleImage single = {};
      cTexture::BitmapLoadOptions opts = {0};
      opts.use_mipmaps = abUseMipMaps;
      opts.use_cubemap = true;
      opts.sRGB = abSRGB;
      single.image.emplace();
      bool ok = single.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                         RI_STAGE_FRAGMENT, *vBitmaps[0], opts);
      for (cBitmap *pBmp : vBitmaps)
        hplDelete(pBmp);
      if (!ok) {
        EndLoad();
        return {};
      }
      image = new Image(sBaseName, sFakeFullPath, std::move(single));
    } else {
      // Aggregate the N frame bitmaps into one cBitmap with N images, then
      // build a single Texture2DArray (N layers). The shader picks the
      // current layer from gPerFrame.afT — the descriptor is written once.
      cBitmap aggregate;
      aggregate.SetSize(vBitmaps[0]->GetSize());
      aggregate.SetPixelFormat(vBitmaps[0]->GetPixelFormat());
      aggregate.SetBytesPerPixel(vBitmaps[0]->GetBytesPerPixel());
      aggregate.SetIsCompressed(vBitmaps[0]->IsCompressed());
      aggregate.SetFileName(cString::To16Char(sBaseName));
      const int lMips = vBitmaps[0]->GetNumOfMipMaps();
      aggregate.SetUpData((int)vBitmaps.size(), lMips);
      for (size_t frame = 0; frame < vBitmaps.size(); ++frame) {
        for (int mip = 0; mip < lMips; ++mip) {
          cBitmapData *src = vBitmaps[frame]->GetData(0, mip);
          cBitmapData *dst = aggregate.GetData((int)frame, mip);
          if (src && dst && src->mpData)
            dst->SetData(src->mpData, src->mlSize);
        }
      }

      Image::AnimatedImage anim = {};
      anim.frameCount = (uint32_t)vBitmaps.size();
      anim.frameTime = aFrameTime;
      anim.animMode = aAnimMode;
      cTexture::BitmapLoadOptions opts = {0};
      opts.use_mipmaps = abUseMipMaps;
      opts.use_array = true;
      opts.sRGB = abSRGB;
      bool ok = anim.image.LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                      RI_STAGE_FRAGMENT, aggregate, opts);
      for (cBitmap *pBmp : vBitmaps)
        hplDelete(pBmp);
      if (!ok) {
        EndLoad();
        return {};
      }
      image = new Image(sBaseName, sFakeFullPath, std::move(anim));
    }
    AddResource(image, true, true, true);
    m_imageResources.push_back(image);
    // Pin the just-uploaded Image for a frame (see _wrapperImageResource).
    mpGraphics->graphicsDefer.push(PinResource(image));
  }

  if (image)
    image->AddReference();
  EndLoad();
  AssignBindlessSlot(image, /*cube*/ aType == eTextureType_CubeMap);
  return SharedResourceHandle<Image>(this,
                                     image); // adopt the reference taken above
}

//-----------------------------------------------------------------------

void cTextureManager::Unload(iResourceBase *apResource) {}
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------

void cTextureManager::Update(float afTimeStep) {
  // Animated textures are Texture2DArrays whose current layer is selected
  // in-shader from gPerFrame.afT — there is no CPU frame cursor and no
  // per-frame descriptor rewrite (which would corrupt in-flight frames).
  (void)afTimeStep;
}

//-----------------------------------------------------------------------
// Bindless texture heap
//-----------------------------------------------------------------------

void cTextureManager::AssignBindlessSlot(Image *apImage, bool abCube) {
  if (apImage == nullptr)
    return;
  // Idempotent: a cache hit / reload keeps its already-assigned slot.
  if (apImage->GetRawBindlessSlot() != kInvalidTextureIndex)
    return;

  // Animated images are one Texture2DArray (binding 2); static images go to
  // the 2D (binding 0) or cube (binding 1) pool.
  const bool isArray = apImage->isAnimated();
  IndexPool &pool = isArray ? m_texture2DArrayPool
                            : (abCube ? m_textureCubePool : m_texture2DPool);
  uint32_t id = pool.requestId();
  if (id == UINT32_MAX) {
    static bool sbWarned = false;
    if (!sbWarned) {
      Warning("cTextureManager: bindless texture pool exhausted; some textures "
              "will be missing\n");
      sbWarned = true;
    }
    return; // slot stays kInvalidTextureIndex
  }
  apImage->SetBindlessSlot(id, /*cube*/ isArray ? false : abCube,
                           /*array*/ isArray);
  WriteImageDescriptor(
      apImage); // view cookie was 0 → forces the one-time write
}

void cTextureManager::WriteImageDescriptor(Image *apImage) {
  if (apImage == nullptr)
    return;
  const uint32_t slot = apImage->GetRawBindlessSlot();
  if (slot == kInvalidTextureIndex)
    return;

  // The global managed set is initialized in cGraphics::Init before any
  // managed texture is created, so it is always present here (null-guard is
  // defensive only — e.g. teardown).
  if (mpGraphics->globalset == nullptr)
    return;
  GlobalManagedSets &g = *mpGraphics->globalset;

  cTexture *tex = apImage->GetTexture();
  if (tex == nullptr)
    return;
  // Already current for this view → nothing to do (write-once; animated
  // arrays never change view).
  if (apImage->GetBindlessViewCookie() == tex->view.cookie)
    return;

  RIBindlessDescriptorSet::WriteBinding binding = {};
  binding.binding = apImage->IsBindlessArray()
                        ? kBindingTextures2DArray
                        : (apImage->IsBindlessCube() ? kBindingTexturesCube
                                                     : kBindingTextures2D);
  binding.arrayElement = slot;
  binding.descriptor = tex->descriptor();
  g.m_bindlessSet.writeDescriptors(&mpGraphics->device, {&binding, 1});
  apImage->SetBindlessViewCookie(tex->view.cookie);

  // Animated: write the per-slot animation record so the shader can pick the
  // layer from gPerFrame.afT (gAnimTex[slot] = {frameCount, frameTime, mode}).
  WriteAnimRecord(apImage);
}

// Upload an animated Image's per-slot gAnimTex record from its CURRENT params.
// Split out of WriteImageDescriptor so the editor can re-upload after a gobo
// param edit (UpdateAnimParams); the runtime writes it once at slot assignment.
void cTextureManager::WriteAnimRecord(Image *apImage) {
  if (apImage == nullptr || !apImage->IsBindlessArray())
    return;
  const uint32_t slot = apImage->GetRawBindlessSlot();
  if (slot == kInvalidTextureIndex)
    return;
  if (mpGraphics->globalset == nullptr)
    return;
  GlobalManagedSets &g = *mpGraphics->globalset;

  AnimTexRec rec = {};
  rec.frameCount = apImage->GetAnimFrameCount();
  rec.frameTime = apImage->GetFrameTime();
  rec.mode = (apImage->GetAnimMode() == eTextureAnimMode_Oscillate)
                 ? kAnimModeOscillate
                 : kAnimModeLoop;
  RIResourceBufferTransaction trans = {};
  trans.target = g.m_animTexBuffer;
  trans.size = sizeof(AnimTexRec);
  trans.offset = (size_t)slot * sizeof(AnimTexRec);
  trans.currentState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  trans.currentStages = RI_STAGE_ALL_SHADER;
  trans.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  trans.postStages = RI_STAGE_ALL_SHADER;
  RI_ResourceBeginCopyBuffer(&mpGraphics->device, &mpGraphics->uploader, &trans);
  std::memcpy(trans.mapped.data, &rec, sizeof(rec));
  RI_ResourceEndCopyBuffer(&mpGraphics->device, &mpGraphics->uploader, &trans);
}

// Editor live-reload: update an animated Image's playback params and re-upload
// its GPU record, but only when they actually changed (no transfer otherwise).
// Used so editing a gobo's anim mode / frame time updates the preview, even when
// CreateAnimImage returned a path-cached Image with the old params. Not called
// by the runtime loaders, which bake correct params at the single load-time write.
void cTextureManager::UpdateAnimParams(Image *apImage, eTextureAnimMode aMode,
                                       float aFrameTime) {
  if (apImage == nullptr || !apImage->isAnimated())
    return;
  if (apImage->GetAnimMode() == aMode && apImage->GetFrameTime() == aFrameTime)
    return;
  apImage->SetAnimMode(aMode);
  apImage->SetFrameTime(aFrameTime);
  WriteAnimRecord(apImage);
}

void cTextureManager::ReturnBindlessSlot(uint32_t slot, bool isCube,
                                         bool isArray) {
  if (slot == kInvalidTextureIndex)
    return;
  (isArray ? m_texture2DArrayPool
           : (isCube ? m_textureCubePool : m_texture2DPool))
      .returnId(slot);
}

// Free function: reachable from ~Image (which has no manager pointer for
// standalone Images). Defers the index return past in-flight frames so a new
// texture can't grab the slot while old frames still read its descriptor.
void ReleaseImageBindlessSlot(Image *apImage) {
  if (apImage == nullptr)
    return;
  const uint32_t slot = apImage->GetRawBindlessSlot();
  if (slot == kInvalidTextureIndex)
    return;
  const bool cube = apImage->IsBindlessCube();
  const bool arr = apImage->IsBindlessArray();
  apImage->SetBindlessSlot(kInvalidTextureIndex, cube, arr);
  Interface<cGraphics>::Get()->graphicsDefer.push(std::function<void()>([slot, cube, arr]() {
    // Look up the manager at drain time; null at engine shutdown, in which
    // case leaking the index is harmless (the pool is being destroyed).
    if (cTextureManager *mgr = g_textureManager)
      mgr->ReturnBindlessSlot(slot, cube, arr);
  }));
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
} // namespace hpl

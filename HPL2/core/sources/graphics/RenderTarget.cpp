#include "absl/types/span.h"
#include "bgfx/bgfx.h"
#include "graphics/Image.h"
#include <array>
#include <graphics/RenderTarget.h>
#include <memory>
#include <bx/debug.h>

namespace hpl
{
    const RenderTarget RenderTarget::EmptyRenderTarget = RenderTarget();

    RenderTarget::RenderTarget(std::shared_ptr<Image> image)
    {
        std::array<std::shared_ptr<Image>, 1> images = { image };
        Initialize(absl::MakeSpan(images));
    }

    RenderTarget::RenderTarget(absl::Span<std::shared_ptr<Image>> images)
    {
        Initialize(images);
    }

    RenderTarget::RenderTarget()
        : m_images({})
    {
    }

    RenderTarget::RenderTarget(RenderTarget&& target)
    {
        m_images = std::move(target.m_images);
        m_buffer = target.m_buffer;
        target.m_buffer = BGFX_INVALID_HANDLE;
    }

    void RenderTarget::operator=(RenderTarget&& target)
    {
        m_images = std::move(target.m_images);
        m_buffer = target.m_buffer;
        target.m_buffer = BGFX_INVALID_HANDLE;
    }

    RenderTarget::~RenderTarget()
    {
        if (bgfx::isValid(m_buffer))
        {
            bgfx::destroy(m_buffer);
        }
    }

    void RenderTarget::Initialize(absl::Span<std::shared_ptr<Image>> images) {
        BX_ASSERT(!bgfx::isValid(m_buffer), "RenderTarget already initialized");
        
        absl::InlinedVector<bgfx::TextureHandle, 7> handles = {};
        absl::InlinedVector<std::shared_ptr<Image>, 7> updateImages = {};
        updateImages.insert(updateImages.end(), images.begin(), images.end());
        for (auto& image : updateImages)
        {
            auto& descriptor = image->GetDescriptor();
            BX_ASSERT(descriptor.m_configuration.m_rt != RTType::None, "Image is not a render target");
            handles.push_back(image->GetHandle());
        }
        m_buffer = bgfx::createFrameBuffer(handles.size(), handles.data(), false);
        m_images = std::move(updateImages);
    }

    void RenderTarget::Invalidate() {
        if(bgfx::isValid(m_buffer)) {
            bgfx::destroy(m_buffer);
        }
        m_buffer = BGFX_INVALID_HANDLE;
    }

    const bool RenderTarget::IsValid() const
    {
        return bgfx::isValid(m_buffer);
    }

    std::weak_ptr<Image> RenderTarget::GetImage(size_t index){
        return m_images[index];
    }

    absl::Span<std::shared_ptr<Image>> RenderTarget::GetImages()
    {
        return absl::MakeSpan(m_images.begin(), m_images.end());
    }
    
    const std::weak_ptr<Image> RenderTarget::GetImage(size_t index) const {
        return m_images[index];
    }

    const absl::Span<const std::shared_ptr<Image>> RenderTarget::GetImages() const 
    {
        return absl::MakeSpan(m_images.begin(), m_images.end());
    }

    const bgfx::FrameBufferHandle RenderTarget::GetHandle() const
    {
        return m_buffer;
    }
} // namespace hpl
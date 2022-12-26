#include "graphics/Enum.h"
#include <absl/container/inlined_vector.h>
#include <absl/strings/string_view.h>
#include <bgfx/defines.h>
#include <cstdint>
#include <graphics/GraphicsTypes.h>
#include <bgfx/bgfx.h>
#include <graphics/GraphicsContext.h>

#include <bx/debug.h>

namespace hpl
{

    struct PositionTexCoord0
    {
        float m_x;
        float m_y;
        float m_z;
        float m_u;
        float m_v;

        static void init()
        {
            _layout.begin()
                .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
                .end();
        }

        static bgfx::VertexLayout _layout;
    };
    bgfx::VertexLayout PositionTexCoord0::_layout;

    GraphicsContext::GraphicsContext()
        : _current(0)
    {
    }


    uint16_t GraphicsContext::ScreenWidth() const
    {
        return 0;
    }
    uint16_t GraphicsContext::ScreenHeight() const
    {
        return 0;
    }

    void GraphicsContext::UpdateScreenSize(uint16_t width, uint16_t height) 
    {
    }


    void GraphicsContext::Init()
    {

	    // bgfx::Init init;
		// init.type     = args.m_type;
		// init.vendorId = args.m_pciId;
		// init.platformData.nwh  = entry::getNativeWindowHandle(entry::kDefaultWindowHandle);
		// init.platformData.ndt  = entry::getNativeDisplayHandle();
		// init.resolution.width  = m_width;
		// init.resolution.height = m_height;
		// init.resolution.reset  = m_reset;
		// bgfx::init(init);

        _caps = bgfx::getCaps();
        PositionTexCoord0::init();
    }

    void GraphicsContext::Reset()
    {
        _current = 0;
    }

    void GraphicsContext::ClearTarget(bgfx::ViewId view, const DrawClear& request) {
        bgfx::setViewClear(view, 
            ([&]() {
                uint16_t flags = 0;
                if(request.m_clear.m_clearOp & ClearOp::Color) {
                    flags |= BGFX_CLEAR_COLOR;
                }
                if(request.m_clear.m_clearOp & ClearOp::Depth) {
                    flags |= BGFX_CLEAR_DEPTH;
                }
                if(request.m_clear.m_clearOp & ClearOp::Stencil) {
                    flags |= BGFX_CLEAR_STENCIL;
                }
                return flags;
            }()), 
            request.m_clear.m_rgba, 
            request.m_clear.m_depth, 
            request.m_clear.m_stencil);
        bgfx::setViewRect(view, request.m_x, request.m_y, request.m_width, request.m_height);
        if(request.m_target.IsValid()) {
            bgfx::setViewFrameBuffer(view, request.m_target.GetHandle());
        } else {
            bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        }
        bgfx::touch(view);
    }
        

    void GraphicsContext::Submit(bgfx::ViewId view, const DrawRequest& request) {
        
        auto& layout = request.m_layout;
        auto& program = request.m_program;

        for(auto& uniform: program.m_uniforms) {
            if(bgfx::isValid(uniform.m_uniformHandle)) {
                bgfx::setUniform(uniform.m_uniformHandle, uniform.m_data, uniform.m_num);
            }
        }

        for(auto& texture: program.m_textures) {
            if(bgfx::isValid(texture.m_textureHandle)) {
                bgfx::setTexture(texture.m_stage, texture.m_uniformHandle, texture.m_textureHandle);
            }
        }

        uint8_t streamIndex = 0;
        for(auto& vertexStream: layout.m_vertexStreams) {
            if(bgfx::isValid(vertexStream.m_handle)) {
                bgfx::setVertexBuffer(++streamIndex, vertexStream.m_handle);
            } else if(bgfx::isValid(vertexStream.m_dynamicHandle)) {
                bgfx::setVertexBuffer(++streamIndex, vertexStream.m_dynamicHandle);
            } else if(bgfx::isValid(vertexStream.m_transient.handle)) {
                bgfx::setVertexBuffer(++streamIndex, &vertexStream.m_transient);
            }
        }
        if(bgfx::isValid(layout.m_indexStream.m_dynamicHandle)) {
            bgfx::setIndexBuffer(layout.m_indexStream.m_dynamicHandle);
        } else if(bgfx::isValid(layout.m_indexStream.m_handle)) {
            bgfx::setIndexBuffer(layout.m_indexStream.m_handle);
        } else if(bgfx::isValid(layout.m_indexStream.m_transient.handle)) {
            bgfx::setIndexBuffer(&layout.m_indexStream.m_transient);
        }

        bgfx::setState( 
            (((program.m_configuration.m_write & Write::Depth) > 0)  ? BGFX_STATE_WRITE_Z : 0) |
            (((program.m_configuration.m_write & Write::R) > 0) ? BGFX_STATE_WRITE_R : 0) |
            (((program.m_configuration.m_write & Write::G) > 0) ? BGFX_STATE_WRITE_G : 0) |
            (((program.m_configuration.m_write & Write::B) > 0) ? BGFX_STATE_WRITE_B : 0) |
            (((program.m_configuration.m_write & Write::A) > 0) ? BGFX_STATE_WRITE_A : 0) |
            ([&]() -> uint64_t {
                switch(program.m_configuration.m_depthTest) {
                    case DepthTest::Always:
                        return BGFX_STATE_DEPTH_TEST_ALWAYS;
                    case DepthTest::Less:
                        return BGFX_STATE_DEPTH_TEST_LESS;
                    case DepthTest::LessEqual:
                        return BGFX_STATE_DEPTH_TEST_LEQUAL;
                    case DepthTest::Equal:
                        return BGFX_STATE_DEPTH_TEST_EQUAL;
                    case DepthTest::GreaterEqual:   
                        return BGFX_STATE_DEPTH_TEST_GEQUAL;
                    case DepthTest::Greater:
                        return BGFX_STATE_DEPTH_TEST_GREATER;
                    case DepthTest::NotEqual:
                        return BGFX_STATE_DEPTH_TEST_NOTEQUAL;
                    default:
                        break;
                }
                return 0;
            })() | ([&]() -> uint64_t {
                switch(program.m_configuration.m_cull) {
                    case Cull::Clockwise:
                        return BGFX_STATE_CULL_CW;
                    case Cull::CounterClockwise:
                        return BGFX_STATE_CULL_CCW;
                    default:
                        break;
                }
                return 0;
            })() | ([&]() -> uint64_t  {
                switch(layout.m_drawType) {
                    case eVertexBufferDrawType_Tri:
                        // this is the default 
                        break;
                    case eVertexBufferDrawType_TriStrip:
                        return BGFX_STATE_PT_TRISTRIP;
                    case eVertexBufferDrawType_TriFan:
                        // unused
                        break;
                    case eVertexBufferDrawType_Quad:
                        // unused
                        break;
                    case eVertexBufferDrawType_QuadStrip:
                        // unused
                        break;
                    case eVertexBufferDrawType_Line:
                        return BGFX_STATE_PT_LINES;
                    case eVertexBufferDrawType_LineLoop:
                        // unused
                        break;
                    case eVertexBufferDrawType_LineStrip:
                        return BGFX_STATE_PT_LINESTRIP;
                    default: 
                        break;
                }
                return 0;
            })() | 
            (program.m_configuration.m_blendAlpha ? BGFX_STATE_BLEND_ALPHA : 0) |
            (BGFX_STATE_ALPHA_REF(program.m_configuration.m_alphaReference)) | ([&] {
                auto mapToBGFXBlendOperator = [](BlendOperator op) -> uint64_t {
                    switch(op) {
                        case BlendOperator::Add:
                            return BGFX_STATE_BLEND_EQUATION_ADD;
                        case BlendOperator::Subtract:
                            return BGFX_STATE_BLEND_EQUATION_SUB;
                        case BlendOperator::ReverseSubtract:
                            return BGFX_STATE_BLEND_EQUATION_REVSUB;
                        case BlendOperator::Min:
                            return BGFX_STATE_BLEND_EQUATION_MIN;
                        case BlendOperator::Max:
                            return BGFX_STATE_BLEND_EQUATION_MAX;
                        default:
                            break;
                    }
                    return BGFX_STATE_BLEND_EQUATION_ADD;
                };

                auto mapToBGFXBlendOperand = [](BlendOperand op) -> uint64_t {
                    switch(op) {
                        case BlendOperand::Zero:
                            return BGFX_STATE_BLEND_ZERO;
                        case BlendOperand::One:
                            return BGFX_STATE_BLEND_ONE;
                        case BlendOperand::SrcColor:
                            return BGFX_STATE_BLEND_SRC_COLOR;
                        case BlendOperand::InvSrcColor:
                            return BGFX_STATE_BLEND_INV_SRC_COLOR;
                        case BlendOperand::SrcAlpha:
                            return BGFX_STATE_BLEND_SRC_ALPHA;
                        case BlendOperand::InvSrcAlpha:
                            return BGFX_STATE_BLEND_INV_SRC_ALPHA;
                        case BlendOperand::DstAlpha:
                            return BGFX_STATE_BLEND_DST_ALPHA;
                        case BlendOperand::InvDestAlpha:
                            return BGFX_STATE_BLEND_INV_DST_ALPHA;
                        case BlendOperand::DstColor:
                            return BGFX_STATE_BLEND_DST_COLOR;
                        case BlendOperand::InvDestColor:
                            return BGFX_STATE_BLEND_INV_DST_COLOR;
                        case BlendOperand::AlphaSat:
                            return BGFX_STATE_BLEND_SRC_ALPHA_SAT;
                        case BlendOperand::BlendFactor:
                            return BGFX_STATE_BLEND_FACTOR;
                        case BlendOperand::BlendInvFactor:
                            return BGFX_STATE_BLEND_INV_FACTOR;
                        default:
                        case BlendOperand::None:
                            break;
                    }
                    return 0;
                };

                BlendFunc alphaFunc = program.m_configuration.m_alphaBlendFunc;
                const auto srcOperandAlpha = mapToBGFXBlendOperand(GetBlendOperandSrc(alphaFunc));
                const auto srcDestAlpha = mapToBGFXBlendOperand(GetBlendOperandDst(alphaFunc));
                const auto alphaEquation = mapToBGFXBlendOperator(GetBlendOperator(alphaFunc));

                BlendFunc rgbFunc = program.m_configuration.m_rgbBlendFunc;
                const auto srcOperandRgb = mapToBGFXBlendOperand(GetBlendOperandSrc(rgbFunc));
                const auto srcDestRgb = mapToBGFXBlendOperand(GetBlendOperandDst(rgbFunc));
                const auto rgbEquation = mapToBGFXBlendOperator(GetBlendOperator(rgbFunc));

                return BGFX_STATE_BLEND_FUNC_SEPARATE(srcOperandRgb, srcDestAlpha, srcDestRgb, srcDestAlpha) |
                    BGFX_STATE_BLEND_EQUATION_SEPARATE(rgbEquation, alphaEquation);
            })());
        if(request.m_clear.has_value()) {
            auto& clear = request.m_clear.value();
            bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clear.m_rgba, clear.m_depth, clear.m_stencil);
        }
        bgfx::setViewRect(view, request.m_x, request.m_y, request.m_width, request.m_height);
        if(request.m_target.IsValid()) {
            bgfx::setViewFrameBuffer(view, request.m_target.GetHandle());
        } else {
            bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        }
        bgfx::submit(view, program.m_handle);
    }

    bgfx::ViewId GraphicsContext::StartPass(absl::string_view name)
    {
        bgfx::ViewId view = _current++;
        bgfx::setViewName(view, name.data());
        return view;
    }

    bool GraphicsContext::isOriginBottomLeft() const { 
        BX_ASSERT(_caps, "GraphicsContext::Init() must be called before isOriginBottomLeft()");
        return _caps->originBottomLeft; 
    }

    void GraphicsContext::CopyTextureToFrameBuffer(Image& image, RenderTarget& target) {
        
    }

    void GraphicsContext::Quad(GraphicsContext::LayoutStream& input, const cVector3f& pos, const cVector2f& size, const cVector2f& uv0, const cVector2f& uv1) {
        BX_ASSERT(_caps, "GraphicsContext::Init() must be called before ScreenSpaceQuad()");

        bgfx::TransientVertexBuffer vb;
        bgfx::allocTransientVertexBuffer(&vb, 4, PositionTexCoord0::_layout);
        PositionTexCoord0* vertex = (PositionTexCoord0*)vb.data;

        const float minx = pos.x;
        const float maxx = pos.x + size.x;
        const float miny = pos.y;
        const float maxy = pos.y + size.y;

        vertex[0].m_x = minx;
        vertex[0].m_y = miny;
        vertex[0].m_z = pos.z;
        vertex[0].m_u = uv0.x;
        vertex[0].m_v = uv0.y;

        vertex[1].m_x = maxx;
        vertex[1].m_y = miny;
        vertex[1].m_z = pos.z;
        vertex[1].m_u = uv1.x;
        vertex[1].m_v = uv0.y;

        vertex[2].m_x = maxx;
        vertex[2].m_y = maxy;
        vertex[2].m_z = pos.z;
        vertex[2].m_u = uv1.x;

        vertex[3].m_x = minx;
        vertex[3].m_y = maxy;
        vertex[3].m_z = pos.z;
        vertex[3].m_u = uv0.x;
        vertex[3].m_v = uv1.y;
        
        input.m_vertexStreams.push_back({
            .m_transient = vb,
        });
    }


    void GraphicsContext::ScreenSpaceQuad(GraphicsContext::LayoutStream& input, float textureWidth, float textureHeight, float width, float height)
    {
        BX_ASSERT(_caps, "GraphicsContext::Init() must be called before ScreenSpaceQuad()");

        bgfx::TransientVertexBuffer vb;
        bgfx::allocTransientVertexBuffer(&vb, 3, PositionTexCoord0::_layout);
        PositionTexCoord0* vertex = (PositionTexCoord0*)vb.data;

        const float minx = -width;
        const float maxx = width;
        const float miny = 0.0f;
        const float maxy = height * 2.0f;

        const bgfx::RendererType::Enum renderer = bgfx::getRendererType();
        const float texelHalf = bgfx::RendererType::Direct3D9 == renderer ? 0.5f : 0.0f;

        const float texelHalfW = texelHalf / textureWidth;
        const float texelHalfH = texelHalf / textureHeight;
        const float minu = -1.0f + texelHalfW;
        const float maxu = 1.0f + texelHalfH;

        const float zz = 0.0f;

        float minv = texelHalfH;
        float maxv = 2.0f + texelHalfH;

        if (_caps->originBottomLeft)
        {
            const float temp = minv;
            minv = maxv;
            maxv = temp;

            minv -= 1.0f;
            maxv -= 1.0f;
        }

        vertex[0].m_x = minx;
        vertex[0].m_y = miny;
        vertex[0].m_z = zz;
        vertex[0].m_u = minu;
        vertex[0].m_v = minv;

        vertex[1].m_x = maxx;
        vertex[1].m_y = miny;
        vertex[1].m_z = zz;
        vertex[1].m_u = maxu;
        vertex[1].m_v = minv;

        vertex[2].m_x = maxx;
        vertex[2].m_y = maxy;
        vertex[2].m_z = zz;
        vertex[2].m_u = maxu;
        vertex[2].m_v = maxv;

        input.m_vertexStreams.push_back({
            .m_transient = vb,
        });
    }

} // namespace hpl
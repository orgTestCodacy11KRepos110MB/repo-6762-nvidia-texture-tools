// Copyright (c) 2009-2011 Ignacio Castano <castano@gmail.com>
// Copyright (c) 2008-2009 NVIDIA Corporation -- Ignacio Castano <icastano@nvidia.com>
// 
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include "Context.h"

#include "nvtt.h"

#include "InputOptions.h"
#include "CompressionOptions.h"
#include "OutputOptions.h"
#include "TexImage.h"

#include "CompressorDX9.h"
#include "CompressorDX10.h"
#include "CompressorDX11.h"
#include "CompressorRGB.h"
#include "CompressorRGBE.h"
#include "cuda/CudaUtils.h"
#include "cuda/CudaCompressorDXT.h"

#include "nvimage/DirectDrawSurface.h"
#include "nvimage/ColorBlock.h"
#include "nvimage/BlockDXT.h"
#include "nvimage/Image.h"
#include "nvimage/FloatImage.h"
#include "nvimage/Filter.h"
#include "nvimage/Quantize.h"
#include "nvimage/NormalMap.h"
#include "nvimage/PixelFormat.h"
#include "nvimage/ColorSpace.h"

#include "nvcore/Memory.h"
#include "nvcore/Ptr.h"

using namespace nv;
using namespace nvtt;

Compressor::Compressor() : m(*new Compressor::Private())
{
    // CUDA initialization.
    m.cudaSupported = cuda::isHardwarePresent();
    m.cudaEnabled = false;
    m.cuda = NULL;

    enableCudaAcceleration(m.cudaSupported);

    m.dispatcher = &m.defaultDispatcher;
}

Compressor::~Compressor()
{
    delete &m;
}


void Compressor::enableCudaAcceleration(bool enable)
{
    if (m.cudaSupported)
    {
        m.cudaEnabled = enable;
    }

    if (m.cudaEnabled && m.cuda == NULL)
    {
        m.cuda = new CudaContext();

        if (!m.cuda->isValid())
        {
            m.cudaEnabled = false;
            m.cuda = NULL;
        }
    }
}

bool Compressor::isCudaAccelerationEnabled() const
{
    return m.cudaEnabled;
}

void Compressor::setTaskDispatcher(TaskDispatcher * disp)
{
    if (disp == NULL) {
        m.dispatcher = &m.defaultDispatcher;
    }
    else {
        m.dispatcher = disp;
    }
}


// Input Options API.
bool Compressor::process(const InputOptions & inputOptions, const CompressionOptions & compressionOptions, const OutputOptions & outputOptions) const
{
    return m.compress(inputOptions.m, compressionOptions.m, outputOptions.m);
}

int Compressor::estimateSize(const InputOptions & inputOptions, const CompressionOptions & compressionOptions) const
{
    int w = inputOptions.m.width;
    int h = inputOptions.m.height;
    int d = inputOptions.m.depth;
    getTargetExtent(w, h, d, inputOptions.m.maxExtent, inputOptions.m.roundMode, inputOptions.m.textureType);

    int mipmapCount = countMipmaps(w, h, d);
    return inputOptions.m.faceCount * estimateSize(w, h, d, mipmapCount, compressionOptions);
}


// TexImage API.
bool Compressor::outputHeader(const TexImage & tex, int mipmapCount, const CompressionOptions & compressionOptions, const OutputOptions & outputOptions) const
{
    return m.outputHeader(TextureType_2D, tex.width(), tex.height(), tex.depth(), mipmapCount, tex.isNormalMap(), compressionOptions.m, outputOptions.m);
}

bool Compressor::compress(const TexImage & tex, int face, int mipmap, const CompressionOptions & compressionOptions, const OutputOptions & outputOptions) const
{
    return m.compress(tex, face, mipmap, compressionOptions.m, outputOptions.m);
}

int Compressor::estimateSize(const TexImage & tex, int mipmapCount, const CompressionOptions & compressionOptions) const
{
    const int w = tex.width();
    const int h = tex.height();
    const int d = tex.depth();

    return estimateSize(w, h, d, mipmapCount, compressionOptions);
}


// Raw API.
bool Compressor::compress(int w, int h, int d, int face, int mipmap, const float * rgba, const CompressionOptions & compressionOptions, const OutputOptions & outputOptions) const
{
    return m.compress(AlphaMode_None, w, h, d, face, mipmap, rgba, compressionOptions.m, outputOptions.m);
}

int Compressor::estimateSize(int w, int h, int d, int mipmapCount, const CompressionOptions & compressionOptions) const
{
    const Format format = compressionOptions.m.format;

    const uint bitCount = compressionOptions.m.getBitCount();
    const uint pitchAlignment = compressionOptions.m.pitchAlignment;

    int size = 0;
    for (int m = 0; m < mipmapCount; m++)
    {
        size += computeImageSize(w, h, d, bitCount, pitchAlignment, format);

        // Compute extents of next mipmap:
        w = max(1, w / 2);
        h = max(1, h / 2);
        d = max(1, d / 2);
    }

    return size;
}





bool Compressor::Private::compress(const InputOptions::Private & inputOptions, const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions) const
{
    // Make sure enums match.
    nvStaticCheck(FloatImage::WrapMode_Clamp == (FloatImage::WrapMode)WrapMode_Clamp);
    nvStaticCheck(FloatImage::WrapMode_Mirror == (FloatImage::WrapMode)WrapMode_Mirror);
    nvStaticCheck(FloatImage::WrapMode_Repeat == (FloatImage::WrapMode)WrapMode_Repeat);

    // Get output handler.
    if (!outputOptions.hasValidOutputHandler())
    {
        outputOptions.error(Error_FileOpen);
        return false;
    }

    nvtt::TexImage img;
    img.setWrapMode(inputOptions.wrapMode);
    img.setAlphaMode(inputOptions.alphaMode);
    img.setNormalMap(inputOptions.isNormalMap);

    const int faceCount = inputOptions.faceCount;
    int w = inputOptions.width;
    int h = inputOptions.height;
    int d = inputOptions.depth;

    nv::getTargetExtent(w, h, d, inputOptions.maxExtent, inputOptions.roundMode, inputOptions.textureType);

    // If the extents have not changed, then we can use source images for all mipmaps.
    bool canUseSourceImages = (inputOptions.width == w && inputOptions.height == h && inputOptions.depth == d);

    int mipmapCount = 1;
    if (inputOptions.generateMipmaps) {
        mipmapCount = countMipmaps(w, h, d);
        if (inputOptions.maxLevel > 0) mipmapCount = min(mipmapCount, inputOptions.maxLevel);
    }

    if (!outputHeader(inputOptions.textureType, w, h, d, mipmapCount, img.isNormalMap(), compressionOptions, outputOptions))
    {
        return false;
    }


    // Output images.
    for (int f = 0; f < faceCount; f++)
    {
        img.setImage2D(inputOptions.inputFormat, inputOptions.width, inputOptions.height, inputOptions.images[f]);

        // To normal map.
        if (inputOptions.convertToNormalMap) {
            img.toGreyScale(inputOptions.heightFactors.x, inputOptions.heightFactors.y, inputOptions.heightFactors.z, inputOptions.heightFactors.w);
            img.toNormalMap(inputOptions.bumpFrequencyScale.x, inputOptions.bumpFrequencyScale.y, inputOptions.bumpFrequencyScale.z, inputOptions.bumpFrequencyScale.w);
        }

        // To linear space.
        if (!img.isNormalMap()) {
            img.toLinear(inputOptions.inputGamma);
        }

        // Resize input.
        img.resize(w, h, ResizeFilter_Box);

        nvtt::TexImage tmp = img;
        if (!img.isNormalMap()) {
            tmp.toGamma(inputOptions.outputGamma);
        }

        quantize(tmp, compressionOptions);
        compress(tmp, f, 0, compressionOptions, outputOptions);

        for (int m = 1; m < mipmapCount; m++) {
            w = max(1, w/2);
            h = max(1, h/2);
            d = max(1, d/2);

            int idx = m * faceCount + f;

            bool useSourceImages = false;
            if (canUseSourceImages) {
                useSourceImages = true;
                if (inputOptions.images[idx] == NULL) { // One face is missing in this mipmap level.
                    useSourceImages = false;
                    canUseSourceImages = false; // If one level is missing, ignore the following source images.
                }
            }

            if (useSourceImages) {
                img.setImage2D(inputOptions.inputFormat, w, h, inputOptions.images[idx]);
            }
            else {
                if (inputOptions.mipmapFilter == MipmapFilter_Kaiser) {
                    float params[2] = { inputOptions.kaiserStretch, inputOptions.kaiserAlpha };
                    img.buildNextMipmap(MipmapFilter_Kaiser, inputOptions.kaiserWidth, params);
                }
                else {
                    img.buildNextMipmap(inputOptions.mipmapFilter);
                }
            }
            nvDebugCheck(img.width() == w);
            nvDebugCheck(img.height() == h);

            if (img.isNormalMap()) {
                if (inputOptions.normalizeMipmaps) {
                    img.normalizeNormalMap();
                }
                tmp = img;
            }
            else {
                tmp = img;
                tmp.toGamma(inputOptions.outputGamma);
            }

            quantize(tmp, compressionOptions);
            compress(tmp, f, m, compressionOptions, outputOptions);
        }
    }

    return true;
}

bool Compressor::Private::compress(const TexImage & tex, int face, int mipmap, const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions) const
{
    if (!compress(tex.alphaMode(), tex.width(), tex.height(), tex.depth(), face, mipmap, tex.data(), compressionOptions, outputOptions)) {
        return false;
    }

    return true;
}

bool Compressor::Private::compress(AlphaMode alphaMode, int w, int h, int d, int face, int mipmap, const float * rgba, const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions) const
{
    int size = computeImageSize(w, h, d, compressionOptions.bitcount, compressionOptions.pitchAlignment, compressionOptions.format);
    outputOptions.beginImage(size, w, h, d, face, mipmap);

    // Decide what compressor to use.
    AutoPtr<CompressorInterface> compressor;
#if defined HAVE_CUDA
    if (cudaEnabled && w * h >= 512)
    {
        compressor = chooseGpuCompressor(compressionOptions);
    }
#endif
    if (compressor == NULL)
    {
        compressor = chooseCpuCompressor(compressionOptions);
    }

    if (compressor == NULL)
    {
        outputOptions.error(Error_UnsupportedFeature);
    }
    else
    {
        compressor->compress(alphaMode, w, h, rgba, dispatcher, compressionOptions, outputOptions);
    }

    return true;
}


void Compressor::Private::quantize(TexImage & img, const CompressionOptions::Private & compressionOptions) const
{
    if (compressionOptions.enableColorDithering) {
        if (compressionOptions.format >= Format_BC1 && compressionOptions.format <= Format_BC3) {
            img.quantize(0, 5, true);
            img.quantize(1, 6, true);
            img.quantize(2, 5, true);
        }
        else if (compressionOptions.format == Format_RGB) {
            img.quantize(0, compressionOptions.rsize, true);
            img.quantize(1, compressionOptions.gsize, true);
            img.quantize(2, compressionOptions.bsize, true);
        }
    }
    if (compressionOptions.enableAlphaDithering) {
        if (compressionOptions.format == Format_RGB) {
            img.quantize(0, compressionOptions.rsize, true);
            img.quantize(1, compressionOptions.gsize, true);
            img.quantize(2, compressionOptions.bsize, true);
        }
    }
    else if (compressionOptions.binaryAlpha) {
        img.binarize(3, float(compressionOptions.alphaThreshold)/255.0f, compressionOptions.enableAlphaDithering);
    }
}


bool Compressor::Private::outputHeader(nvtt::TextureType textureType, int w, int h, int d, int mipmapCount, bool isNormalMap, const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions) const
{
    if (w <= 0 || h <= 0 || d <= 0 || mipmapCount <= 0)
    {
        outputOptions.error(Error_InvalidInput);
        return false;
    }

    if (!outputOptions.outputHeader)
    {
        return true;
    }

    // Output DDS header.
    if (outputOptions.container == Container_DDS || outputOptions.container == Container_DDS10)
    {
        DDSHeader header;

        header.setUserVersion(outputOptions.version);

        if (textureType == TextureType_2D) {
            header.setTexture2D();
        }
        else if (textureType == TextureType_Cube) {
            header.setTextureCube();
        }
        /*else if (textureType == TextureType_3D) {
            header.setTexture3D();
            header.setDepth(d);
        }*/

        header.setWidth(w);
        header.setHeight(h);
        header.setMipmapCount(mipmapCount);

        bool supported = true;

        if (outputOptions.container == Container_DDS10)
        {
            if (compressionOptions.format == Format_RGBA)
            {
                int bitcount = compressionOptions.bitcount;
                if (bitcount == 0) {
                    bitcount = compressionOptions.rsize + compressionOptions.gsize + compressionOptions.bsize + compressionOptions.asize;
                }

                if (bitcount == 16)
                {
                    if (compressionOptions.rsize == 16)
                    {
                        header.setDX10Format(56); // R16_UNORM
                    }
                    else
                    {
                        // B5G6R5_UNORM
                        // B5G5R5A1_UNORM
                        supported = false;
                    }
                }
                else if (bitcount == 32)
                {
                    // B8G8R8A8_UNORM
                    // B8G8R8X8_UNORM
                    // R8G8B8A8_UNORM
                    // R10G10B10A2_UNORM
                    supported = false;
                }
                else {
                    supported = false;
                }
            }
            else
            {
                if (compressionOptions.format == Format_DXT1 || compressionOptions.format == Format_DXT1a || compressionOptions.format == Format_DXT1n) {
                    header.setDX10Format(70); // DXGI_FORMAT_BC1_TYPELESS
                    if (compressionOptions.format == Format_DXT1a) header.setHasAlphaFlag(true);
                    if (isNormalMap) header.setNormalFlag(true);
                }
                else if (compressionOptions.format == Format_DXT3) {
                    header.setDX10Format(73); // DXGI_FORMAT_BC2_TYPELESS
                }
                else if (compressionOptions.format == Format_DXT5) {
                    header.setDX10Format(76); // DXGI_FORMAT_BC3_TYPELESS
                }
                else if (compressionOptions.format == Format_DXT5n) {
                    header.setDX10Format(76); // DXGI_FORMAT_BC3_TYPELESS
                    if (isNormalMap) header.setNormalFlag(true);
                }
                else if (compressionOptions.format == Format_BC4) {
                    header.setDX10Format(79); // DXGI_FORMAT_BC4_TYPELESS
                }
                else if (compressionOptions.format == Format_BC5) {
                    header.setDX10Format(82); // DXGI_FORMAT_BC5_TYPELESS
                    if (isNormalMap) header.setNormalFlag(true);
                }
                else if (compressionOptions.format == Format_BC6) {
                    header.setDX10Format(94); // DXGI_FORMAT_BC6H_TYPELESS
                }
                else if (compressionOptions.format == Format_BC7) {
                    header.setDX10Format(97); // DXGI_FORMAT_BC7_TYPELESS
                    if (isNormalMap) header.setNormalFlag(true);
                }
                else {
                    supported = false;
                }
            }
        }
        else
        {
            if (compressionOptions.format == Format_RGBA)
            {
                // Get output bit count.
                header.setPitch(computePitch(w, compressionOptions.getBitCount(), compressionOptions.pitchAlignment));

                if (compressionOptions.pixelType == PixelType_Float)
                {
                    if (compressionOptions.rsize == 16 && compressionOptions.gsize == 0 && compressionOptions.bsize == 0 && compressionOptions.asize == 0)
                    {
                        header.setFormatCode(111); // D3DFMT_R16F
                    }
                    else if (compressionOptions.rsize == 16 && compressionOptions.gsize == 16 && compressionOptions.bsize == 0 && compressionOptions.asize == 0)
                    {
                        header.setFormatCode(112); // D3DFMT_G16R16F
                    }
                    else if (compressionOptions.rsize == 16 && compressionOptions.gsize == 16 && compressionOptions.bsize == 16 && compressionOptions.asize == 16)
                    {
                        header.setFormatCode(113); // D3DFMT_A16B16G16R16F
                    }
                    else if (compressionOptions.rsize == 32 && compressionOptions.gsize == 0 && compressionOptions.bsize == 0 && compressionOptions.asize == 0)
                    {
                        header.setFormatCode(114); // D3DFMT_R32F
                    }
                    else if (compressionOptions.rsize == 32 && compressionOptions.gsize == 32 && compressionOptions.bsize == 0 && compressionOptions.asize == 0)
                    {
                        header.setFormatCode(115); // D3DFMT_G32R32F
                    }
                    else if (compressionOptions.rsize == 32 && compressionOptions.gsize == 32 && compressionOptions.bsize == 32 && compressionOptions.asize == 32)
                    {
                        header.setFormatCode(116); // D3DFMT_A32B32G32R32F
                    }
                    else
                    {
                        supported = false;
                    }
                }
                else // Fixed point
                {
                    const uint bitcount = compressionOptions.getBitCount();

                    if (compressionOptions.bitcount != 0)
                    {
                        // Masks already computed.
                        header.setPixelFormat(compressionOptions.bitcount, compressionOptions.rmask, compressionOptions.gmask, compressionOptions.bmask, compressionOptions.amask);
                    }
                    else if (bitcount <= 32)
                    {
                        // Compute pixel format masks.
                        const uint ashift = 0;
                        const uint bshift = ashift + compressionOptions.asize;
                        const uint gshift = bshift + compressionOptions.bsize;
                        const uint rshift = gshift + compressionOptions.gsize;

                        const uint rmask = ((1 << compressionOptions.rsize) - 1) << rshift;
                        const uint gmask = ((1 << compressionOptions.gsize) - 1) << gshift;
                        const uint bmask = ((1 << compressionOptions.bsize) - 1) << bshift;
                        const uint amask = ((1 << compressionOptions.asize) - 1) << ashift;

                        header.setPixelFormat(bitcount, rmask, gmask, bmask, amask);
                    }
                    else
                    {
                        supported = false;
                    }
                }
            }
            else
            {
                header.setLinearSize(computeImageSize(w, h, d, compressionOptions.bitcount, compressionOptions.pitchAlignment, compressionOptions.format));

                if (compressionOptions.format == Format_DXT1 || compressionOptions.format == Format_DXT1a || compressionOptions.format == Format_DXT1n) {
                    header.setFourCC('D', 'X', 'T', '1');
                    if (isNormalMap) header.setNormalFlag(true);
                }
                else if (compressionOptions.format == Format_DXT3) {
                    header.setFourCC('D', 'X', 'T', '3');
                }
                else if (compressionOptions.format == Format_DXT5) {
                    header.setFourCC('D', 'X', 'T', '5');
                }
                else if (compressionOptions.format == Format_DXT5n) {
                    header.setFourCC('D', 'X', 'T', '5');
                    if (isNormalMap) {
                        header.setNormalFlag(true);
                        header.setSwizzleCode('A', '2', 'D', '5');
                        //header.setSwizzleCode('x', 'G', 'x', 'R');
                    }
                }
                else if (compressionOptions.format == Format_BC4) {
                    header.setFourCC('A', 'T', 'I', '1');
                }
                else if (compressionOptions.format == Format_BC5) {
                    header.setFourCC('A', 'T', 'I', '2');
                    if (isNormalMap) {
                        header.setNormalFlag(true);
                        header.setSwizzleCode('A', '2', 'X', 'Y');
                    }
                }
                else if (compressionOptions.format == Format_BC6) { // @@ This is not supported by D3DX. Always use DX10 header with BC6-7 formats.
                    header.setFourCC('Z', 'O', 'H', ' ');
                }
                else if (compressionOptions.format == Format_BC7) {
                    header.setFourCC('Z', 'O', 'L', 'A');
                    if (isNormalMap) header.setNormalFlag(true);
                }
                else if (compressionOptions.format == Format_CTX1) {
                    header.setFourCC('C', 'T', 'X', '1');
                    if (isNormalMap) header.setNormalFlag(true);
                }
                else {
                    supported = false;
                }
            }

            if (outputOptions.srgb) header.setSrgbFlag(true);
        }

        if (!supported)
        {
            // This container does not support the requested format.
            outputOptions.error(Error_UnsupportedOutputFormat);
            return false;
        }

        uint headerSize = 128;
        if (header.hasDX10Header())
        {
            nvStaticCheck(sizeof(DDSHeader) == 128 + 20);
            headerSize = 128 + 20;
        }

        // Swap bytes if necessary.
        header.swapBytes();

        bool writeSucceed = outputOptions.writeData(&header, headerSize);
        if (!writeSucceed)
        {
            outputOptions.error(Error_FileWrite);
        }

        return writeSucceed;
    }

    return true;
}


CompressorInterface * Compressor::Private::chooseCpuCompressor(const CompressionOptions::Private & compressionOptions) const
{
    if (compressionOptions.format == Format_RGB)
    {
        return new PixelFormatConverter;
    }
    else if (compressionOptions.format == Format_DXT1)
    {
#if defined(HAVE_ATITC)
        if (compressionOptions.externalCompressor == "ati") return new AtiCompressorDXT1;
        else
#endif

#if defined(HAVE_SQUISH)
        if (compressionOptions.externalCompressor == "squish") return new SquishCompressorDXT1;
        else
#endif

#if defined(HAVE_D3DX)
        if (compressionOptions.externalCompressor == "d3dx") return new D3DXCompressorDXT1;
        else
#endif

#if defined(HAVE_D3DX)
        if (compressionOptions.externalCompressor == "stb") return new StbCompressorDXT1;
        else
#endif

        if (compressionOptions.quality == Quality_Fastest)
        {
            return new FastCompressorDXT1;
        }

        return new NormalCompressorDXT1;
    }
    else if (compressionOptions.format == Format_DXT1a)
    {
        if (compressionOptions.quality == Quality_Fastest)
        {
            return new FastCompressorDXT1a;
        }

        return new NormalCompressorDXT1a;
    }
    else if (compressionOptions.format == Format_DXT1n)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_DXT3)
    {
        if (compressionOptions.quality == Quality_Fastest)
        {
            return new FastCompressorDXT3;
        }

        return new NormalCompressorDXT3;
    }
    else if (compressionOptions.format == Format_DXT5)
    {
#if defined(HAVE_ATITC)
        if (compressionOptions.externalCompressor == "ati") return new AtiCompressorDXT5;
        else
#endif

        if (compressionOptions.quality == Quality_Fastest)
        {
            return new FastCompressorDXT5;
        }

        return new NormalCompressorDXT5;
    }
    else if (compressionOptions.format == Format_DXT5n)
    {
        if (compressionOptions.quality == Quality_Fastest)
        {
            return new FastCompressorDXT5n;
        }

        return new NormalCompressorDXT5n;
    }
    else if (compressionOptions.format == Format_BC4)
    {
        if (compressionOptions.quality == Quality_Fastest || compressionOptions.quality == Quality_Normal)
        {
            return new FastCompressorBC4;
        }

        return new ProductionCompressorBC4;
    }
    else if (compressionOptions.format == Format_BC5)
    {
        if (compressionOptions.quality == Quality_Fastest || compressionOptions.quality == Quality_Normal)
        {
            return new FastCompressorBC5;
        }

        return new ProductionCompressorBC5;
    }
    else if (compressionOptions.format == Format_CTX1)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_BC6)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_BC7)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_RGBE)
    {
        return new CompressorRGBE;
    }

    return NULL;
}


CompressorInterface * Compressor::Private::chooseGpuCompressor(const CompressionOptions::Private & compressionOptions) const
{
    nvDebugCheck(cudaSupported);

    if (compressionOptions.quality == Quality_Fastest)
    {
        // Do not use CUDA compressors in fastest quality mode.
        return NULL;
    }

#if defined HAVE_CUDA
    if (compressionOptions.format == Format_DXT1)
    {
        return new CudaCompressorDXT1(*cuda);
    }
    else if (compressionOptions.format == Format_DXT1a)
    {
        //#pragma NV_MESSAGE("TODO: Implement CUDA DXT1a compressor.")
    }
    else if (compressionOptions.format == Format_DXT1n)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_DXT3)
    {
        //return new CudaCompressorDXT3(*cuda);
    }
    else if (compressionOptions.format == Format_DXT5)
    {
        //return new CudaCompressorDXT5(*cuda);
    }
    else if (compressionOptions.format == Format_DXT5n)
    {
        // @@ Return CUDA compressor.
    }
    else if (compressionOptions.format == Format_BC4)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_BC5)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_CTX1)
    {
        // @@ Return CUDA compressor.
    }
    else if (compressionOptions.format == Format_BC6)
    {
        // Not supported.
    }
    else if (compressionOptions.format == Format_BC7)
    {
        // Not supported.
    }
#endif // defined HAVE_CUDA

    return NULL;
}

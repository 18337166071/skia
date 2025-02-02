/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef skgpu_AtlasTypes_DEFINED
#define skgpu_AtlasTypes_DEFINED

#include <array>

#include "include/core/SkColorType.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypes.h"
#include "include/private/SkTo.h"
#include "src/core/SkIPoint16.h"
#include "src/gpu/RectanizerSkyline.h"

/**
 * This file includes internal types that are used by all of our gpu backends for atlases.
 */

namespace skgpu {

struct IRect16 {
    int16_t fLeft, fTop, fRight, fBottom;

    static IRect16 SK_WARN_UNUSED_RESULT MakeEmpty() {
        IRect16 r;
        r.setEmpty();
        return r;
    }

    static IRect16 SK_WARN_UNUSED_RESULT MakeWH(int16_t w, int16_t h) {
        IRect16 r;
        r.set(0, 0, w, h);
        return r;
    }

    static IRect16 SK_WARN_UNUSED_RESULT MakeXYWH(int16_t x, int16_t y, int16_t w, int16_t h) {
        IRect16 r;
        r.set(x, y, x + w, y + h);
        return r;
    }

    static IRect16 SK_WARN_UNUSED_RESULT Make(const SkIRect& ir) {
        IRect16 r;
        r.set(ir);
        return r;
    }

    int width() const { return fRight - fLeft; }
    int height() const { return fBottom - fTop; }
    int area() const { return this->width() * this->height(); }
    bool isEmpty() const { return fLeft >= fRight || fTop >= fBottom; }

    void setEmpty() { memset(this, 0, sizeof(*this)); }

    void set(int16_t left, int16_t top, int16_t right, int16_t bottom) {
        fLeft = left;
        fTop = top;
        fRight = right;
        fBottom = bottom;
    }

    void set(const SkIRect& r) {
        fLeft   = SkToS16(r.fLeft);
        fTop    = SkToS16(r.fTop);
        fRight  = SkToS16(r.fRight);
        fBottom = SkToS16(r.fBottom);
    }

    void offset(int16_t dx, int16_t dy) {
        fLeft   += dx;
        fTop    += dy;
        fRight  += dx;
        fBottom += dy;
    }
};

/**
 *  Formats for masks, used by the font cache. Important that these are 0-based.
 */
enum class MaskFormat : int {
    kA8,    //!< 1-byte per pixel
    kA565,  //!< 2-bytes per pixel, RGB represent 3-channel LCD coverage
    kARGB,  //!< 4-bytes per pixel, color format

    kLast = kARGB
};
static const int kMaskFormatCount = static_cast<int>(MaskFormat::kLast) + 1;

/**
 *  Return the number of bytes-per-pixel for the specified mask format.
 */
inline constexpr int MaskFormatBytesPerPixel(MaskFormat format) {
    SkASSERT(static_cast<int>(format) < kMaskFormatCount);
    // kA8   (0) -> 1
    // kA565 (1) -> 2
    // kARGB (2) -> 4
    static_assert(static_cast<int>(MaskFormat::kA8) == 0, "enum_order_dependency");
    static_assert(static_cast<int>(MaskFormat::kA565) == 1, "enum_order_dependency");
    static_assert(static_cast<int>(MaskFormat::kARGB) == 2, "enum_order_dependency");

    return SkTo<int>(1u << static_cast<int>(format));
}

static constexpr SkColorType MaskFormatToColorType(MaskFormat format) {
    switch (format) {
        case MaskFormat::kA8:
            return kAlpha_8_SkColorType;
        case MaskFormat::kA565:
            return kRGB_565_SkColorType;
        case MaskFormat::kARGB:
            return kRGBA_8888_SkColorType;
    }
    SkUNREACHABLE;
}

/**
 * Keep track of generation number for atlases and Plots.
 */
class AtlasGenerationCounter {
public:
    inline static constexpr uint64_t kInvalidGeneration = 0;
    uint64_t next() {
        return fGeneration++;
    }

private:
    uint64_t fGeneration{1};
};

/**
 * A PlotLocator specifies the plot and is analogous to a directory path:
 *    page/plot/plotGeneration
 *
 * In fact PlotLocator is a portion of a glyph image location in the atlas fully specified by:
 *    format/atlasGeneration/page/plot/plotGeneration/rect
 *
 * TODO: Remove the small path renderer's use of the PlotLocator for eviction.
 */
class PlotLocator {
public:
    // These are both restricted by the space they occupy in the PlotLocator.
    // maxPages is also limited by being crammed into the glyph uvs.
    // maxPlots is also limited by the fPlotAlreadyUpdated bitfield in
    // GrDrawOpAtlas::BulkUseTokenUpdater.
    inline static constexpr auto kMaxMultitexturePages = 4;
    inline static constexpr int kMaxPlots = 32;

    PlotLocator(uint32_t pageIdx, uint32_t plotIdx, uint64_t generation)
            : fGenID(generation)
            , fPlotIndex(plotIdx)
            , fPageIndex(pageIdx) {
        SkASSERT(pageIdx < kMaxMultitexturePages);
        SkASSERT(plotIdx < kMaxPlots);
        SkASSERT(generation < ((uint64_t)1 << 48));
    }

    PlotLocator()
            : fGenID(AtlasGenerationCounter::kInvalidGeneration)
            , fPlotIndex(0)
            , fPageIndex(0) {}

    bool isValid() const {
        return fGenID != 0 || fPlotIndex != 0 || fPageIndex != 0;
    }

    void makeInvalid() {
        fGenID = 0;
        fPlotIndex = 0;
        fPageIndex = 0;
    }

    bool operator==(const PlotLocator& other) const {
        return fGenID == other.fGenID &&
               fPlotIndex == other.fPlotIndex &&
               fPageIndex == other.fPageIndex; }

    uint32_t pageIndex() const { return fPageIndex; }
    uint32_t plotIndex() const { return fPlotIndex; }
    uint64_t genID() const { return fGenID; }

private:
    uint64_t fGenID:48;
    uint64_t fPlotIndex:8;
    uint64_t fPageIndex:8;
};

// AtlasLocator handles atlas position information. It keeps a left-top, right-bottom pair of
// encoded UV coordinates. The bits 13 & 14 of the U coordinates hold the atlas page index.
// This information is handed directly as is from fUVs. This encoding has the nice property
// that width = fUVs[2] - fUVs[0]; the page encoding in the top bits subtracts to zero.
class AtlasLocator {
public:
    std::array<uint16_t, 4> getUVs() const {
        return fUVs;
    }

    void invalidatePlotLocator() { fPlotLocator.makeInvalid(); }

    // TODO: Remove the small path renderer's use of this for eviction
    PlotLocator plotLocator() const { return fPlotLocator; }

    uint32_t pageIndex() const { return fPlotLocator.pageIndex(); }

    uint32_t plotIndex() const { return fPlotLocator.plotIndex(); }

    uint64_t genID() const { return fPlotLocator.genID(); }

    SkIPoint topLeft() const {
        return {fUVs[0] & 0x1FFF, fUVs[1]};
    }

    uint16_t width() const {
        return fUVs[2] - fUVs[0];
    }

    uint16_t height() const {
        return fUVs[3] - fUVs[1];
    }

    void insetSrc(int padding) {
        SkASSERT(2 * padding <= this->width());
        SkASSERT(2 * padding <= this->height());

        fUVs[0] += padding;
        fUVs[1] += padding;
        fUVs[2] -= padding;
        fUVs[3] -= padding;
    }

    void updatePlotLocator(PlotLocator p) {
        fPlotLocator = p;
        SkASSERT(fPlotLocator.pageIndex() <= 3);
        uint16_t page = fPlotLocator.pageIndex() << 13;
        fUVs[0] = (fUVs[0] & 0x1FFF) | page;
        fUVs[2] = (fUVs[2] & 0x1FFF) | page;
    }

    void updateRect(skgpu::IRect16 rect) {
        SkASSERT(rect.fLeft <= rect.fRight);
        SkASSERT(rect.fRight <= 0x1FFF);
        fUVs[0] = (fUVs[0] & 0xE000) | rect.fLeft;
        fUVs[1] = rect.fTop;
        fUVs[2] = (fUVs[2] & 0xE000) | rect.fRight;
        fUVs[3] = rect.fBottom;
    }

private:
    PlotLocator fPlotLocator{0, 0, 0};

    // The inset padded bounds in the atlas in the lower 13 bits, and page index in bits 13 &
    // 14 of the Us.
    std::array<uint16_t, 4> fUVs{0, 0, 0, 0};
};

/**
 * An interface for eviction callbacks. Whenever an atlas evicts a specific PlotLocator,
 * it will call all of the registered listeners so they can process the eviction.
 */
class PlotEvictionCallback {
public:
    virtual ~PlotEvictionCallback() = default;
    virtual void evict(PlotLocator) = 0;
};

/**
 * The backing texture for an atlas is broken into a spatial grid of Plots. The Plots
 * keep track of subimage placement via their Rectanizer. A Plot may be subclassed if
 * the atlas class needs to track additional information.
 */
class Plot : public SkRefCnt {
public:
    Plot(int pageIndex, int plotIndex, AtlasGenerationCounter* generationCounter,
         int offX, int offY, int width, int height, SkColorType colorType, size_t bpp);

    uint32_t pageIndex() const { return fPageIndex; }

    /** plotIndex() is a unique id for the plot relative to the owning GrAtlas and page. */
    uint32_t plotIndex() const { return fPlotIndex; }
    /**
     * genID() is incremented when the plot is evicted due to a atlas spill. It is used to know
     * if a particular subimage is still present in the atlas.
     */
    uint64_t genID() const { return fGenID; }
    PlotLocator plotLocator() const {
        SkASSERT(fPlotLocator.isValid());
        return fPlotLocator;
    }
    SkDEBUGCODE(size_t bpp() const { return fBytesPerPixel; })

    bool addSubImage(int width, int height, const void* image, AtlasLocator* atlasLocator);

    void resetRects();

protected:
    ~Plot() override;

    struct {
        const uint32_t fPageIndex : 16;
        const uint32_t fPlotIndex : 16;
    };
    AtlasGenerationCounter* const fGenerationCounter;
    uint64_t fGenID;
    PlotLocator fPlotLocator;
    unsigned char* fData;
    const int fWidth;
    const int fHeight;
    const int fX;
    const int fY;
    skgpu::RectanizerSkyline fRectanizer;
    const SkIPoint16 fOffset;  // the offset of the plot in the backing texture
    const SkColorType fColorType;
    const size_t fBytesPerPixel;
    SkIRect fDirtyRect;
    SkDEBUGCODE(bool fDirty);
};

} // namespace skgpu

#endif // skgpu_GpuTypesInternal_DEFINED

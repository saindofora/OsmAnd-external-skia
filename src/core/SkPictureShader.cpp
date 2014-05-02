/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkPictureShader.h"

#include "SkBitmap.h"
#include "SkBitmapProcShader.h"
#include "SkCanvas.h"
#include "SkMatrixUtils.h"
#include "SkPicture.h"
#include "SkReadBuffer.h"

#if SK_SUPPORT_GPU
#include "GrContext.h"
#endif

SkPictureShader::SkPictureShader(SkPicture* picture, TileMode tmx, TileMode tmy)
    : fPicture(SkRef(picture))
    , fTmx(tmx)
    , fTmy(tmy) { }

SkPictureShader::SkPictureShader(SkReadBuffer& buffer)
        : INHERITED(buffer) {
    fTmx = static_cast<SkShader::TileMode>(buffer.read32());
    fTmy = static_cast<SkShader::TileMode>(buffer.read32());
    fPicture = SkPicture::CreateFromBuffer(buffer);
}

SkPictureShader::~SkPictureShader() {
    fPicture->unref();
}

SkPictureShader* SkPictureShader::Create(SkPicture* picture, TileMode tmx, TileMode tmy) {
    if (!picture || 0 == picture->width() || 0 == picture->height()) {
        return NULL;
    }
    return SkNEW_ARGS(SkPictureShader, (picture, tmx, tmy));
}

void SkPictureShader::flatten(SkWriteBuffer& buffer) const {
    this->INHERITED::flatten(buffer);

    buffer.write32(fTmx);
    buffer.write32(fTmy);
    fPicture->flatten(buffer);
}

SkShader* SkPictureShader::refBitmapShader(const SkMatrix& matrix) const {
    SkASSERT(fPicture && fPicture->width() > 0 && fPicture->height() > 0);

    SkMatrix m;
    if (this->hasLocalMatrix()) {
        m.setConcat(matrix, this->getLocalMatrix());
    } else {
        m = matrix;
    }

    // Use a rotation-invariant scale
    SkPoint scale;
    if (!SkDecomposeUpper2x2(m, NULL, &scale, NULL)) {
        // Decomposition failed, use an approximation.
        scale.set(SkScalarSqrt(m.getScaleX() * m.getScaleX() + m.getSkewX() * m.getSkewX()),
                  SkScalarSqrt(m.getScaleY() * m.getScaleY() + m.getSkewY() * m.getSkewY()));
    }
    SkSize scaledSize = SkSize::Make(scale.x() * fPicture->width(), scale.y() * fPicture->height());

    SkISize tileSize = scaledSize.toRound();
    if (tileSize.isEmpty()) {
        return NULL;
    }

    // The actual scale, compensating for rounding.
    SkSize tileScale = SkSize::Make(SkIntToScalar(tileSize.width()) / fPicture->width(),
                                    SkIntToScalar(tileSize.height()) / fPicture->height());

    SkAutoMutexAcquire ama(fCachedBitmapShaderMutex);

    if (!fCachedBitmapShader || tileScale != fCachedTileScale ||
        this->getLocalMatrix() != fCachedLocalMatrix) {
        SkBitmap bm;
        if (!bm.allocN32Pixels(tileSize.width(), tileSize.height())) {
            return NULL;
        }
        bm.eraseColor(SK_ColorTRANSPARENT);

        SkCanvas canvas(bm);
        canvas.scale(tileScale.width(), tileScale.height());
        canvas.drawPicture(*fPicture);

        fCachedTileScale = tileScale;
        fCachedLocalMatrix = this->getLocalMatrix();

        SkMatrix shaderMatrix = this->getLocalMatrix();
        shaderMatrix.preScale(1 / tileScale.width(), 1 / tileScale.height());
        fCachedBitmapShader.reset(CreateBitmapShader(bm, fTmx, fTmy, &shaderMatrix));
    }

    // Increment the ref counter inside the mutex to ensure the returned pointer is still valid.
    // Otherwise, the pointer may have been overwritten on a different thread before the object's
    // ref count was incremented.
    fCachedBitmapShader.get()->ref();
    return fCachedBitmapShader;
}

SkShader* SkPictureShader::validInternal(const ContextRec& rec, SkMatrix* totalInverse) const {
    if (!this->INHERITED::validContext(rec, totalInverse)) {
        return NULL;
    }

    SkAutoTUnref<SkShader> bitmapShader(this->refBitmapShader(*rec.fMatrix));
    if (!bitmapShader || !bitmapShader->validContext(rec)) {
        return NULL;
    }

    return bitmapShader.detach();
}

bool SkPictureShader::validContext(const ContextRec& rec, SkMatrix* totalInverse) const {
    SkAutoTUnref<SkShader> shader(this->validInternal(rec, totalInverse));
    return shader != NULL;
}

SkShader::Context* SkPictureShader::createContext(const ContextRec& rec, void* storage) const {
    SkAutoTUnref<SkShader> bitmapShader(this->validInternal(rec, NULL));
    if (!bitmapShader) {
        return NULL;
    }

    return SkNEW_PLACEMENT_ARGS(storage, PictureShaderContext, (*this, rec, bitmapShader.detach()));
}

size_t SkPictureShader::contextSize() const {
    return sizeof(PictureShaderContext);
}

SkPictureShader::PictureShaderContext::PictureShaderContext(
        const SkPictureShader& shader, const ContextRec& rec, SkShader* bitmapShader)
    : INHERITED(shader, rec)
    , fBitmapShader(bitmapShader)
{
    SkASSERT(fBitmapShader);
    fBitmapShaderContextStorage = sk_malloc_throw(fBitmapShader->contextSize());
    fBitmapShaderContext = fBitmapShader->createContext(rec, fBitmapShaderContextStorage);
    SkASSERT(fBitmapShaderContext);
}

SkPictureShader::PictureShaderContext::~PictureShaderContext() {
    fBitmapShaderContext->~Context();
    sk_free(fBitmapShaderContextStorage);
}

uint32_t SkPictureShader::PictureShaderContext::getFlags() const {
    return fBitmapShaderContext->getFlags();
}

SkShader::Context::ShadeProc SkPictureShader::PictureShaderContext::asAShadeProc(void** ctx) {
    return fBitmapShaderContext->asAShadeProc(ctx);
}

void SkPictureShader::PictureShaderContext::shadeSpan(int x, int y, SkPMColor dstC[], int count) {
    SkASSERT(fBitmapShaderContext);
    fBitmapShaderContext->shadeSpan(x, y, dstC, count);
}

void SkPictureShader::PictureShaderContext::shadeSpan16(int x, int y, uint16_t dstC[], int count) {
    SkASSERT(fBitmapShaderContext);
    fBitmapShaderContext->shadeSpan16(x, y, dstC, count);
}

#ifndef SK_IGNORE_TO_STRING
void SkPictureShader::toString(SkString* str) const {
    static const char* gTileModeName[SkShader::kTileModeCount] = {
        "clamp", "repeat", "mirror"
    };

    str->appendf("PictureShader: [%d:%d] ",
                 fPicture ? fPicture->width() : 0,
                 fPicture ? fPicture->height() : 0);

    str->appendf("(%s, %s)", gTileModeName[fTmx], gTileModeName[fTmy]);

    this->INHERITED::toString(str);
}
#endif

#if SK_SUPPORT_GPU
GrEffectRef* SkPictureShader::asNewEffect(GrContext* context, const SkPaint& paint) const {
    SkAutoTUnref<SkShader> bitmapShader(this->refBitmapShader(context->getMatrix()));
    if (!bitmapShader) {
        return NULL;
    }
    return bitmapShader->asNewEffect(context, paint);
}
#endif

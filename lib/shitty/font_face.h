/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/sys/types.h>

namespace stl {
    class Buffer;
    class StringView;
}

// A resolved font: the raw bytes of a font file plus the face index inside
// a collection. A face is size-independent — one face serves any pixel
// size. Faces are born unreferenced: the resolver hands one over and the
// first IntrusivePtr adopts it. The count is virtual, not embedded: an
// implementation over persistent bytes makes ref/unref no-ops and outlives
// every pointer, one over a mapping counts its consumers and unmaps after
// the last one.
struct FontFace {
    virtual ~FontFace() noexcept;

    // Dense program-wide identity, small enough to pack into hash-map
    // keys; implementations take nextFontFaceId() at construction.
    virtual u32 id() const noexcept = 0;

    virtual const void* data() const = 0;
    virtual size_t size() const = 0;
    virtual i32 faceIndex() const = 0;

    virtual void ref() noexcept = 0;
    virtual i32 unref() noexcept = 0;
    virtual i32 refCount() const noexcept = 0;
};

// The auto-incremented program-wide counter behind FontFace::id().
u32 nextFontFaceId() noexcept;

// A counted face over caller-owned bytes; the bytes must outlive the face.
FontFace* createMemoryFontFace(const void* data, size_t size, i32 faceIndex);

// Owns a moved byte buffer for the lifetime of the returned face.
FontFace* createOwnedFontFace(stl::Buffer&& data, i32 faceIndex);

// Maps a font file into memory; throws when the file cannot be opened.
FontFace* openFontFile(stl::StringView path, i32 faceIndex);

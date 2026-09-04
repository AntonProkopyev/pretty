/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "font_windows.h"

#include "composer.h"
#include "font_face.h"
#include "font_resolver.h"

#include <std/mem/obj_pool.h>
#include <std/lib/buffer.h>
#include <std/str/view.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <dwrite.h>
    #include <dwrite_2.h>
    #include <wrl/client.h>

    #include <climits>
    #include <string>
    #include <vector>
#endif

using namespace stl;

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

namespace {
    std::wstring wide(StringView text) {
        if (text.empty() || text.length() > INT_MAX) {
            return {};
        }
        const int length = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(text.data()),
            static_cast<int>(text.length()),
            nullptr,
            0
        );
        if (length <= 0) {
            return {};
        }
        std::wstring result(static_cast<size_t>(length), L'\0');
        return MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(text.data()),
            static_cast<int>(text.length()),
            result.data(),
            length
        ) == length ? result : std::wstring();
    }

    DWRITE_FONT_WEIGHT weight(FontStyle style) {
        return style == FontStyle::Bold || style == FontStyle::BoldItalic
            ? DWRITE_FONT_WEIGHT_BOLD
            : DWRITE_FONT_WEIGHT_REGULAR;
    }

    DWRITE_FONT_STYLE fontStyle(FontStyle style) {
        return style == FontStyle::Italic || style == FontStyle::BoldItalic
            ? DWRITE_FONT_STYLE_ITALIC
            : DWRITE_FONT_STYLE_NORMAL;
    }

    bool covers(IDWriteFontFace& face, const u32* codepoints, size_t count) {
        std::vector<UINT32> requested(count);
        std::vector<UINT16> glyphs(count);
        for (size_t index = 0; index != count; ++index) {
            requested[index] = codepoints[index];
        }
        if (FAILED(face.GetGlyphIndices(
                requested.data(),
                static_cast<UINT32>(count),
                glyphs.data()
            ))) {
            return false;
        }
        for (const UINT16 glyph : glyphs) {
            if (glyph == 0) {
                return false;
            }
        }
        return true;
    }

    bool colored(IDWriteFontFace& face) {
        ComPtr<IDWriteFontFace2> extended;
        return SUCCEEDED(face.QueryInterface(
            __uuidof(IDWriteFontFace2),
            reinterpret_cast<void**>(extended.GetAddressOf())
        )) && extended->IsColorFont();
    }

    FontFace* mappedFace(IDWriteFontFace& face) {
        UINT32 fileCount = 0;
        if (FAILED(face.GetFiles(&fileCount, nullptr)) || fileCount == 0) {
            return nullptr;
        }
        std::vector<IDWriteFontFile*> rawFiles(fileCount, nullptr);
        if (FAILED(face.GetFiles(&fileCount, rawFiles.data()))) {
            return nullptr;
        }
        std::vector<ComPtr<IDWriteFontFile>> files(fileCount);
        for (UINT32 index = 0; index != fileCount; ++index) {
            files[index].Attach(rawFiles[index]);
        }
        const void* key = nullptr;
        UINT32 keySize = 0;
        ComPtr<IDWriteFontFileLoader> loader;
        if (FAILED(files.front()->GetReferenceKey(&key, &keySize))
                || FAILED(files.front()->GetLoader(loader.GetAddressOf()))) {
            return nullptr;
        }
        ComPtr<IDWriteFontFileStream> stream;
        if (FAILED(loader->CreateStreamFromKey(
                key,
                keySize,
                stream.GetAddressOf()
            ))) {
            return nullptr;
        }
        UINT64 size = 0;
        const void* fragment = nullptr;
        void* context = nullptr;
        if (FAILED(stream->GetFileSize(&size)) || size == 0 || size > SIZE_MAX
                || FAILED(stream->ReadFileFragment(
                    &fragment,
                    0,
                    size,
                    &context
                ))) {
            return nullptr;
        }
        Buffer content(fragment, static_cast<size_t>(size));
        stream->ReleaseFileFragment(context);
        return createOwnedFontFace(
            static_cast<Buffer&&>(content),
            face.GetIndex()
        );
    }

    struct WindowsFontResolver final: FontResolver {
        explicit WindowsFontResolver(Composer& composer_)
            : composer(composer_)
        {
        }

        bool initialize() {
            if (FAILED(DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(factory.GetAddressOf())
                ))) {
                return false;
            }
            return SUCCEEDED(factory->GetSystemFontCollection(
                collection.GetAddressOf(),
                FALSE
            ));
        }

        FontFace* resolve(const FontRequest& request) override {
            if (request.name.memChr('/') || request.name.memChr('\\')) {
                return nullptr;
            }
            const std::wstring familyName = wide(request.name);
            if (familyName.empty()) {
                return nullptr;
            }
            UINT32 familyIndex = 0;
            BOOL exists = FALSE;
            if (FAILED(collection->FindFamilyName(
                    familyName.c_str(),
                    &familyIndex,
                    &exists
                )) || !exists) {
                return nullptr;
            }
            ComPtr<IDWriteFontFamily> family;
            ComPtr<IDWriteFont> font;
            ComPtr<IDWriteFontFace> face;
            if (FAILED(collection->GetFontFamily(familyIndex, family.GetAddressOf()))
                    || FAILED(family->GetFirstMatchingFont(
                        weight(request.style),
                        DWRITE_FONT_STRETCH_NORMAL,
                        fontStyle(request.style),
                        font.GetAddressOf()
                    ))
                    || FAILED(font->CreateFontFace(face.GetAddressOf()))) {
                return nullptr;
            }
            return mappedFace(*face.Get());
        }

        FontFace* resolveCluster(
            const u32* codepoints,
            size_t count,
            FontPlane plane
        ) override {
            if (count == 0) {
                return nullptr;
            }
            // ponytail: cached misses use a linear scan; use IDWriteFontFallback
            // if profiling shows repeated scans outside FontPack's miss cache.
            for (UINT32 familyIndex = 0;
                    familyIndex != collection->GetFontFamilyCount();
                    ++familyIndex) {
                ComPtr<IDWriteFontFamily> family;
                if (FAILED(collection->GetFontFamily(
                        familyIndex,
                        family.GetAddressOf()
                    ))) {
                    continue;
                }
                for (UINT32 fontIndex = 0;
                        fontIndex != family->GetFontCount();
                        ++fontIndex) {
                    ComPtr<IDWriteFont> font;
                    ComPtr<IDWriteFontFace> face;
                    if (FAILED(family->GetFont(fontIndex, font.GetAddressOf()))
                            || FAILED(font->CreateFontFace(face.GetAddressOf()))
                            || !covers(*face.Get(), codepoints, count)) {
                        continue;
                    }
                    const bool color = colored(*face.Get());
                    if ((plane == FontPlane::Color && !color)
                            || (plane == FontPlane::Mask && color)) {
                        continue;
                    }
                    if (FontFace* const resolved = mappedFace(*face.Get())) {
                        return resolved;
                    }
                }
            }
            return nullptr;
        }

        Composer& composer;
        ComPtr<IDWriteFactory> factory;
        ComPtr<IDWriteFontCollection> collection;
    };
}
#endif

FontResolver* createWindowsFontResolver(Composer& composer) {
#if defined(_WIN32)
    WindowsFontResolver* const resolver = composer.pool->make<WindowsFontResolver>(composer);
    return resolver->initialize() ? resolver : nullptr;
#else
    (void)(composer);
    return nullptr;
#endif
}

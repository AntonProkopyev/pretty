#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <lib/shitty/font_face.h>
#include <std/str/view.h>

#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace stl;

namespace {
    struct Apartment final {
        bool initialize() {
            const HRESULT result = OleInitialize(nullptr);
            active = result == S_OK || result == S_FALSE;
            return active;
        }

        ~Apartment() noexcept {
            if (active) {
                OleUninitialize();
            }
        }

        bool active = false;
    };

    std::string utf8(const wchar_t* text, size_t length) {
        const int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            static_cast<int>(length),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (size <= 0) {
            return {};
        }
        std::string result(static_cast<size_t>(size), '\0');
        return WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            static_cast<int>(length),
            result.data(),
            size,
            nullptr,
            nullptr
        ) == size ? result : std::string();
    }

    ComPtr<IDWriteFontFace> face(
        IDWriteFontCollection& collection,
        const wchar_t* name,
        DWRITE_FONT_WEIGHT weight,
        DWRITE_FONT_STYLE style
    ) {
        UINT32 familyIndex = 0;
        BOOL exists = FALSE;
        if (FAILED(collection.FindFamilyName(name, &familyIndex, &exists)) || !exists) {
            return {};
        }
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFace> result;
        if (FAILED(collection.GetFontFamily(familyIndex, family.GetAddressOf()))
                || FAILED(family->GetFirstMatchingFont(
                    weight,
                    DWRITE_FONT_STRETCH_NORMAL,
                    style,
                    font.GetAddressOf()
                ))
                || FAILED(font->CreateFontFace(result.GetAddressOf()))) {
            return {};
        }
        return result;
    }

    bool covers(IDWriteFontFace& face, UINT32 codepoint) {
        UINT16 glyph = 0;
        return SUCCEEDED(face.GetGlyphIndices(&codepoint, 1, &glyph)) && glyph != 0;
    }

    std::string path(IDWriteFontFace& face) {
        UINT32 count = 0;
        if (FAILED(face.GetFiles(&count, nullptr)) || count == 0) {
            return {};
        }
        std::vector<IDWriteFontFile*> raw(count, nullptr);
        if (FAILED(face.GetFiles(&count, raw.data()))) {
            return {};
        }
        std::vector<ComPtr<IDWriteFontFile>> files(count);
        for (UINT32 index = 0; index != count; ++index) {
            files[index].Attach(raw[index]);
        }
        const void* key = nullptr;
        UINT32 keySize = 0;
        ComPtr<IDWriteFontFileLoader> loader;
        ComPtr<IDWriteLocalFontFileLoader> local;
        if (FAILED(files.front()->GetReferenceKey(&key, &keySize))
                || FAILED(files.front()->GetLoader(loader.GetAddressOf()))
                || FAILED(loader.As(&local))) {
            return {};
        }
        UINT32 length = 0;
        if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &length))) {
            return {};
        }
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        if (FAILED(local->GetFilePathFromKey(
                key,
                keySize,
                value.data(),
                length + 1
            ))) {
            return {};
        }
        return utf8(value.data(), length);
    }

    bool cjk(IDWriteFontCollection& collection) {
        constexpr UINT32 codepoint = 0x4e2d;
        for (UINT32 familyIndex = 0;
                familyIndex != collection.GetFontFamilyCount();
                ++familyIndex) {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection.GetFontFamily(
                    familyIndex,
                    family.GetAddressOf()
                ))) {
                continue;
            }
            for (UINT32 fontIndex = 0; fontIndex != family->GetFontCount(); ++fontIndex) {
                ComPtr<IDWriteFont> font;
                ComPtr<IDWriteFontFace> candidate;
                if (SUCCEEDED(family->GetFont(fontIndex, font.GetAddressOf()))
                        && SUCCEEDED(font->CreateFontFace(candidate.GetAddressOf()))
                        && covers(*candidate.Get(), codepoint)) {
                    return true;
                }
            }
        }
        return false;
    }
}

int main() {
    Apartment apartment;
    if (!apartment.initialize()) {
        return 1;
    }
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf())
        ))
            || FAILED(factory->GetSystemFontCollection(
                collection.GetAddressOf(),
                FALSE
            ))) {
        return 2;
    }
    ComPtr<IDWriteFontFace> regular = face(
        *collection.Get(),
        L"Consolas",
        DWRITE_FONT_WEIGHT_REGULAR,
        DWRITE_FONT_STYLE_NORMAL
    );
    ComPtr<IDWriteFontFace> bold = face(
        *collection.Get(),
        L"Consolas",
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL
    );
    ComPtr<IDWriteFontFace> italic = face(
        *collection.Get(),
        L"Consolas",
        DWRITE_FONT_WEIGHT_REGULAR,
        DWRITE_FONT_STYLE_ITALIC
    );
    if (!regular || !bold || !italic || !covers(*regular.Get(), 'A')) {
        return 3;
    }
    const std::string filename = path(*regular.Get());
    if (filename.empty()) {
        return 4;
    }
    FontFace* const mapped = openFontFile(
        StringView(filename.c_str()),
        static_cast<i32>(regular->GetIndex())
    );
    const bool mapping = mapped->size() > 0;
    delete mapped;
    ComPtr<IDWriteFontFace> emoji = face(
        *collection.Get(),
        L"Segoe UI Emoji",
        DWRITE_FONT_WEIGHT_REGULAR,
        DWRITE_FONT_STYLE_NORMAL
    );
    const bool emojiCoverage = emoji && covers(*emoji.Get(), 0x1f642);
    const bool cjkCoverage = cjk(*collection.Get());
    std::printf(
        "family=Consolas bytes=%zu emoji=%d cjk=%d\n",
        mapping ? filename.length() : 0,
        emojiCoverage,
        cjkCoverage
    );
    return mapping && emojiCoverage && cjkCoverage ? 0 : 5;
}

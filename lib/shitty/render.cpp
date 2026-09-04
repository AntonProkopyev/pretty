/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "render.h"

#include "render_reference.h"
#include <lib/vterm/terminal_types.h>

#include <plt/window.h>

#if defined(HAVE_METAL_RENDERER)
    #include "render_metal.h"
#elif defined(HAVE_VULKAN_RENDERER)
    #include "render_vk.h"
#else
    #error No renderer backend selected
#endif

bool Renderer::captureOutput(stl::Buffer& rgb, u32& width, u32& height) {
    (void)(rgb);
    (void)(width);
    (void)(height);
    return false;
}

Renderer* Renderer::create(Composer& composer, stl::ObjPool& pool, const plt::RenderContext& context) {
    if (context.backend == plt::RenderBackend::Headless) {
        return ReferenceRenderer::create(composer, pool, context);
    }
#if defined(HAVE_METAL_RENDERER)
    return createMetalRenderer(composer, pool, context);
#else
    return createVulkanRenderer(composer, pool, context);
#endif
}

u32 Renderer::cellAttributes(const TerminalCell& cell) {
    return ((u32)(cell.bold) << 2) | ((u32)(cell.italic) << 3) | ((u32)(cell.underlined()) << 4) | ((u32)(cell.inverse) << 5) | ((u32)(cell.wrap) << 6) | ((u32)(cell.faint) << 8) | ((u32)(cell.blink) << 9) | ((u32)(cell.conceal) << 10) | ((u32)(cell.strike) << 11) | ((u32)(cell.overline) << 12) | ((u32)(cell.underline_style) << 13) | ((u32)(cell.dwidth) << 16) | ((u32)(cell.dwidth_cont) << 17) | ((u32)(cell.protected_char) << 18) | ((u32)(cell.drawn) << 20);
}

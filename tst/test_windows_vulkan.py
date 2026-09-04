# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WindowsVulkanContractTests(unittest.TestCase):
    def test_build_graph_enables_windows_vulkan(self):
        build = (ROOT / "build.py").read_text()
        windows = (ROOT / "windows_build.py").read_text()

        self.assertIn("HAVE_VULKAN_RENDERER", build)
        self.assertIn("HAVE_VULKAN_WIN32", build)
        self.assertIn("vulkan_test.cpp", windows)
        self.assertIn("libvulkan-1.dll.a", windows)
        self.assertIn('group("windows-vulkan-test"', windows)

    def test_renderer_selects_win32_wsi(self):
        source = (ROOT / "lib/shitty/render_vk.cpp").read_text()

        self.assertIn("HAVE_VULKAN_WIN32", source)
        self.assertIn("VK_KHR_WIN32_SURFACE_EXTENSION_NAME", source)
        self.assertIn("VkWin32SurfaceCreateInfoKHR", source)
        self.assertIn("vkCreateWin32SurfaceKHR", source)

    def test_renderer_uses_platform_neutral_vulkan_guard(self):
        source = (ROOT / "lib/shitty/render.cpp").read_text()

        self.assertIn("HAVE_VULKAN_RENDERER", source)
        self.assertNotIn("HAVE_VULKAN_WAYLAND", source)

    def test_probe_presents_and_recreates_swapchain(self):
        source = (ROOT / "dev/windows/vulkan_test.cpp").read_text()

        required = [
            "vkCreateSwapchainKHR",
            "vkAcquireNextImageKHR",
            "vkCmdClearColorImage",
            "vkCmdCopyImageToBuffer",
            "vkQueuePresentKHR",
            "SW_MINIMIZE",
            "SW_RESTORE",
        ]
        for text in required:
            with self.subTest(text=text):
                self.assertIn(text, source)


if __name__ == "__main__":
    unittest.main()

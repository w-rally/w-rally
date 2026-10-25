#pragma once
// WRally's Dear ImGui configuration (IMGUI_USER_CONFIG): 32-bit indices, so ImGui's draw data is the renderer's
// UiDrawList as it is (wrally/iface/renderer.h: uint32_t indices) - no conversion per frame.
#define ImDrawIdx unsigned int
// Only the current API: nothing of ours leans on what ImGui has deprecated.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

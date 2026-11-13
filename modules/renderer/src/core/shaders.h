// ---------------------------------------------------------------------------
// The renderer's shaders: HLSL files in modules/renderer/shaders (docs/ngg-design.md, "Shaders and the pipeline
// cache"), asked for by file, entry point, profile and compiler arguments - one line of shaders.txt.
//
// PRECOMPILED: the build's ngg-shaderc (src/shaderc) compiles every line of shaders/shaders.txt into WRenderer.dll
// (shaders_embedded.h), so a shipped game compiles nothing and needs neither dxcompiler.dll nor dxil.dll - ray tracing
// included. A shader not listed there does not load.
//
// DEVELOPMENT: NGG_SHADER_DIR=<repo>/modules/renderer/shaders compiles them from the files at run time instead
// (ShaderCompiler, shader_compiler.h - the build's own code and arguments), so a shader edit needs no rebuild, only a
// restart; the log names any shader asked for that shaders.txt does not list. NGG_SHADER_DUMP=<dir> writes each
// shader out as it is handed out.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3dcommon.h>
#include <wrl/client.h>
#include <initializer_list>
#include <string>
#include <vector>

namespace ngg {
namespace core {

// `file`: "scene.hlsl"; `profile`: "ps_5_1" (FXC, DXBC) or "ps_6_0" (DXC, DXIL); `args` as DXC takes them, e.g.
// { "-HV", "2018", "-DALPHA_TEST=1" } (FXC takes the -D ones). Null after logging the compiler's diagnostics.
Microsoft::WRL::ComPtr<ID3DBlob> LoadShader(const char* file, const char* entry, const char* profile,
                                            std::initializer_list<const char*> args = {});
Microsoft::WRL::ComPtr<ID3DBlob> LoadShader(const char* file, const char* entry, const char* profile,
                                            const std::vector<std::string>& args);

// Shader model 6 (DXIL) shaders can be had: precompiled, or DXC is loadable for the run-time compile.
bool ShaderModel6Available();

// The source of a shader file with its includes written out in place (NGG_DUMP_PS's .hlsl, for RGA and fxc by hand),
// from NGG_SHADER_DIR or the folder the build was made from; empty when the files are not at hand.
std::string FlattenedShaderSource(const char* file);

// One line for the log: how many shaders were handed out, precompiled or compiled, and the time they took since the
// last call.
void LogShaderStats(const char* when);

} // namespace core
} // namespace ngg

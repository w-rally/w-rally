// ---------------------------------------------------------------------------
// The renderer's shaders: HLSL files in modules/renderer/shaders (docs/ngg-design.md, "Shaders and the pipeline
// cache"), asked for by file, entry point, profile and compiler arguments - one line of shaders.txt.
//
// Compiled at run time from the files (ShaderCompiler, shader_compiler.h): from NGG_SHADER_DIR=<dir> when it is set,
// else from the source folder the build was made from. NGG_SHADER_DUMP=<dir> writes each one out as it is handed out.
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

// Shader model 6 (DXIL) shaders can be had: DXC is loadable.
bool ShaderModel6Available();

// The source of a shader file with its includes written out in place (NGG_DUMP_PS's .hlsl, for RGA and fxc by hand);
// empty when the files are not at hand.
std::string FlattenedShaderSource(const char* file);

// One line for the log: how many shaders, compiled or loaded, and the time they took since the last call.
void LogShaderStats(const char* when);

} // namespace core
} // namespace ngg

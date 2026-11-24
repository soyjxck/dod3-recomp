# Third-party notices

dod3-recomp includes or links the following. Each keeps its own licence.

| Component | Used for | Licence |
|---|---|---|
| [ps3recomp](https://github.com/sp00nznet/ps3recomp) (the [soyjxck fork](https://github.com/soyjxck/ps3recomp), as the `ps3recomp` submodule), © 2026 Ned Heller / sp00nznet | The recompiler, the runtime and the PS3 library implementations | MIT, see `ps3recomp/LICENSE` |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b, © 2014-2026 Omar Cornut, in `third_party/imgui` | The installer's and the launcher's user interface | MIT, see `third_party/imgui/LICENSE.txt` |
| [Inter](https://github.com/rsms/inter) 4.1, © 2016 The Inter Project Authors, in `third_party/fonts/inter` (Regular, Medium, SemiBold, Display Bold) | The launcher's and the setup's typeface | SIL Open Font License 1.1, see `third_party/fonts/inter/LICENSE.txt` |
| stb_image by Sean Barrett (the copy in `ps3recomp/libs/codec`) | Reading the key art the launcher and the setup show | MIT or public domain |
| stb_rectpack, stb_textedit, stb_truetype by Sean Barrett (`third_party/imgui/imstb_*.h`) | Part of Dear ImGui | MIT or public domain, see each file |
| [zlib](https://zlib.net), © Jean-loup Gailly and Mark Adler | Decompression | zlib licence |
| [SDL2](https://libsdl.org) | Controllers (and audio on macOS) | zlib licence |
| [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB), the community's controller mappings, in `third_party/gamecontrollerdb` (revision f510b744) | Controllers SDL2 has no mapping of its own for | zlib licence, `third_party/gamecontrollerdb/LICENSE.txt` |
| [Roboto](https://github.com/googlefonts/roboto) Bold, © Google | The letters in the button-prompt atlases (`src/res/prompts`, drawn by `tools/prompt_atlas.py`) | Apache-2.0 |
| [minimp3](https://github.com/lieff/minimp3) by lieff | The constant tables of the MP3 decoder (`src/apu/mp3dec_tables.h`, written by `tools/gen_mp3_tables.py`) | CC0 (public domain) |
| [glslang](https://github.com/KhronosGroup/glslang), [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools), [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) (macOS builds and the Vulkan renderer) | Shader translation | BSD-3-Clause, Apache-2.0 and MIT; see each project |

Release packages carry these notices in `THIRD_PARTY_NOTICES.md`.

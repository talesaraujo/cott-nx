// Command line around MojoRecomp's ShaderTranslator: every vs_/ps_<hash>.ucode of a folder -> .spv + .meta.json.
#include "gpu/shader_translator.h"
#include <cstdio>
int main(int argc, char** argv) {
  if (argc != 3) { std::fprintf(stderr, "usage: mojo_xlate <ucode dir> <out dir>\n"); return 2; }
  return ShaderTranslator::TranslateDirectory(argv[1], argv[2]);
}

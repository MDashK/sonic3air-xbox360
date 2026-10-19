// Experiment 3: this project compiles nothing of its own. main() comes from d3dmin_ms.a, which is
// main.cpp built by the stock XDK cl.exe and translated to ELF64 by coff2elf (see build_exp3.cmd).
// The RXDK clang toolset needs at least one compiled source to run its link step.
extern "C" void d3dmin_exp3_stub()
{
}

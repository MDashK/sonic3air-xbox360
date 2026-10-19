#!/usr/bin/env python3
"""Generates the Phase 3 MSBuild projects in x360/build (Xbox 360, RXDK clang toolset).

    python -I gen_projects.py

All projects import x360.toolset.props / x360.common.props. Sources come straight from the upstream
tree via wildcards ($(S3AIRRoot)), so new upstream files are picked up automatically.
"""
import os

BUILD_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build")

TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Xbox 360">
      <Configuration>Debug</Configuration>
      <Platform>Xbox 360</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|Xbox 360">
      <Configuration>Release</Configuration>
      <Platform>Xbox 360</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{{{guid}}}</ProjectGuid>
    <Keyword>Xbox360Proj</Keyword>
    <RootNamespace>{name}</RootNamespace>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <Import Project="x360.toolset.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>{type}</ConfigurationType>{extra_props}
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <Import Project="x360.common.props" />{extra_groups}
  <ItemGroup>
{items}
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>
"""

PURE_C = """
    <!-- Pure C, no XDK API: plain clang mode (picolibc only), see zlib.vcxproj -->
    <RxdkClangXdkHeaders>false</RxdkClangXdkHeaders>"""


def compile_items(*entries):
	lines = []
	for entry in entries:
		include, exclude = (entry if isinstance(entry, tuple) else (entry, None))
		if exclude:
			lines.append('    <ClCompile Include="%s" Exclude="%s" />' % (include, exclude))
		else:
			lines.append('    <ClCompile Include="%s" />' % include)
	return "\n".join(lines)


def defines_group(defines, comment):
	return """
  <ItemDefinitionGroup>
    <ClCompile>
      <!-- %s -->
      <PreprocessorDefinitions>%s;%%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>""" % (comment, defines)


APP_GROUPS = """
  <PropertyGroup>
    <!-- The game folder: default.xex here, plus the data (see x360/README.md) -->
    <OutDir>$(X360Root)deploy\\$(Configuration)\\</OutDir>
    <X360LibDir>$(X360Root)build\\out\\$(Configuration)\\</X360LibDir>
    <RxdkClangLibs>$(X360LibDir)oxygen.lib;$(X360LibDir)oxygen_netcore.lib;$(X360LibDir)lemonscript.lib;$(X360LibDir)rmxext_oggvorbis.lib;$(X360LibDir)rmxmedia.lib;$(X360LibDir)rmxbase.lib;$(X360LibDir)oggvorbis.lib;$(X360LibDir)minizip.lib;$(X360LibDir)zlib.lib;$(X360LibDir)sdl2lite.lib;$(X360LibDir)x360runtime.lib</RxdkClangLibs>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <Link>
      <AdditionalDependencies>xapilib.lib;d3d9.lib;xaudio2.lib;x3daudio.lib;xmcore.lib;xboxkrnl.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
    <ImageXex>
      <ImageXexOutput>$(OutDir)default.xex</ImageXexOutput>
    </ImageXex>
  </ItemDefinitionGroup>"""

PROJECTS = [
	dict(name="oggvorbis", guid="7B6E0C11-3A21-4C1E-9D0B-5A1E2F0C1007", extra_props=PURE_C,
		 extra_groups=defines_group("alloca=__builtin_alloca", "libvorbis only includes alloca.h on platforms it knows"),
		 items=compile_items(r"$(S3AIRRoot)framework\external\ogg-vorbis\libogg\src\*.c",
							 (r"$(S3AIRRoot)framework\external\ogg-vorbis\libvorbis\lib\*.c",
							  r"$(S3AIRRoot)framework\external\ogg-vorbis\libvorbis\lib\barkmel.c"))),
	dict(name="minizip", guid="7B6E0C11-3A21-4C1E-9D0B-5A1E2F0C1008", extra_props=PURE_C,
		 extra_groups=defines_group("USE_FILE32API;Z_HAVE_UNISTD_H", "No 64-bit stdio API (fopen64 etc.) in picolibc; the game's files are far below 2 GB"),
		 items=compile_items(r"$(S3AIRRoot)framework\external\zlib\zlib\contrib\minizip\ioapi.c",
							 r"$(S3AIRRoot)framework\external\zlib\zlib\contrib\minizip\unzip.c")),
		# zip.c (zip writing) is not used by S3AIR, and RXDK's clang refuses one of its code patterns at -O2
		# ("OR8_rec: a record form sets CR0 from the low 32 bits of its 64-bit result on the console")
	dict(name="rmxext_oggvorbis", guid="7B6E0C11-3A21-4C1E-9D0B-5A1E2F0C1009",
		 items=compile_items(r"$(S3AIRRoot)librmx\source\rmxext_oggvorbis\*.cpp")),
	dict(name="oxygen_netcore", guid="7B6E0C11-3A21-4C1E-9D0B-5A1E2F0C100A",
		 # No networking on Xbox 360: x360/port/Sockets_x360.cpp replaces the socket implementation
		 items=compile_items((r"$(S3AIRRoot)Oxygen\oxygenengine\source\oxygen_netcore\**\*.cpp",
							  r"$(S3AIRRoot)Oxygen\oxygenengine\source\oxygen_netcore\network\Sockets.cpp"),
							 r"$(X360Root)port\Sockets_x360.cpp")),
	dict(name="oxygen", guid="7B6E0C11-3A21-4C1E-9D0B-5A1E2F0C100B",
		 items=compile_items(r"$(S3AIRRoot)Oxygen\oxygenengine\source\oxygen\**\*.cpp")),
	dict(name="sonic3air", guid="7B6E0C11-3A21-4C1E-9D0B-5A1E2F0C100C", type="Application",
		 extra_groups=APP_GROUPS,
		 items=compile_items(r"$(S3AIRRoot)Oxygen\sonic3air\source\sonic3air\**\*.cpp",
							 r"$(X360Root)port\x360_platform.cpp",
							 r"$(X360Root)port\D3D9Drawer.cpp")),
]


def main():
	for project in PROJECTS:
		text = TEMPLATE.format(name=project["name"], guid=project["guid"], type=project.get("type", "StaticLibrary"),
							   extra_props=project.get("extra_props", ""), extra_groups=project.get("extra_groups", ""),
							   items=project["items"])
		path = os.path.join(BUILD_DIR, project["name"] + ".vcxproj")
		with open(path, "w", encoding="utf-8", newline="\r\n") as f:
			f.write(text)
		print("wrote", path)


if __name__ == "__main__":
	main()

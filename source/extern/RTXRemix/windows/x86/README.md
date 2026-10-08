RTX Remix runtime for the avpremix_x86 build (CMake -DAVP_ENABLE_RTX_REMIX=ON).

Drop the contents of an NVIDIA RTX Remix *runtime* release here:
  d3d9.dll          the 32-bit bridge client
  .trex/            the 64-bit bridge server and Remix renderer
  (rtx.conf)        optional

When d3d9.dll is present, the post-build step copies this whole folder next to
avpremix_x86.exe in build/windows/x86remix/. This README is copied too, which is harmless.

NOTE: the game still renders through OpenGL, which never loads d3d9.dll, so Remix
does nothing yet. See "RTX Remix" in CLAUDE.md.

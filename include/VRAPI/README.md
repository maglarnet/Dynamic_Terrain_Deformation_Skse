# Open Shaders API headers

`CSinterface001.h` and `TerrainDepthOffset.h` are the consumer contract from the
matching Open Shaders terrain API proposal, based on its `dev` branch on
2026-10-30. Both headers are LGPL-3.0-or-later; see `COPYING.LESSER` here and the
repository's GPL license for the rest of DTD.

Source: https://github.com/alandtse/open-shaders

Request revision 5 explicitly. Do not call the appended terrain method on an
older interface or infer support from the DLL name or plugin version.

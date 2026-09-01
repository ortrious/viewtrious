# Open CASCADE Technology runtime

Viewtrious STEP/STP support uses OCCT 8.0.1 as a dynamic x64 dependency. Set
`VIEWTRIOUS_OCCT_ROOT` to its install root; the build expects `inc` and
`win64/vc14/{lib,bin}` beneath that directory.

The Viewtrious post-build step copies the required `TK*.dll` runtime closure
next to `Viewtrious.exe`. Keep `LICENSE_LGPL_21.txt` and
`OCCT_LGPL_EXCEPTION.txt` with distributed copies.

No OCCT Qt, OpenGL, or renderer DLL is used by Viewtrious. `TKV3d.dll` and
`TKVCAF.dll` are retained solely because stock OCCT XDE packages depend on
their document/presentation metadata types.

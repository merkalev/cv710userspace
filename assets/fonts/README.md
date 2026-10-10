# Bundled OSD font

`ui.ttf` is **Inter** (variable font, opsz+wght axes), Copyright © 2016-2022
The Inter Project Authors (Rasmus Andersson et al.), distributed by Google
Fonts.

Licensed under the **SIL Open Font License, Version 1.1** — the full license
text is in `OFL-1.1.txt` in this directory, with no Reserved Font Name
restriction asserted for this use.

The loader (`SdlVideoOutput::loadFont`) prefers this bundled file
(`assets/fonts/ui.ttf`), falling back to `CV710_FONT`, then a list of common
system sans faces, then the SDL debug font.
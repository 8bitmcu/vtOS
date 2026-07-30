# License and Attribution

## Primary License
**vtOS** is licensed under the **MIT License**.

© 2026 Vincent (8bitmcu)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

## Third-Party Component Licenses

### st (Suckless Terminal)
The core terminal engine is based on `st`.
**License:** MIT/X Consortium License

- © 2014-2022 Hiltjo Posthuma <hiltjo at codemadness dot org>
- © 2018 Devin J. Pohly <djpohly at gmail dot com>
- © 2014-2017 Quentin Rameau <quinq at fifth dot space>
- © 2009-2012 Aurélien APTEL <aurelien dot aptel at gmail dot com>
- © 2008-2017 Anselm R Garbe <garbeam at gmail dot com>
- © 2012-2017 Roberto E. Vargas Caballero <k0ga at shike2 dot com>
- © 2012-2016 Christoph Lohmann <20h at r-36 dot net>
- © 2013 Eon S. Jeon <esjeon at hyunmu dot am>
- © 2013 Alexander Sedov <alex0player at gmail dot com>
- © 2013 Mark Edgar <medgar123 at gmail dot com>
- © 2013-2014 Eric Pruitt <eric.pruitt at gmail dot com>
- © 2013 Michael Forney <mforney at mforney dot org>
- © 2013-2014 Markus Teich <markus dot teich at stusta dot mhn dot de>
- © 2014-2015 Laslo Hunhold <dev at frign dot de>

### vi (neatvi)
The `vi` implementation is derived from [neatvi](https://github.com/aligrudi/neatvi), vendored under `modules/modvi/neatvi/`.
* **Copyright:** 2015-2026 Ali Gholami Rudi <ali@rudi.ir>.
* **License:** **ISC**.
* **Summary:** Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

### frotz (Zmachine interpreter)
The Z-Machine interpreter is based on Frotz.
* **Copyright:** 1995-1997 Stefan Jokisch, 1998-present David Griffith and contributors.
* **License:** **GNU General Public License v2.0 (or later)**.
* **Summary:** Frotz is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

### st7789_mpy
* **Copyright:** Russ Hughes.
* **License:** **MIT License**.

### codec2
The low-bitrate speech codec used by the `c2` encode/decode utility is based on codec2.
* **Copyright:** 2010 David Rowe.
* **License:** **GNU Lesser General Public License v2.1**.
* **Summary:** Unlike GPL, LGPL permits linking/combining with differently-licensed code without relicensing the combined work, provided the LGPL-covered portion remains modifiable and its complete corresponding source is available. This repository vendors codec2's full source verbatim under `modules/codec2/vendor/` (see `modules/codec2/COPYING` for the license text in full) to satisfy that.

### mxml (Mini-XML)
The `modxml` module's XML parsing is based on Mini-XML, vendored under `modules/modxml/mxml/`.
* **Copyright:** © 2003-2025 by Michael R Sweet.
* **License:** **Apache License 2.0**, with an additional exception permitting combination with GPLv2/LGPLv2-licensed code (relevant here alongside `frotz` and `codec2`; see `modules/modxml/mxml/NOTICE` for the exception's exact terms).

### Terminus Font
* **Copyright:** (c) 2020 Dimitar Zhekov. 
* **License:** **SIL Open Font License 1.1**.

### Cozette Font
* **Copyright:** (c) 2020 Samhain <samhain@moonwit.ch> & contributors <https://github.com/the-moonwitch/Cozette/contributors>.
* **License:** **MIT License**.

### Tamzen Font
* **Copyright:** 2011 Suraj N. Kurapati <https://github.com/sunaku/tamzen-font>.
* **License:** Permissive, custom terms.
* **Summary:** Tamzen font is free. You are hereby granted permission to use, copy, modify, and distribute it as you see fit. Tamzen font is provided "as is" without any express or implied warranty. The author makes no representations about the suitability of this font for a particular purpose. In no event will the author be held liable for damages arising from the use of this font.

### Gohu Font
* **Copyright:** 2015 by Hugo Chargois.
* **License:** **[WTFPL version 2](https://www.wtfpl.net/about/)**.

### Spleen Font
* **Copyright:** (c) 2018-2026, Frédéric Cambus.
* **License:** **BSD 2-Clause License**.

### Scientifica Font
* **Copyright:** (c) 2020 Akshay Oppiliappan <nerdy@peppe.rs>.
* **License:** **SIL Open Font License 1.1**.

### GNU Unifont
* **Copyright:** Roman Czyborra, Paul Hardy, and contributors.
* **License:** Dual-licensed under the **SIL Open Font License 1.1** and **GNU GPL v2+ with the GNU Font Embedding Exception**; used here under the SIL Open Font License 1.1.

### Siji Font
The `--icons` glyph set is based on Siji.
* **Copyright:** stark and contributors (based on Stlarch; glyphs drawn from FontAwesome and other icon packs).
* **License:** **GNU General Public License v2.0**.
* **Summary:** Unlike Unifont's GPL option, Siji carries no font-embedding exception -- compiling it into the firmware creates a combined GPLv2 work, same as `frotz` (see the note at the end of this file).

### MicroPython
* **Copyright:** (c) Damien P. George.
* **License:** **MIT License**.

---

### Implementation Note for the T-Deck
While the original `vi` code (neatvi) follows the ISC license and the `st` engine follows the MIT/X Consortium license, the specific architectural modifications in this repository—including the **MicroPython VFS Bridge**, **NLR Exception Guarding**, **Zero-allocation Status Bar**, and **GC-safe Memory Management**—are contributed under the project's primary MIT license.

---

While the original source code for vtOS is provided under the MIT license, compiling it with the optional frotz module or the siji icon font creates a combined work that falls under the GNU General Public License v2.0 (GPL-2.0) upon distribution.

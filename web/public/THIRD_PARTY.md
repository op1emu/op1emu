# Third-party notices

## op1kenobi

The faceplate (`gui/ui.png`) is made from op1kenobi's
(https://github.com/alexmandelshtam/op1kenobi), and the computer keyboard's
musical keys (`keymap.js`) are laid out as it lays them out. Its license:

```
CSS & HTML License by Shunya Koide -- project inspiration and web design wizard. Check out their work yourself!

Copyright (c) 2022 by Shunya Koide (https://codepen.io/shunyadezain/pen/QWyVwep)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

MIT License

Copyright (c) 2022 Alex Ownejazayeri

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## SVG front panel

`web/panel/op1.svg` and its six palettes are the artwork and class-only theme
derivative from [op1emu/op1emu PR #77](https://github.com/op1emu/op1emu/pull/77),
at GosuBoy/op1emu revision `523eac7092c0958a2b02576d9165e7fd35e950be`.
The SVG was supplied with redistribution authorization, as recorded there.
The browser port preserves its geometry, gradients, filters and outlined labels.
The existing PNG panel's op1kenobi notices above still apply to the PNG fallback.

## npm packages

The page's npm dependencies are bundled by Vite, which lists the licenses of
those in the page itself in `licenses.md` (wasm-feature-detect). Vite does not
list those in the Worker:

### brotli-dec-wasm 2.3.2

https://github.com/ustclug-dev/brotli-dec-wasm, under the MIT license or the
Apache License 2.0, at your option. Its MIT license:

```
MIT License

Copyright (c) 2026 Yulong Ming <i@myl7.org>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Firmware preparation

The loader and XTEA transformation in `firmware-package.js` derives from
[GosuBoy/op-decryptor2](https://github.com/GosuBoy/op-decryptor2), revision
`4bad290f54174b392a8e221eeab28f7eb1abe1f5`. Synthetic known-answer values
originate in LibTomCrypt's XTEA tests. No firmware, OTP dumps or factory sounds
are included. The YAFFS2 writer is an independent format implementation, not a
copy of GPL-2.0 mkyaffs2. The LZMA decoder is npm `lzma` 2.3.2, MIT, by Nathan
Rugg; its license is included in the generated npm license inventory.

### op-decryptor2 license
MIT License

Copyright (c) 2026 op-decryptor2 contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

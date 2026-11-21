# Vendored stb_image

`stb_image.h` is stb_image v2.30 by Sean Barrett, copied from
https://github.com/nothings/stb at commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20`.

SHA-256 of the upstream `stb_image.h`:
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`

Local changes, made for CodeQL's "multiplication result converted to larger
type" alerts: buffer sizes in `stbi__convert_format16`, the PNG row copy and
the GIF frame code are worked out in `size_t`, and `stbi__load_gif_main`
fails when `layers * stride` would overflow an `int` instead of allocating a
wrapped size.

BMP fixes, marked `3dsDL:` in the file, for screenshots that came out wrong
(issue #41): pixels after a gap below the header were skipped to twice,
which started Luma3DS screenshots 10 bytes late with the colours turned
round; a 56-byte header's masks were read again from the pixels, which
refused nds-bootstrap (TWiLight Menu) screenshots with "bad offset"; and
`stbi_info` gave a top-down BMP a negative height.

Carry all of them over when updating, unless upstream has fixed them.

It is public domain, or MIT at your choice (see the end of the file). 3dsDL
uses it to show PNG, JPEG, BMP, GIF and TGA pictures in Files, and only
compiles it in `source/core/picture.cpp`.

# Experimental native-pixmap import contract

Applies only to the original and byte-verified stripped build hashes in README.md. No public API stability is implied.

The binary exports:

```c
uint32_t egl_create_pixmap_ID_mapping(void *metadata);
int egl_destroy_pixmap_ID_mapping(uint32_t id);
```

The first registers a caller-owned metadata pointer. Keep that object alive
until the imported EGLImage and GLES objects have been destroyed. The second
removes the registry entry and returns success; it does not free the caller's
metadata. The probe keeps the DMA-BUF open throughout the image's lifetime.

The traced registry object is 68 bytes, with 32-bit scalar fields:

| Offset | Field |
| --- | --- |
| 0x00 | width |
| 0x04 | height |
| 0x08 + 12*i | plane i pitch |
| 0x0c + 12*i | unused by this path; zero |
| 0x10 + 12*i | plane i byte offset |
| 0x2c | reserved; zero |
| 0x30 | internal format word |
| 0x34 | internal format flags |
| 0x38 + 4*i | plane i DMA-BUF FD |

Only plane 0 is used for these RGB formats. Unused FDs are -1. Flags are 0.

| Test | Format word | Format-table name |
| --- | --- | --- |
| linear | 0x010bb688 | EGL_COLOR_BUFFER_FORMAT_ABGR8888 |
| afbc | 0x360bb688 | EGL_COLOR_BUFFER_FORMAT_ABGR8888_AFBC |
| afbc-split | 0x760bb688 | EGL_COLOR_BUFFER_FORMAT_ABGR8888_AFBC_SPLITBLK |

The executable's actual format table supplies these names/values. These are
private Mali format words, NOT DRM modifiers and NOT Amlogic OSD format flags.

Import uses:

```c
id = egl_create_pixmap_ID_mapping(&metadata);
image = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                         EGL_NATIVE_PIXMAP_KHR,
                         (EGLClientBuffer)(uintptr_t)id,
                         { EGL_IMAGE_PRESERVED_KHR, EGL_FALSE, EGL_NONE });
```

No AFBC modifier attribute is passed. In `common_get_native_buffer` (module VA
0x0017c3b4), the native-pixmap branch looks up that registry entry, obtains its
format at +0x30, and constructs a plane descriptor. The observed first 16 words
for the probe's C-generated metadata with FD=3 are:

```
byte offsets: 0, 0, 0, 0
pitches:      512, 0, 0, 0
FDs:          3, -1, -1, 0
memory types: 2, 2, 2, 0
```

This reaches `egl_color_buffer_wrap_external_planar` (0x0044a1c8) with width and
height 128 and the AFBC format unmodified. The wrapper has an explicit AFBC
branch that computes layout/alignment and creates planes from the supplied
allocation. Retained code and emulated argument dispatch do not prove that the
hardware import, rendering or presentation is valid; that is the purpose of the
runtime tests.

The native registry exports are used directly. The public `fbdev_pixmap` header
shipped elsewhere in the package tree describes a different structure and is
not substituted for this object.

For AFBC, plane pitch is supplied for consistency but the library's AFBC branch
computes header/body layout from format/dimensions. The oversized DMA-BUF is not
an implementation of a general-purpose AFBC allocator; this fixed 128x128
experiment does not establish production allocation sizing.

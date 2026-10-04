# The DAS app links at 0x800 (the bootloader owns 0x0-0x800), but minichlink rejects a raw
# 0x800 address and wants the symbolic/aliased flash form ("flash+0x800"). The platform
# derives BOTH the linker origin and the upload address from board_upload.offset_address, so
# this post-script only rewrites the upload address, leaving the 0x800 linker origin alone.
# The address is read back from the board config so platformio.ini is the single source of
# truth (the DAS bootloader source still carries its own 0x800 constant).
Import("env")

_offset = env.BoardConfig().get("upload.offset_address", "0x800")
_offset = int(_offset, 0) if isinstance(_offset, str) else int(_offset)

env.Replace(UPLOADERPOSTFLAGS="flash+0x%X -b" % _offset)

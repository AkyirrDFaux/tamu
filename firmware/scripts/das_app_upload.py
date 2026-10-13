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


# The image must not carry the storage region. The link pins a .fixed_data reservation at
# 0x3E00 (the last 512 B of the chip flash) so a code overflow fails the link instead of
# corrupting the storage at runtime; a plain objcopy therefore emits an image padded with that
# region. Writing it would erase the device's real storage, so strip the section: the image is
# the app's code alone. The bootloader's write window still spans the whole app region.
def _strip_storage(source, target, env):
    _elf = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    _bin = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    env.Execute("$OBJCOPY -O binary -R .fixed_data %s %s" % (_elf, _bin))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _strip_storage)

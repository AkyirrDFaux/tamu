# The DAS app links at 0x800 (the bootloader owns 0x0-0x800), but minichlink rejects a raw
# 0x800 address and wants the symbolic/aliased flash form ("flash+0x800"). The platform
# derives BOTH the linker origin and the upload address from board_upload.offset_address, so
# this post-script only rewrites the upload address, leaving the 0x800 linker origin alone.
Import("env")

env.Replace(UPLOADERPOSTFLAGS="flash+0x800 -b")

/* DAS v0.1 standalone bootloader linker script.
 *
 * Bootloader at 0x0000-0x0FFF (4 KB).
 * Provides all symbols the noneos-sdk startup assembly expects.
 */

ENTRY( _start )

__stack_size = 512;

PROVIDE( _stack_size = __stack_size );

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4096
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 2K
}

SECTIONS
{
	.init :
	{
		_sinit = .;
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.init)))
		. = ALIGN(4);
		_einit = .;
	} >FLASH AT>FLASH

	/* Highcode: bootloader has no time-critical code, provide empty stubs */
	PROVIDE(_highcode_lma = 0);
	PROVIDE(_highcode_vma_start = 0);
	PROVIDE(_highcode_vma_end = 0);
	PROVIDE(_highcode_init_lma = 0);
	PROVIDE(_highcode_init_vma_start = 0);
	PROVIDE(_highcode_init_vma_end = 0);

	.text :
	{
		. = ALIGN(4);
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata*)
		*(.gnu.linkonce.t.*)
		. = ALIGN(4);
	} >FLASH AT>FLASH

	.fini :
	{
		KEEP(*(SORT_NONE(.fini)))
		. = ALIGN(4);
	} >FLASH AT>FLASH

	PROVIDE( _etext = . );
	PROVIDE( _eitcm = . );

	.dalign :
	{
		. = ALIGN(4);
		PROVIDE(_data_vma = .);
	} >RAM AT>FLASH

	.dlalign :
	{
		. = ALIGN(4);
		PROVIDE(_data_lma = .);
	} >FLASH AT>FLASH

	.data :
	{
		. = ALIGN(4);
		*(.data .data.*)
		*(.sdata .sdata.*)
		*(.srodata .srodata.*)
		. = ALIGN(4);
		PROVIDE( _edata = .);
	} >RAM AT>FLASH

	/* Provide __global_pointer$ for the startup GP init */
	. = ALIGN(8);
	PROVIDE( __global_pointer$ = . + 0x800 );

	.bss :
	{
		. = ALIGN(4);
		PROVIDE( _sbss = .);
		*(.bss*)
		*(COMMON*)
		. = ALIGN(4);
		PROVIDE( _ebss = .);
	} >RAM AT>FLASH

	PROVIDE( _end = _ebss);
	PROVIDE( end = . );

	.stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
	{
		PROVIDE( _heap_end = . );
		. = ALIGN(4);
		PROVIDE(_susrstack = . );
		. = . + __stack_size;
		PROVIDE( _eusrstack = .);
	} >RAM
}

OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS
{
	. = 0x80009000;
	__image_start = .;
	.text : ALIGN(4) { KEEP(*(.text.start)) *(.text .text.*) }
	.rodata : ALIGN(4) { *(.rodata .rodata.*) }
	.data : ALIGN(4) { *(.data .data.*) }
	.got : ALIGN(4) { *(.got .got.*) }
	/* Tabela de CRC32 por bloco de 128 B, gravada depois do link pelo
	 * tools/econet_chainloader_image.py. Fica por ultimo: tudo antes dela
	 * e o que e conferido. LONG() da conteudo a secao: so com FILL ela e
	 * NOBITS, o objcopy nao a emite e a tabela cairia sobre o .rodata.
	 */
	.imgchk : ALIGN(4) { __chk_start = .; LONG(0); . = __chk_start + 320 * 4; }
	__image_load_end = .;
	.bss (NOLOAD) : ALIGN(16) { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(16); __bss_end = .; }
	__image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.gnu.attributes) }
}

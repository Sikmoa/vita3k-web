ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
}

SECTIONS
{
    . = 0x1000;
    .text : ALIGN(4)
    {
        KEEP(*(.text.start))
        *(.text*)
    } :text
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}

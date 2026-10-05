ENTRY(__start)

SECTIONS
{
  .text 0x00001000 : { *(.text*) }
  .data 0x00002000 : { *(.data*) }
}

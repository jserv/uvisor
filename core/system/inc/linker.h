#ifndef __LINKER_H__
#define __LINKER_H__

EXTERN uint32_t __stack_end__;

EXTERN uint32_t __bss_start__;
EXTERN uint32_t __bss_end__;
EXTERN const uint32_t __bss_size__;

EXTERN uint32_t __data_start__;
EXTERN uint32_t __data_end__;
EXTERN const uint32_t __data_start_src__;
EXTERN const uint32_t __data_size__;
EXTERN uint32_t __stack_start__;
EXTERN const uint32_t __code_end__;
#endif/*__LINKER_H__*/

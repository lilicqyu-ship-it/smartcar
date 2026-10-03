/*
 * clangd shim:tc275_car 的 Compilers.h 在 __HIGHTEC__ && __clang__ 分支引用的
 * CompilerHighTec.h（HighTec LLVM 工具链版本），仓库里没有这份文件。
 *
 * 这里对齐 CompilerGnuc.h 的宏面（链接符号 / IFX_* / 段属性），内建函数走
 * 同目录 machine/cint.h shim；仅服务宿主 clang 解析（scripts/gen-tc275-cdb.py
 * 的 -I 兜底），不参与真实编译，中断宏按软件管理语义展开为普通函数。
 */
#ifndef COMPILERHIGHTEC_SHIM_H
#define COMPILERHIGHTEC_SHIM_H

#include <stddef.h>

#include <machine/cint.h>

#ifndef IFX_CFG_USE_COMPILER_DEFAULT_LINKER

#define IFXCOMPILER_COMMON_LINKER_SYMBOLS()                                   \
    extern unsigned int __A0_MEM[];     /**< center of A0 addressable area */ \
    extern unsigned int __A1_MEM[];     /**< center of A1 addressable area */ \
    extern unsigned int __A8_MEM[];     /**< center of A8 addressable area */ \
    extern unsigned int __A9_MEM[];     /**< center of A9 addressable area */

#define IFXCOMPILER_CORE_LINKER_SYMBOLS(cpu)                                    \
    extern unsigned int __USTACK##cpu[];      /**< user stack end */            \
    extern unsigned int __ISTACK##cpu[];      /**< interrupt stack end */       \
    extern unsigned int __INTTAB_CPU##cpu[];  /**< Interrupt vector table */    \
    extern unsigned int __TRAPTAB_CPU##cpu[]; /**< trap table */                \
    extern unsigned int __CSA##cpu[];         /**< context save area 1 begin */ \
    extern unsigned int __CSA##cpu##_END[];   /**< context save area 1 begin */

#define __USTACK(cpu)      __USTACK##cpu
#define __ISTACK(cpu)      __ISTACK##cpu
#define __INTTAB_CPU(cpu)  __INTTAB_CPU##cpu
#define __TRAPTAB_CPU(cpu) __TRAPTAB_CPU##cpu
#define __CSA(cpu)         __CSA##cpu
#define __CSA_END(cpu)     __CSA##cpu##_END

#if defined(IFX_USE_SW_MANAGED_INT)
#define __INTTAB(cpu) ((unsigned int)__INTTAB_CPU##cpu | (unsigned int)0x1FE0)
#else
#define __INTTAB(cpu) __INTTAB_CPU##cpu
#endif

#define __TRAPTAB(cpu) __TRAPTAB_CPU##cpu

#define __SDATA1(cpu) __A0_MEM
#define __SDATA2(cpu) __A1_MEM
#define __SDATA3(cpu) __A8_MEM
#define __SDATA4(cpu) __A9_MEM

#endif /* IFX_CFG_USE_COMPILER_DEFAULT_LINKER */

#ifndef IFX_INLINE
#define IFX_INLINE static inline __attribute__((always_inline))
#endif

#define IFX_PACKED        __attribute__((packed))
#define COMPILER_NAME     "HIGHTEC(clangd shim)"
#define COMPILER_VERSION  0
#define COMPILER_REVISION 0
#define IFX_INTERRUPT_FAST IFX_INTERRUPT

#ifndef IFX_INTERRUPT
#define IFX_INTERRUPT(isr, vectabNum, prio) void isr(void)
#endif
#ifndef IFX_INTERRUPT_INTERNAL
#define IFX_INTERRUPT_INTERNAL(isr, vectabNum, prio) IFX_EXTERN void isr(void)
#endif

#define IFX_ALIGN(n) __attribute__((aligned(n)))

#ifndef IFX_FAR_ABS
#define IFX_FAR_ABS
#endif
#ifndef IFX_NEAR_ABS
#define IFX_NEAR_ABS
#endif
#ifndef IFX_REL_A0
#define IFX_REL_A0
#endif
#ifndef IFX_REL_A1
#define IFX_REL_A1
#endif
#ifndef IFX_REL_A8
#define IFX_REL_A8
#endif
#ifndef IFX_REL_A9
#define IFX_REL_A9
#endif

#endif /* COMPILERHIGHTEC_SHIM_H */

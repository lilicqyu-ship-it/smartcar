/*
 * clangd shim:HighTec TriCore 工具链的 <machine/cint.h>（裸内建函数声明）。
 *
 * tc275_sbl 的 Ifx_TypesGnuc.h 在 __HIGHTEC__ 路径下 include 本文件，但 HighTec
 * 工具链不随仓库分发。这里只补齐宿主 clang 解析所需的声明（scripts/gen-tc275-cdb.py
 * 把本目录加进 -I 兜底），不参与真实编译，签名为解析对齐用，实现一律空转。
 */
#ifndef CLANGD_SHIM_CINT_H
#define CLANGD_SHIM_CINT_H

static inline unsigned __mfcr(unsigned addr) { (void)addr; return 0; }
static inline void __mtcr(unsigned addr, unsigned val) { (void)addr; (void)val; }
static inline void __enable(void) {}
static inline void __disable(void) {}
static inline void __dsync(void) {}
static inline void __isync(void) {}
static inline unsigned __syscall(unsigned n) { (void)n; return 0; }
static inline unsigned __extr(unsigned a, unsigned p, unsigned w) { (void)a; (void)p; (void)w; return 0; }
static inline unsigned __extru(unsigned a, unsigned p, unsigned w) { (void)a; (void)p; (void)w; return 0; }
static inline unsigned __insert(unsigned d, unsigned s, unsigned p, unsigned w) { (void)d; (void)s; (void)p; (void)w; return 0; }
static inline int __abs(int v) { return v < 0 ? -v : v; }
static inline int __absdif(int a, int b) { int r = a - b; return r < 0 ? -r : r; }

#ifndef __min
#define __min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef __max
#define __max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef __getbit
#define __getbit(addr, bit) __extru(*(addr), (bit), 1)
#endif

#endif /* CLANGD_SHIM_CINT_H */

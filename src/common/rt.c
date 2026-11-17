/* Tiny replacements for the few C-runtime helpers the compiler may call (we link without a C runtime). */
typedef unsigned int u32;
typedef unsigned long long u64;

/* __chkstk (32-bit MSVC ABI): eax = bytes to allocate; touch every 4 KB page below esp, then move esp. */
__attribute__((naked)) void _chkstk(void) {
  __asm__ volatile(
    "push %ecx\n"
    "lea 8(%esp), %ecx\n"          /* ecx = esp before the call (return address excluded) */
    "1:\n"
    "cmp $0x1000, %eax\n"
    "jb 2f\n"
    "sub $0x1000, %ecx\n"
    "test %eax, (%ecx)\n"          /* touch the page */
    "sub $0x1000, %eax\n"
    "jmp 1b\n"
    "2:\n"
    "sub %eax, %ecx\n"
    "test %eax, (%ecx)\n"
    "mov %esp, %eax\n"             /* eax -> saved ecx */
    "mov %ecx, %esp\n"
    "mov (%eax), %ecx\n"           /* restore ecx */
    "mov 4(%eax), %eax\n"          /* return address */
    "push %eax\n"
    "ret\n");
}

void *__cdecl memset(void *d, int v, unsigned n) { volatile unsigned char *a = d; while (n--) *a++ = (unsigned char)v; return d; }
void *__cdecl memcpy(void *d, const void *s, unsigned n) { volatile unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
int _fltused = 1;

/* __aullrem: unsigned 64-bit remainder (MSVC helper, args on the stack, callee pops 16 bytes).
   Only referenced by Monocypher's Argon2, which we never call - but the linker needs it. */
__attribute__((naked)) void _aullrem(void) {
  __asm__ volatile(
    "push %ebx\n push %esi\n push %edi\n push %ebp\n"
    "mov 20(%esp), %eax\n mov 24(%esp), %edx\n"     /* dividend */
    "mov 28(%esp), %ebx\n mov 32(%esp), %ecx\n"     /* divisor */
    "xor %esi, %esi\n xor %edi, %edi\n mov $64, %ebp\n"
    "1:\n shl $1, %eax\n rcl $1, %edx\n rcl $1, %esi\n rcl $1, %edi\n"
    "cmp %ecx, %edi\n jb 3f\n ja 2f\n cmp %ebx, %esi\n jb 3f\n"
    "2:\n sub %ebx, %esi\n sbb %ecx, %edi\n"
    "3:\n dec %ebp\n jnz 1b\n"
    "mov %esi, %eax\n mov %edi, %edx\n"
    "pop %ebp\n pop %edi\n pop %esi\n pop %ebx\n ret $16\n");
}

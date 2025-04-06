# qalloc

Reimplementation of the glibc memory allocator interface

The approach to replacing GNU malloc is described here: https://www.gnu.org/software/libc/manual/html_node/Replacing-malloc.html All you have to do to use it is to set LD_PRELOAD to point to the compiled library.

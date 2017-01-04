#include "stack.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <libunwind.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include "_vmprof.h"
#include <stddef.h>
#include <dlfcn.h>

static int vmp_native_traces_enabled = 0;
static int vmp_native_traces_sp_offset = -1;
static ptr_t *vmp_ranges = NULL;
static ssize_t vmp_range_count = 0;


int vmp_walk_and_record_python_stack(PyFrameObject *frame, void ** result,
                                     int max_depth)
{
    // called in signal handler
    void *ip, *sp;
    unw_cursor_t cursor;
    unw_context_t uc;
    unw_proc_info_t pip;
    unw_word_t rbx;

    unw_getcontext(&uc);
    int ret = unw_init_local(&cursor, &uc);
    if (ret < 0) {
        // could not initialize lib unwind cursor and context
        return -1;
    }

    PyFrameObject * top_most_frame = frame;
    int depth = 0;
    int step_result;
    while (depth < max_depth) {
        if (!vmp_native_enabled()) {
            if (top_most_frame == NULL) {
                break;
            }
            // TODO add line profiling
            sp = (void*)CODE_ADDR_TO_UID(top_most_frame->f_code);
            result[depth++] = sp;
            top_most_frame = top_most_frame->f_back;
            continue;
        }
        unw_get_proc_info(&cursor, &pip);

        if (unw_get_reg(&cursor, UNW_X86_64_RBX, &rbx) < 0) {
            // could not retrieve
            break;
        }

        if ((void*)pip.start_ip == cpython_vmprof_PyEval_EvalFrameEx) {
            // yes we found one stack entry of the python frames!
            if (rbx != (unw_word_t)top_most_frame) {
                // uh we are screwed! the ip indicates we are have context
                // to a PyEval_EvalFrameEx function, but when we tried to retrieve
                // the stack located py frame it has a different address than the
                // current top_most_frame
                //printf("oh no!!! %p != %p\n", compare_frame, top_most_frame);
            } else {
                //printf("yes match!!!!\n");
            }
            sp = (void*)CODE_ADDR_TO_UID(top_most_frame->f_code);
            result[depth++] = sp;
            top_most_frame = top_most_frame->f_back;
        } else if (vmp_ignore_ip((ptr_t)pip.start_ip)) {
            // this is an instruction pointer that should be ignored,
            // (that is any function name in the mapping range of
            //  cpython, but of course not extenstions in site-packages))
            //char name[64];
            //unw_get_proc_name(&cursor, name, 64, &off);
            //printf("ignore ip %p %s\n", pip.start_ip, name);
        } else {
            ip = (void*)((ptr_t)pip.start_ip | 0x1);
            // mark native routines with the first bit set,
            // this is possible because compiler align to 8 bytes.
            // TODO need to check if this is possible on other 
            // compiler than e.g. gcc/clang too?
            //
            result[depth++] = ip;
        }

        step_result = unw_step(&cursor);
        if (step_result <= 0) {
            break;
        }
    }

    return depth;
}


void *vmp_get_virtual_ip(char* sp) {
    PyFrameObject *f = *(PyFrameObject **)(sp + vmp_native_sp_offset());
    return (void *)CODE_ADDR_TO_UID(f->f_code);
}

int vmp_native_enabled(void) {
    return vmp_native_traces_enabled;
}

int vmp_native_sp_offset(void) {
    return vmp_native_traces_sp_offset;
}

void vmp_get_symbol_for_ip(void * ip, char * name, int length) {
    Dl_info info;
    // ip is off +1, does not matter for dladdr (see manpage)
    if (dladdr(ip, &info) != 0) {
        strcpy(name, "unknown symbol");
    }
    strncpy(name, info.dli_sname, length-1);
    name[length-1] = '\x00'; // null terminate just in case
}

#ifdef __unix__
int vmp_read_vmaps(const char * fname) {

    FILE * fd = fopen(fname, "rb");
    if (fd == NULL) {
        return 0;
    }
    char * saveptr;
    char * line = NULL;
    char * he = NULL;
    char * name;
    char *start_hex = NULL, *end_hex = NULL;
    size_t n = 0;
    ssize_t size;
    ptr_t start, end;

    // assumptions to be verified:
    // 1) /proc/self/maps is ordered ascending by start address
    // 2) libraries that contain the name 'python' are considered
    //    candidates in the mapping to be ignored
    // 3) libraries containing site-packages are not considered
    //    candidates

    // initially 10 (start, stop) entries!
    int max_count = 10;
    vmp_range_count = 0;
    if (vmp_ranges != NULL) { free(vmp_ranges); }
    vmp_ranges = malloc(max_count * sizeof(ptr_t));
    ptr_t * cursor = vmp_ranges;
    cursor[0] = -1;
    while ((size = getline(&line, &n, fd)) >= 0) {
        assert(line != NULL);
        start_hex = strtok_r(line, "-", &saveptr);
        if (start_hex == NULL) { continue; }
        start = strtoll(start_hex, &he, 16);
        end_hex = strtok_r(NULL, " ", &saveptr);
        if (end_hex == NULL) { continue; }
        end = strtoll(end_hex, &he, 16);
        // skip over flags, ...
        strtok_r(NULL, " ", &saveptr);
        strtok_r(NULL, " ", &saveptr);
        strtok_r(NULL, " ", &saveptr);
        strtok_r(NULL, " ", &saveptr);

        name = saveptr;
        if (strstr(name, "python") != NULL && \
            strstr(name, ".so\n") == NULL) {
            // realloc if the chunk is to small
            ptrdiff_t diff = (cursor - vmp_ranges);
            if (diff + 2 > max_count) {
                vmp_ranges = realloc(vmp_ranges, max_count*2*sizeof(ptr_t));
                max_count *= 2;
                cursor = vmp_ranges + diff;
            }

            if (cursor[0] == start) {
                cursor[0] = end;
            } else {
                if (cursor != vmp_ranges) {
                    // not pointing to the first entry
                    cursor++;
                }
                cursor[0] = start;
                cursor[1] = end;
                vmp_range_count += 2;
                cursor++;
            }
        }
        free(line);
        line = NULL;
        n = 0;
    }

    fclose(fd);
    return 1;
}
#endif

#ifdef __APPLE__
int vmp_read_vmaps(const char * fname) {
    kern_return_t kr;
    task_t task;
    return 0;
}
#endif

int vmp_native_enable(int offset) {
    vmp_native_traces_enabled = 1;
    vmp_native_traces_sp_offset = offset;

#if defined(__unix__)
    return vmp_read_vmaps("/proc/self/maps");
#elif defined(__APPLE__)
    return vmp_read_vmaps(NULL);
#endif
// TODO MAC use mach task interface to extract the same information
}

void vmp_native_disable(void) {
    vmp_native_traces_enabled = 0;
    vmp_native_traces_sp_offset = 0;
    if (vmp_ranges != NULL) {
        free(vmp_ranges);
        vmp_ranges = NULL;
    }
    vmp_range_count = 0;
}

int vmp_ignore_ip(ptr_t ip) {
    int i = vmp_binary_search_ranges(ip, vmp_ranges, vmp_range_count);
    if (i == -1) {
        return 0;
    }

    assert((i & 1) == 0 && "returned index MUST be even");

    ptr_t v = vmp_ranges[i];
    ptr_t v2 = vmp_ranges[i+1];
    return v <= ip && ip <= v2;
}

int vmp_binary_search_ranges(ptr_t ip, ptr_t * l, int count) {
    ptr_t * r = l + count;
    ptr_t * ol = l;
    ptr_t * or = r-1;
    while (1) {
        ptrdiff_t i = (r-l)/2;
        if (i == 0) {
            if (l == ol && *l > ip) {
                // at the start
                return -1;
            } else if (l == or && *l < ip) {
                // at the end
                return -1;
            } else {
                // we found the lower bound
                i = l - ol;
                if ((i & 1) == 1) {
                    return i-1;
                }
                return i;
            }
        }
        ptr_t * m = l + i;
        if (ip < *m) {
            r = m;
        } else {
            l = m;
        }
    }
    return -1;
}

int vmp_ignore_symbol_count(void) {
    return vmp_range_count;
}

ptr_t * vmp_ignore_symbols(void) {
    return vmp_ranges;
}

void vmp_set_ignore_symbols(ptr_t * symbols, int count) {
    vmp_ranges = symbols;
    vmp_range_count = count;
}
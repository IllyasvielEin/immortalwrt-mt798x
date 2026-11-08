#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IN
#define TRUE 1
#define FALSE 0
#define NDIS_STATUS_SUCCESS 0
#define MTWF_DBG(...) ((void)0)
typedef int INT;
typedef unsigned char UCHAR;
typedef char RTMP_STRING;
typedef struct { unsigned int unchanged; } RTMP_ADAPTER;
typedef RTMP_ADAPTER *PRTMP_ADAPTER;
typedef struct { struct { struct { size_t length; char *pointer; } data; } u; } RTMP_IOCTL_INPUT_STRUCT;
static unsigned int allocations, frees;
static bool allocation_error, copy_error;
static void os_alloc_mem(void *unused, UCHAR **out, size_t size)
{
 (void)unused;
 *out = allocation_error ? NULL : malloc(size);
 if (*out) allocations++;
}
static void os_free_mem(void *memory) { assert(memory); frees++; free(memory); }
static int copy_from_user(void *dest, const void *source, size_t size)
{
 if (copy_error) return 1;
 memcpy(dest, source, size);
 return 0;
}
static int rtstrcasecmp(const char *left, const char *right) { return strcasecmp(left, right) == 0; }
#include "fq.inc"
/* Registration and dispatch both come from the production AP ioctl source. */
static struct private_set_proc {
 const char *name;
 INT (*set_proc)(PRTMP_ADAPTER, RTMP_STRING *);
} *PRTMP_PRIVATE_SET_PROC, RTMP_PRIVATE_SUPPORT_PROC[] = {
#include "fq-entry.inc"
 {NULL, NULL}
};
#include "setter.inc"
static int dispatch(RTMP_ADAPTER *adapter, char *text)
{
 RTMP_IOCTL_INPUT_STRUCT input = {.u.data = {.length = strlen(text), .pointer = text}};
 return RTMPAPPrivIoctlSet(adapter, &input);
}
int main(void)
{
 RTMP_ADAPTER adapter = {.unchanged = 71};
 assert(dispatch(&adapter, "fq_en=1") == -EINVAL);
 assert(dispatch(&adapter, "fq_en=0") == -EINVAL);
 assert(dispatch(&adapter, "fq_en=1-4") == -EINVAL);
 assert(dispatch(&adapter, "FQ_EN=1") == -EINVAL);
 assert(dispatch(&adapter, "unknown=1") == -EINVAL);
 assert(adapter.unchanged == 71 && allocations == frees);
 copy_error = true;
 assert(dispatch(&adapter, "fq_en=1") == -EFAULT);
 assert(allocations == frees);
 copy_error = false;
 allocation_error = true;
 assert(dispatch(&adapter, "fq_en=1") == -ENOMEM);
 assert(allocations == frees);
 puts("actual fq_en registration/ioctl dispatcher: disabled setter reports failure, no mutation or allocation leak");
 return 0;
}

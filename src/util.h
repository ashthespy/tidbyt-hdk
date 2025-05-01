#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Simple MAX/MIN if not already defined
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifdef __cplusplus
}
#endif
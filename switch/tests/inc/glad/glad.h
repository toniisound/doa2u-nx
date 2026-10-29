/* Host stand-in for devkitPro's glad: Mesa's libGL exports every core entry point. */
#pragma once
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
static inline int gladLoadGL(void) { return 1; }

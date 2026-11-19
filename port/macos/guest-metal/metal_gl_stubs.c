/* No-op OpenGL entry points for the native Metal guest. */
#define GL_FUNCTIONS_DEFINE
#include "xgpu.h"

static const GLubyte * metal_glGetString(GLenum name)
{
	(void)name;
	return 0;
}
__typeof__(&glGetString) halo_glGetString = metal_glGetString;

static void metal_glGetIntegerv(GLenum pname, GLint *data)
{
	if (data)
		*data = 0;
}
__typeof__(&glGetIntegerv) halo_glGetIntegerv = metal_glGetIntegerv;

static void metal_glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth)
{
	(void)srcName;
	(void)srcTarget;
	(void)srcLevel;
	(void)srcX;
	(void)srcY;
	(void)srcZ;
	(void)dstName;
	(void)dstTarget;
	(void)dstLevel;
	(void)dstX;
	(void)dstY;
	(void)dstZ;
	(void)srcWidth;
	(void)srcHeight;
	(void)srcDepth;
}
__typeof__(&glCopyImageSubData) halo_glCopyImageSubData = metal_glCopyImageSubData;

static void metal_glGenerateMipmap(GLenum target)
{
	(void)target;
}
__typeof__(&glGenerateMipmap) halo_glGenerateMipmap = metal_glGenerateMipmap;

static GLenum metal_glGetError(void)
{
	return 0;
}
__typeof__(&glGetError) halo_glGetError = metal_glGetError;

static void metal_glEnable(GLenum cap)
{
	(void)cap;
}
__typeof__(&glEnable) halo_glEnable = metal_glEnable;

static void metal_glDisable(GLenum cap)
{
	(void)cap;
}
__typeof__(&glDisable) halo_glDisable = metal_glDisable;

static void metal_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}
__typeof__(&glViewport) halo_glViewport = metal_glViewport;

static void metal_glDepthRangef(GLfloat n, GLfloat f)
{
	(void)n;
	(void)f;
}
__typeof__(&glDepthRangef) halo_glDepthRangef = metal_glDepthRangef;

static void metal_glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}
__typeof__(&glScissor) halo_glScissor = metal_glScissor;

static void metal_glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
}
__typeof__(&glClearColor) halo_glClearColor = metal_glClearColor;

static void metal_glClearDepthf(GLfloat d)
{
	(void)d;
}
__typeof__(&glClearDepthf) halo_glClearDepthf = metal_glClearDepthf;

static void metal_glClearStencil(GLint s)
{
	(void)s;
}
__typeof__(&glClearStencil) halo_glClearStencil = metal_glClearStencil;

static void metal_glClear(GLbitfield mask)
{
	(void)mask;
}
__typeof__(&glClear) halo_glClear = metal_glClear;

static void metal_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
}
__typeof__(&glColorMask) halo_glColorMask = metal_glColorMask;

static void metal_glDepthMask(GLboolean flag)
{
	(void)flag;
}
__typeof__(&glDepthMask) halo_glDepthMask = metal_glDepthMask;

static void metal_glDepthFunc(GLenum func)
{
	(void)func;
}
__typeof__(&glDepthFunc) halo_glDepthFunc = metal_glDepthFunc;

static void metal_glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
	(void)func;
	(void)ref;
	(void)mask;
}
__typeof__(&glStencilFunc) halo_glStencilFunc = metal_glStencilFunc;

static void metal_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
	(void)fail;
	(void)zfail;
	(void)zpass;
}
__typeof__(&glStencilOp) halo_glStencilOp = metal_glStencilOp;

static void metal_glStencilMask(GLuint mask)
{
	(void)mask;
}
__typeof__(&glStencilMask) halo_glStencilMask = metal_glStencilMask;

static void metal_glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	(void)sfactor;
	(void)dfactor;
}
__typeof__(&glBlendFunc) halo_glBlendFunc = metal_glBlendFunc;

static void metal_glBlendEquation(GLenum mode)
{
	(void)mode;
}
__typeof__(&glBlendEquation) halo_glBlendEquation = metal_glBlendEquation;

static void metal_glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
}
__typeof__(&glBlendColor) halo_glBlendColor = metal_glBlendColor;

static void metal_glCullFace(GLenum mode)
{
	(void)mode;
}
__typeof__(&glCullFace) halo_glCullFace = metal_glCullFace;

static void metal_glFrontFace(GLenum mode)
{
	(void)mode;
}
__typeof__(&glFrontFace) halo_glFrontFace = metal_glFrontFace;

static void metal_glPolygonOffset(GLfloat factor, GLfloat units)
{
	(void)factor;
	(void)units;
}
__typeof__(&glPolygonOffset) halo_glPolygonOffset = metal_glPolygonOffset;

static void metal_glLineWidth(GLfloat width)
{
	(void)width;
}
__typeof__(&glLineWidth) halo_glLineWidth = metal_glLineWidth;

static void metal_glPixelStorei(GLenum pname, GLint param)
{
	(void)pname;
	(void)param;
}
__typeof__(&glPixelStorei) halo_glPixelStorei = metal_glPixelStorei;

static void metal_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)format;
	(void)type;
	(void)pixels;
}
__typeof__(&glReadPixels) halo_glReadPixels = metal_glReadPixels;

static void metal_glFinish(void)
{
}
__typeof__(&glFinish) halo_glFinish = metal_glFinish;

static void metal_glFlush(void)
{
}
__typeof__(&glFlush) halo_glFlush = metal_glFlush;

static void metal_glGenTextures(GLsizei n, GLuint *textures)
{
	if (textures && n > 0)
	{
		GLsizei index, limit = n > 64 ? 64 : n;
		for (index = 0; index < limit; index++)
			textures[index] = 0;
	}
}
__typeof__(&glGenTextures) halo_glGenTextures = metal_glGenTextures;

static void metal_glDeleteTextures(GLsizei n, const GLuint *textures)
{
	(void)n;
	(void)textures;
}
__typeof__(&glDeleteTextures) halo_glDeleteTextures = metal_glDeleteTextures;

static void metal_glBindTexture(GLenum target, GLuint texture)
{
	(void)target;
	(void)texture;
}
__typeof__(&glBindTexture) halo_glBindTexture = metal_glBindTexture;

static void metal_glActiveTexture(GLenum texture)
{
	(void)texture;
}
__typeof__(&glActiveTexture) halo_glActiveTexture = metal_glActiveTexture;

static void metal_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)target;
	(void)level;
	(void)internalformat;
	(void)width;
	(void)height;
	(void)border;
	(void)format;
	(void)type;
	(void)pixels;
}
__typeof__(&glTexImage2D) halo_glTexImage2D = metal_glTexImage2D;

static void metal_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)target;
	(void)level;
	(void)internalformat;
	(void)width;
	(void)height;
	(void)depth;
	(void)border;
	(void)format;
	(void)type;
	(void)pixels;
}
__typeof__(&glTexImage3D) halo_glTexImage3D = metal_glTexImage3D;

static void metal_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	(void)target;
	(void)level;
	(void)xoffset;
	(void)yoffset;
	(void)width;
	(void)height;
	(void)format;
	(void)type;
	(void)pixels;
}
__typeof__(&glTexSubImage2D) halo_glTexSubImage2D = metal_glTexSubImage2D;

static void metal_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
	(void)target;
	(void)level;
	(void)internalformat;
	(void)width;
	(void)height;
	(void)border;
	(void)imageSize;
	(void)data;
}
__typeof__(&glCompressedTexImage2D) halo_glCompressedTexImage2D = metal_glCompressedTexImage2D;

static void metal_glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data)
{
	(void)target;
	(void)level;
	(void)internalformat;
	(void)width;
	(void)height;
	(void)depth;
	(void)border;
	(void)imageSize;
	(void)data;
}
__typeof__(&glCompressedTexImage3D) halo_glCompressedTexImage3D = metal_glCompressedTexImage3D;

static void metal_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	(void)target;
	(void)pname;
	(void)param;
}
__typeof__(&glTexParameteri) halo_glTexParameteri = metal_glTexParameteri;

static void metal_glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
	(void)target;
	(void)pname;
	(void)params;
}
__typeof__(&glTexParameteriv) halo_glTexParameteriv = metal_glTexParameteriv;

static void metal_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
	(void)target;
	(void)pname;
	(void)param;
}
__typeof__(&glTexParameterf) halo_glTexParameterf = metal_glTexParameterf;

static void metal_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
	(void)target;
	(void)pname;
	(void)params;
}
__typeof__(&glTexParameterfv) halo_glTexParameterfv = metal_glTexParameterfv;

static void metal_glGenSamplers(GLsizei count, GLuint *samplers)
{
	if (samplers && count > 0)
	{
		GLsizei index, limit = count > 64 ? 64 : count;
		for (index = 0; index < limit; index++)
			samplers[index] = 0;
	}
}
__typeof__(&glGenSamplers) halo_glGenSamplers = metal_glGenSamplers;

static void metal_glBindSampler(GLuint unit, GLuint sampler)
{
	(void)unit;
	(void)sampler;
}
__typeof__(&glBindSampler) halo_glBindSampler = metal_glBindSampler;

static void metal_glSamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
	(void)sampler;
	(void)pname;
	(void)param;
}
__typeof__(&glSamplerParameteri) halo_glSamplerParameteri = metal_glSamplerParameteri;

static void metal_glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param)
{
	(void)sampler;
	(void)pname;
	(void)param;
}
__typeof__(&glSamplerParameterf) halo_glSamplerParameterf = metal_glSamplerParameterf;

static void metal_glSamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *param)
{
	(void)sampler;
	(void)pname;
	(void)param;
}
__typeof__(&glSamplerParameterfv) halo_glSamplerParameterfv = metal_glSamplerParameterfv;

static void metal_glGenFramebuffers(GLsizei n, GLuint *framebuffers)
{
	if (framebuffers && n > 0)
	{
		GLsizei index, limit = n > 64 ? 64 : n;
		for (index = 0; index < limit; index++)
			framebuffers[index] = 0;
	}
}
__typeof__(&glGenFramebuffers) halo_glGenFramebuffers = metal_glGenFramebuffers;

static void metal_glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
	(void)n;
	(void)framebuffers;
}
__typeof__(&glDeleteFramebuffers) halo_glDeleteFramebuffers = metal_glDeleteFramebuffers;

static void metal_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
	(void)target;
	(void)framebuffer;
}
__typeof__(&glBindFramebuffer) halo_glBindFramebuffer = metal_glBindFramebuffer;

static void metal_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	(void)target;
	(void)attachment;
	(void)textarget;
	(void)texture;
	(void)level;
}
__typeof__(&glFramebufferTexture2D) halo_glFramebufferTexture2D = metal_glFramebufferTexture2D;

static GLenum metal_glCheckFramebufferStatus(GLenum target)
{
	(void)target;
	return 0;
}
__typeof__(&glCheckFramebufferStatus) halo_glCheckFramebufferStatus = metal_glCheckFramebufferStatus;

static void metal_glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)
{
	(void)srcX0;
	(void)srcY0;
	(void)srcX1;
	(void)srcY1;
	(void)dstX0;
	(void)dstY0;
	(void)dstX1;
	(void)dstY1;
	(void)mask;
	(void)filter;
}
__typeof__(&glBlitFramebuffer) halo_glBlitFramebuffer = metal_glBlitFramebuffer;

static void metal_glDrawBuffers(GLsizei n, const GLenum *bufs)
{
	(void)n;
	(void)bufs;
}
__typeof__(&glDrawBuffers) halo_glDrawBuffers = metal_glDrawBuffers;

static void metal_glGenRenderbuffers(GLsizei n, GLuint *renderbuffers)
{
	if (renderbuffers && n > 0)
	{
		GLsizei index, limit = n > 64 ? 64 : n;
		for (index = 0; index < limit; index++)
			renderbuffers[index] = 0;
	}
}
__typeof__(&glGenRenderbuffers) halo_glGenRenderbuffers = metal_glGenRenderbuffers;

static void metal_glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers)
{
	(void)n;
	(void)renderbuffers;
}
__typeof__(&glDeleteRenderbuffers) halo_glDeleteRenderbuffers = metal_glDeleteRenderbuffers;

static void metal_glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
	(void)target;
	(void)renderbuffer;
}
__typeof__(&glBindRenderbuffer) halo_glBindRenderbuffer = metal_glBindRenderbuffer;

static void metal_glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
{
	(void)target;
	(void)samples;
	(void)internalformat;
	(void)width;
	(void)height;
}
__typeof__(&glRenderbufferStorageMultisample) halo_glRenderbufferStorageMultisample = metal_glRenderbufferStorageMultisample;

static void metal_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
	(void)target;
	(void)attachment;
	(void)renderbuffertarget;
	(void)renderbuffer;
}
__typeof__(&glFramebufferRenderbuffer) halo_glFramebufferRenderbuffer = metal_glFramebufferRenderbuffer;

static void metal_glReadBuffer(GLenum src)
{
	(void)src;
}
__typeof__(&glReadBuffer) halo_glReadBuffer = metal_glReadBuffer;

static void metal_glInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments)
{
	(void)target;
	(void)numAttachments;
	(void)attachments;
}
__typeof__(&glInvalidateFramebuffer) halo_glInvalidateFramebuffer = metal_glInvalidateFramebuffer;

static void metal_glGenBuffers(GLsizei n, GLuint *buffers)
{
	if (buffers && n > 0)
	{
		GLsizei index, limit = n > 64 ? 64 : n;
		for (index = 0; index < limit; index++)
			buffers[index] = 0;
	}
}
__typeof__(&glGenBuffers) halo_glGenBuffers = metal_glGenBuffers;

static void metal_glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
	(void)n;
	(void)buffers;
}
__typeof__(&glDeleteBuffers) halo_glDeleteBuffers = metal_glDeleteBuffers;

static void metal_glBindBuffer(GLenum target, GLuint buffer)
{
	(void)target;
	(void)buffer;
}
__typeof__(&glBindBuffer) halo_glBindBuffer = metal_glBindBuffer;

static void metal_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	(void)target;
	(void)size;
	(void)data;
	(void)usage;
}
__typeof__(&glBufferData) halo_glBufferData = metal_glBufferData;

static void metal_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	(void)target;
	(void)offset;
	(void)size;
	(void)data;
}
__typeof__(&glBufferSubData) halo_glBufferSubData = metal_glBufferSubData;

static void metal_glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
{
	(void)readTarget;
	(void)writeTarget;
	(void)readOffset;
	(void)writeOffset;
	(void)size;
}
__typeof__(&glCopyBufferSubData) halo_glCopyBufferSubData = metal_glCopyBufferSubData;

static void metal_glMemoryBarrier(GLbitfield barriers)
{
	(void)barriers;
}
__typeof__(&glMemoryBarrier) halo_glMemoryBarrier = metal_glMemoryBarrier;

static void metal_glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
	(void)target;
	(void)index;
	(void)buffer;
}
__typeof__(&glBindBufferBase) halo_glBindBufferBase = metal_glBindBufferBase;

static void metal_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	(void)target;
	(void)index;
	(void)buffer;
	(void)offset;
	(void)size;
}
__typeof__(&glBindBufferRange) halo_glBindBufferRange = metal_glBindBufferRange;

static void metal_glGenVertexArrays(GLsizei n, GLuint *arrays)
{
	if (arrays && n > 0)
	{
		GLsizei index, limit = n > 64 ? 64 : n;
		for (index = 0; index < limit; index++)
			arrays[index] = 0;
	}
}
__typeof__(&glGenVertexArrays) halo_glGenVertexArrays = metal_glGenVertexArrays;

static void metal_glBindVertexArray(GLuint array)
{
	(void)array;
}
__typeof__(&glBindVertexArray) halo_glBindVertexArray = metal_glBindVertexArray;

static void metal_glEnableVertexAttribArray(GLuint index)
{
	(void)index;
}
__typeof__(&glEnableVertexAttribArray) halo_glEnableVertexAttribArray = metal_glEnableVertexAttribArray;

static void metal_glDisableVertexAttribArray(GLuint index)
{
	(void)index;
}
__typeof__(&glDisableVertexAttribArray) halo_glDisableVertexAttribArray = metal_glDisableVertexAttribArray;

static void metal_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)
{
	(void)index;
	(void)size;
	(void)type;
	(void)normalized;
	(void)stride;
	(void)pointer;
}
__typeof__(&glVertexAttribPointer) halo_glVertexAttribPointer = metal_glVertexAttribPointer;

static void metal_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	(void)index;
	(void)size;
	(void)type;
	(void)stride;
	(void)pointer;
}
__typeof__(&glVertexAttribIPointer) halo_glVertexAttribIPointer = metal_glVertexAttribIPointer;

static void metal_glVertexAttrib4fv(GLuint index, const GLfloat *v)
{
	(void)index;
	(void)v;
}
__typeof__(&glVertexAttrib4fv) halo_glVertexAttrib4fv = metal_glVertexAttrib4fv;

static void metal_glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w)
{
	(void)index;
	(void)x;
	(void)y;
	(void)z;
	(void)w;
}
__typeof__(&glVertexAttribI4ui) halo_glVertexAttribI4ui = metal_glVertexAttribI4ui;

static void metal_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	(void)mode;
	(void)first;
	(void)count;
}
__typeof__(&glDrawArrays) halo_glDrawArrays = metal_glDrawArrays;

static void metal_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	(void)mode;
	(void)count;
	(void)type;
	(void)indices;
}
__typeof__(&glDrawElements) halo_glDrawElements = metal_glDrawElements;

static void metal_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
	(void)mode;
	(void)count;
	(void)type;
	(void)indices;
	(void)basevertex;
}
__typeof__(&glDrawElementsBaseVertex) halo_glDrawElementsBaseVertex = metal_glDrawElementsBaseVertex;

static GLuint metal_glCreateShader(GLenum type)
{
	(void)type;
	return 0;
}
__typeof__(&glCreateShader) halo_glCreateShader = metal_glCreateShader;

static void metal_glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length)
{
	(void)shader;
	(void)count;
	(void)string;
	(void)length;
}
__typeof__(&glShaderSource) halo_glShaderSource = metal_glShaderSource;

static void metal_glCompileShader(GLuint shader)
{
	(void)shader;
}
__typeof__(&glCompileShader) halo_glCompileShader = metal_glCompileShader;

static void metal_glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
	(void)shader;
	(void)pname;
	(void)params;
}
__typeof__(&glGetShaderiv) halo_glGetShaderiv = metal_glGetShaderiv;

static void metal_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
	(void)shader;
	(void)bufSize;
	(void)length;
	(void)infoLog;
}
__typeof__(&glGetShaderInfoLog) halo_glGetShaderInfoLog = metal_glGetShaderInfoLog;

static void metal_glDeleteShader(GLuint shader)
{
	(void)shader;
}
__typeof__(&glDeleteShader) halo_glDeleteShader = metal_glDeleteShader;

static GLuint metal_glCreateProgram(void)
{
	return 0;
}
__typeof__(&glCreateProgram) halo_glCreateProgram = metal_glCreateProgram;

static void metal_glAttachShader(GLuint program, GLuint shader)
{
	(void)program;
	(void)shader;
}
__typeof__(&glAttachShader) halo_glAttachShader = metal_glAttachShader;

static void metal_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
	(void)program;
	(void)index;
	(void)name;
}
__typeof__(&glBindAttribLocation) halo_glBindAttribLocation = metal_glBindAttribLocation;

static void metal_glLinkProgram(GLuint program)
{
	(void)program;
}
__typeof__(&glLinkProgram) halo_glLinkProgram = metal_glLinkProgram;

static void metal_glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
	(void)program;
	(void)pname;
	(void)params;
}
__typeof__(&glGetProgramiv) halo_glGetProgramiv = metal_glGetProgramiv;

static void metal_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
	(void)program;
	(void)bufSize;
	(void)length;
	(void)infoLog;
}
__typeof__(&glGetProgramInfoLog) halo_glGetProgramInfoLog = metal_glGetProgramInfoLog;

static void metal_glUseProgram(GLuint program)
{
	(void)program;
}
__typeof__(&glUseProgram) halo_glUseProgram = metal_glUseProgram;

static GLint metal_glGetUniformLocation(GLuint program, const GLchar *name)
{
	(void)program;
	(void)name;
	return 0;
}
__typeof__(&glGetUniformLocation) halo_glGetUniformLocation = metal_glGetUniformLocation;

static void metal_glUniform1i(GLint location, GLint v0)
{
	(void)location;
	(void)v0;
}
__typeof__(&glUniform1i) halo_glUniform1i = metal_glUniform1i;

static void metal_glUniform1iv(GLint location, GLsizei count, const GLint *value)
{
	(void)location;
	(void)count;
	(void)value;
}
__typeof__(&glUniform1iv) halo_glUniform1iv = metal_glUniform1iv;

static void metal_glUniform1f(GLint location, GLfloat v0)
{
	(void)location;
	(void)v0;
}
__typeof__(&glUniform1f) halo_glUniform1f = metal_glUniform1f;

static void metal_glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
{
	(void)location;
	(void)count;
	(void)value;
}
__typeof__(&glUniform4fv) halo_glUniform4fv = metal_glUniform4fv;

static void metal_glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
	(void)location;
	(void)v0;
	(void)v1;
}
__typeof__(&glUniform2f) halo_glUniform2f = metal_glUniform2f;

static void metal_glGenQueries(GLsizei n, GLuint *ids)
{
	if (ids && n > 0)
	{
		GLsizei index, limit = n > 64 ? 64 : n;
		for (index = 0; index < limit; index++)
			ids[index] = 0;
	}
}
__typeof__(&glGenQueries) halo_glGenQueries = metal_glGenQueries;

static void metal_glBeginQuery(GLenum target, GLuint id)
{
	(void)target;
	(void)id;
}
__typeof__(&glBeginQuery) halo_glBeginQuery = metal_glBeginQuery;

static void metal_glEndQuery(GLenum target)
{
	(void)target;
}
__typeof__(&glEndQuery) halo_glEndQuery = metal_glEndQuery;

static void metal_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params)
{
	(void)id;
	(void)pname;
	(void)params;
}
__typeof__(&glGetQueryObjectuiv) halo_glGetQueryObjectuiv = metal_glGetQueryObjectuiv;

static void metal_glGetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei count, GLint *params)
{
	(void)target;
	(void)internalformat;
	(void)pname;
	(void)count;
	(void)params;
}
__typeof__(&glGetInternalformativ) halo_glGetInternalformativ = metal_glGetInternalformativ;

static void metal_glDrawElementsBaseVertexEXT(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
	(void)mode;
	(void)count;
	(void)type;
	(void)indices;
	(void)basevertex;
}
__typeof__(&glDrawElementsBaseVertexEXT) halo_glDrawElementsBaseVertexEXT = metal_glDrawElementsBaseVertexEXT;

static void metal_glDrawElementsBaseVertexOES(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
	(void)mode;
	(void)count;
	(void)type;
	(void)indices;
	(void)basevertex;
}
__typeof__(&glDrawElementsBaseVertexOES) halo_glDrawElementsBaseVertexOES = metal_glDrawElementsBaseVertexOES;

int xgpu_gl_ready(void) { return 0; }
void xgpu_gl_state_invalidate(void) {}
void xgpu_gl_texture_deleted(unsigned int texture) { (void)texture; }
GLuint xgpu_compile_shader(GLenum type, const char *code, const char *what)
{ (void)type; (void)code; (void)what; return 0; }
GLuint xgpu_link_program(GLuint vertex_shader, GLuint fragment_shader, const char *what)
{ (void)vertex_shader; (void)fragment_shader; (void)what; return 0; }
void xgpu_mcc_texture_unbind(DWORD const *resource) { (void)resource; }

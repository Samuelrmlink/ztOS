/* The GPU and Wayland, as stubs that count: included after core/zrnmetrocore.c by
 * the tests that run the real core with nothing under it. */
/* ---- the GPU: only counts ---------------------------------------------------- */
#define MAX_TEX 8192
static size_t tex_bytes[MAX_TEX];
static GLuint tex_next = 1, tex_bound;
static size_t gl_live, gl_uploaded;
static unsigned gl_tex_live, gl_draws;

void glGenTextures(GLsizei n, GLuint *t) {
	for (int i = 0; i < n; i++) {
		t[i] = tex_next++ % MAX_TEX;
		tex_bytes[t[i]] = 0;
		gl_tex_live++;
	}
}
void glDeleteTextures(GLsizei n, const GLuint *t) {
	for (int i = 0; i < n; i++) {
		gl_live -= tex_bytes[t[i]];
		tex_bytes[t[i]] = 0;
		gl_tex_live--;
	}
}
void glBindTexture(GLenum target, GLuint t) { tex_bound = t; }
void glTexImage2D(GLenum a, GLint b, GLint c, GLsizei w, GLsizei h, GLint d, GLenum e, GLenum f, const void *g) {
	gl_live -= tex_bytes[tex_bound];
	tex_bytes[tex_bound] = (size_t)w * h * 4;
	gl_live += tex_bytes[tex_bound];
}
void glTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei w, GLsizei h, GLenum e, GLenum f, const void *g) {
	gl_uploaded += (size_t)w * h * 4;
}
void glTexParameteri(GLenum a, GLenum b, GLint c) {}
void glPixelStorei(GLenum a, GLint b) {}
GLenum glGetError(void) { return 0; }
void glGetIntegerv(GLenum a, GLint *b) { *b = 4096; }
const GLubyte *glGetString(GLenum a) { return (const GLubyte *)"counting stub"; }
GLuint glCreateShader(GLenum a) { return 1; }
void glShaderSource(GLuint a, GLsizei b, const GLchar *const *c, const GLint *d) {}
void glCompileShader(GLuint a) {}
void glGetShaderiv(GLuint a, GLenum b, GLint *c) { *c = 1; }
void glGetShaderInfoLog(GLuint a, GLsizei b, GLsizei *c, GLchar *d) {}
void glGetProgramInfoLog(GLuint a, GLsizei b, GLsizei *c, GLchar *d) {}
GLuint glCreateProgram(void) { return 1; }
void glAttachShader(GLuint a, GLuint b) {}
void glBindAttribLocation(GLuint a, GLuint b, const GLchar *c) {}
void glLinkProgram(GLuint a) {}
void glGetProgramiv(GLuint a, GLenum b, GLint *c) { *c = 1; }
void glDeleteShader(GLuint a) {}
void glDeleteProgram(GLuint a) {}
GLint glGetUniformLocation(GLuint a, const GLchar *b) { return 0; }
void glActiveTexture(GLenum a) {}
void glBindBuffer(GLenum a, GLuint b) {}
void glBlendFunc(GLenum a, GLenum b) {}
void glClear(GLbitfield a) {}
void glClearColor(GLfloat a, GLfloat b, GLfloat c, GLfloat d) {}
void glDisable(GLenum a) {}
void glEnable(GLenum a) {}
void glDrawArrays(GLenum a, GLint b, GLsizei c) { gl_draws++; }
void glEnableVertexAttribArray(GLuint a) {}
void glScissor(GLint a, GLint b, GLsizei c, GLsizei d) {}
void glUniform1f(GLint a, GLfloat b) {}
void glUniform1i(GLint a, GLint b) {}
void glUniform2f(GLint a, GLfloat b, GLfloat c) {}
void glUniform4f(GLint a, GLfloat b, GLfloat c, GLfloat d, GLfloat e) {}
void glUseProgram(GLuint a) {}
void glVertexAttribPointer(GLuint a, GLint b, GLenum c, GLboolean d, GLsizei e, const void *f) {}
void glViewport(GLint a, GLint b, GLsizei c, GLsizei d) {}
/* (retro/dotmatrix.c's, which a Metro page never reaches) */
void glGenBuffers(GLsizei a, GLuint *b) {}
void glBufferData(GLenum a, GLsizeiptr b, const void *c, GLenum d) {}
void glBufferSubData(GLenum a, GLintptr b, GLsizeiptr c, const void *d) {}
void glDeleteBuffers(GLsizei a, const GLuint *b) {}
GLint glGetAttribLocation(GLuint a, const GLchar *b) { return 0; }
void glUniform3f(GLint a, GLfloat b, GLfloat c, GLfloat d) {}
void glUniform3fv(GLint a, GLsizei b, const GLfloat *c) {}
void glDisableVertexAttribArray(GLuint a) {}

static char fake[64];
EGLDisplay eglGetDisplay(EGLNativeDisplayType a) { return fake; }
EGLBoolean eglInitialize(EGLDisplay a, EGLint *b, EGLint *c) { return 1; }
EGLBoolean eglBindAPI(EGLenum a) { return 1; }
EGLBoolean eglChooseConfig(EGLDisplay a, const EGLint *b, EGLConfig *c, EGLint d, EGLint *e) { *e = 1; return 1; }
EGLContext eglCreateContext(EGLDisplay a, EGLConfig b, EGLContext c, const EGLint *d) { return fake; }
EGLSurface eglCreateWindowSurface(EGLDisplay a, EGLConfig b, EGLNativeWindowType c, const EGLint *d) { return fake; }
EGLBoolean eglMakeCurrent(EGLDisplay a, EGLSurface b, EGLSurface c, EGLContext d) { return 1; }
EGLBoolean eglSwapInterval(EGLDisplay a, EGLint b) { return 1; }
static unsigned frames;
EGLBoolean eglSwapBuffers(EGLDisplay a, EGLSurface b) { frames++; return 1; }
EGLBoolean eglDestroyContext(EGLDisplay a, EGLContext b) { return 1; }
EGLBoolean eglDestroySurface(EGLDisplay a, EGLSurface b) { return 1; }
EGLBoolean eglTerminate(EGLDisplay a) { return 1; }
EGLint eglGetError(void) { return EGL_SUCCESS; }
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *a) { return NULL; }
struct wl_egl_window *wl_egl_window_create(struct wl_surface *a, int b, int c) { return (void *)fake; }
void wl_egl_window_destroy(struct wl_egl_window *a) {}
void wl_egl_window_resize(struct wl_egl_window *a, int b, int c, int d, int e) {}

/* ---- Wayland: nothing there, and the script runs from its event dispatch ------ */
#define IFACE(n) const struct wl_interface n##_interface = { .name = #n }
IFACE(wl_compositor); IFACE(wl_shm); IFACE(wl_subcompositor); IFACE(wl_seat); IFACE(wl_surface);
IFACE(wl_callback); IFACE(wl_registry); IFACE(wl_region); IFACE(wl_buffer); IFACE(wl_shm_pool);
IFACE(wl_subsurface); IFACE(wl_touch); IFACE(wl_keyboard); IFACE(wl_output); IFACE(wl_pointer);

struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *p, uint32_t op, const struct wl_interface *i, uint32_t v,
		uint32_t f, ...) { return (struct wl_proxy *)fake; }
int wl_proxy_add_listener(struct wl_proxy *p, void (**l)(void), void *d) { return 0; }
void wl_proxy_destroy(struct wl_proxy *p) {}
uint32_t wl_proxy_get_version(struct wl_proxy *p) { return 9; }
static int display_fd = -1;
struct wl_display *wl_display_connect(const char *a) {
	display_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	struct itimerspec soon = { .it_value = { 0, 1000000 } };
	timerfd_settime(display_fd, 0, &soon, NULL);
	return (struct wl_display *)fake;
}
void wl_display_disconnect(struct wl_display *a) {}
int wl_display_roundtrip(struct wl_display *a) {
	registry_global(NULL, NULL, 1, "wl_compositor", 4);
	registry_global(NULL, NULL, 2, "wl_shm", 1);
	registry_global(NULL, NULL, 3, "xdg_wm_base", 1);
	return 0;
}
int wl_display_get_fd(struct wl_display *a) { return display_fd; }
int wl_display_flush(struct wl_display *a) { return 0; }
int wl_display_prepare_read(struct wl_display *a) { return 0; }
void wl_display_cancel_read(struct wl_display *a) {}
int wl_display_read_events(struct wl_display *a) {
	uint64_t n;
	if (read(display_fd, &n, sizeof(n)) < 0) {}
	return 0;
}


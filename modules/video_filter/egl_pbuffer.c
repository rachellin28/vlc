/*****************************************************************************
 * egl_pbuffer.c: OpenGL filter in EGL offscreen framebuffer
 *****************************************************************************
 * Copyright (C) 2020 Videolabs
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_modules.h>
#include <vlc_picture.h>
#include <vlc_filter.h>
#include <vlc_opengl.h>
#include <vlc_vout_display.h>
#include <vlc_atomic.h>
#include "../video_output/opengl/vout_helper.h"
#include "../video_output/opengl/filters.h"
#include "../video_output/opengl/gl_api.h"
#include "../video_output/opengl/gl_common.h"
#include "../video_output/opengl/interop.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define BUFFER_COUNT 4

struct pbo_picture_context
{
    struct picture_context_t context;
    void *buffer_mapping;
    int rc;
    vlc_mutex_t *lock;
    vlc_cond_t *cond;
};

struct vlc_gl_pbuffer
{
    vlc_gl_t                *gl;
    vlc_mutex_t             lock;
    vlc_cond_t              cond;

    video_format_t          fmt_out;

    struct vlc_gl_api api;

    size_t                  current_flip;
    GLuint                  pixelbuffers[BUFFER_COUNT];
    GLuint                  framebuffers[BUFFER_COUNT];
    GLuint                  textures[BUFFER_COUNT];
    struct pbo_picture_context     picture_contexts[BUFFER_COUNT];

    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;

    PFNEGLCREATEIMAGEKHRPROC    eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC   eglDestroyImageKHR;

    bool current;
};

static int MakeCurrent (vlc_gl_t *gl)
{
    struct vlc_gl_pbuffer *sys = gl->sys;

    assert(!sys->current);
    if (eglMakeCurrent (sys->display, sys->surface, sys->surface,
                        sys->context) != EGL_TRUE)
        return VLC_EGENERIC;

    sys->current = true;
    return VLC_SUCCESS;
}

static void ReleaseCurrent (vlc_gl_t *gl)
{
    struct vlc_gl_pbuffer *sys = gl->sys;

    assert(sys->current);
    eglMakeCurrent (sys->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                    EGL_NO_CONTEXT);

    sys->current = false;
}

static void *GetSymbol(vlc_gl_t *gl, const char *procname)
{
    (void) gl;
    return (void *)eglGetProcAddress (procname);
}

static const char *QueryString(vlc_gl_t *gl, int32_t name)
{
    struct vlc_gl_pbuffer *sys = gl->sys;

    return eglQueryString(sys->display, name);
}

static void *CreateImageKHR(vlc_gl_t *gl, unsigned target, void *buffer,
                            const int32_t *attrib_list)
{
    struct vlc_gl_pbuffer *sys = gl->sys;

    return sys->eglCreateImageKHR(sys->display, NULL, target, buffer,
                                  attrib_list);
}

static bool DestroyImageKHR(vlc_gl_t *gl, void *image)
{
    struct vlc_gl_pbuffer *sys = gl->sys;

    return sys->eglDestroyImageKHR(sys->display, image);
}

static int InitEGL(vlc_gl_t *gl, unsigned width, unsigned height)
{
    struct vlc_gl_pbuffer *sys = gl->sys;

    sys->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (sys->display == EGL_NO_DISPLAY)
        return VLC_EGENERIC;

    /* Initialize EGL display */
    EGLint major, minor;
    if (eglInitialize(sys->display, &major, &minor) != EGL_TRUE)
        return VLC_EGENERIC;
    msg_Dbg(gl, "EGL version %s by %s, API %s",
            eglQueryString(sys->display, EGL_VERSION),
            eglQueryString(sys->display, EGL_VENDOR),
#ifdef USE_OPENGL_ES2
            "OpenGL ES2"
#else
            "OpenGL"
#endif
            );

    const EGLint conf_attr[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
#ifdef USE_OPENGL_ES2
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
#else
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
#endif
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE,
    };
    EGLConfig cfgv[1];
    EGLint cfgc;

    msg_Info(gl, "WIDTH=%u HEIGHT=%u", width, height);
    const EGLint surface_attr[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_NONE,
    };

    if (eglChooseConfig(sys->display, conf_attr, cfgv, 1, &cfgc) != EGL_TRUE
     || cfgc == 0)
    {
        msg_Err (gl, "cannot choose EGL configuration");
        goto error;
    }

    /* Create a drawing surface */
    sys->surface = eglCreatePbufferSurface(sys->display, cfgv[0], surface_attr);
    if (sys->surface == EGL_NO_SURFACE)
    {
        msg_Err (gl, "cannot create EGL window surface");
        assert(false);
        goto error;
    }

#ifdef USE_OPENGL_ES2
    if (eglBindAPI (EGL_OPENGL_ES_API) != EGL_TRUE)
    {
        msg_Err (gl, "cannot bind EGL OPENGL ES API");
        goto error;
    }

    const GLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
#else
    if (eglBindAPI (EGL_OPENGL_API) != EGL_TRUE)
    {
        msg_Err (gl, "cannot bind EGL OPENGL API");
        goto error;
    }

    const GLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
#endif

    EGLContext ctx
        = sys->context
        = eglCreateContext(sys->display, cfgv[0], EGL_NO_CONTEXT, ctx_attr);

    if (ctx == EGL_NO_CONTEXT)
    {
        msg_Err (gl, "cannot create EGL context");
        goto error;
    }

    return VLC_SUCCESS;
error:
    eglTerminate(sys->display);
    return VLC_EGENERIC;
}

static picture_context_t *picture_context_copy(picture_context_t *input)
{
    struct pbo_picture_context *context =
        (struct pbo_picture_context *)input;

    vlc_mutex_lock(context->lock);
    context->rc++;
    vlc_mutex_unlock(context->lock);
    return input;
}

static void picture_context_destroy(picture_context_t *input)
{
    struct pbo_picture_context *context = (struct pbo_picture_context *) input;

    vlc_mutex_lock(context->lock);
    context->rc--;
    vlc_cond_signal(context->cond);
    vlc_mutex_unlock(context->lock);
}

static inline void BindDrawFramebuffer(struct vlc_gl_pbuffer *sys)
{
    const opengl_vtable_t *vt = &sys->api.vt;
    vt->BindFramebuffer(GL_DRAW_FRAMEBUFFER,
                        sys->framebuffers[sys->current_flip]);
}

static void UpdateBuffer(vlc_gl_t *gl)
{
    struct vlc_gl_pbuffer *sys = gl->sys;

    vlc_mutex_lock(&sys->lock);
    size_t index;

    do {
        for (index = 0; index < BUFFER_COUNT; ++index)
        {
            assert(sys->picture_contexts[index].rc >= 0);
            if (sys->picture_contexts[index].rc == 0)
                goto out_loop;
        }
        vlc_cond_wait(&sys->cond, &sys->lock);
    } while (index == BUFFER_COUNT);
out_loop:
    vlc_mutex_unlock(&sys->lock);

    sys->current_flip = index;
    BindDrawFramebuffer(sys);
}

static picture_t *Swap(vlc_gl_t *gl)
{
    struct vlc_gl_pbuffer *sys = gl->sys;
    const opengl_vtable_t *vt = &sys->api.vt;

    if (!sys->current)
        eglMakeCurrent(sys->display, sys->surface, sys->surface, sys->context);

    /* Read current framebuffer */
    struct pbo_picture_context *context =
        &sys->picture_contexts[sys->current_flip];

    vt->BindBuffer(GL_PIXEL_PACK_BUFFER, sys->pixelbuffers[sys->current_flip]);
    vt->BindFramebuffer(GL_FRAMEBUFFER, sys->framebuffers[sys->current_flip]);
    if (context->buffer_mapping != NULL)
        vt->UnmapBuffer(GL_PIXEL_PACK_BUFFER);

    GLsizei width = sys->fmt_out.i_visible_width;
    GLsizei height = sys->fmt_out.i_visible_height;
    GLenum format = GL_RGBA;

    vt->ReadPixels(0, 0, width, height, format, GL_UNSIGNED_BYTE, 0);

    void *pixels = vt->MapBufferRange(
            GL_PIXEL_PACK_BUFFER, 0, width*height*4, GL_MAP_READ_BIT);

    GLsizei stride;
    vt->GetIntegerv(GL_PACK_ROW_LENGTH, &stride);
    stride = width;

    context->buffer_mapping = pixels;
    context->rc ++;

    /* Swap framebuffer */
    UpdateBuffer(gl);

    if (!sys->current)
        eglMakeCurrent(sys->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);

    /* Output as picture */
    picture_resource_t pict_resource = {
        .pf_destroy = NULL,
    };

    pict_resource.p[0].p_pixels = pixels;
    pict_resource.p[0].i_lines = height;
    pict_resource.p[0].i_pitch = stride * 4;


    picture_t *output = picture_NewFromResource(&sys->fmt_out, &pict_resource);
    assert(output);
    if (output == NULL)
        goto error;

    output->context = (picture_context_t *)context;
    output->context->vctx = NULL;

    return output;

error:
    context->rc--;
    return NULL;
}

static void Close( vlc_gl_t *gl )
{
    struct vlc_gl_pbuffer *sys = gl->sys;
    const opengl_vtable_t *vt = &sys->api.vt;

    vlc_gl_MakeCurrent(sys->gl);
    vt->DeleteBuffers(BUFFER_COUNT, sys->pixelbuffers);
    vt->DeleteFramebuffers(BUFFER_COUNT, sys->framebuffers);
    vt->DeleteTextures(BUFFER_COUNT, sys->textures);
    vlc_gl_ReleaseCurrent(sys->gl);

    eglTerminate(sys->display);
}

static int Open(vlc_gl_t *gl, unsigned width, unsigned height)
{
    struct vlc_gl_pbuffer *sys = vlc_obj_malloc(&gl->obj, sizeof *sys);
    if (sys == NULL)
        return VLC_ENOMEM;

    sys->gl = gl;
    sys->current = false;

    video_format_Init(&sys->fmt_out, VLC_CODEC_RGBA);
    sys->fmt_out.i_visible_width
        = sys->fmt_out.i_width
        = width;
    sys->fmt_out.i_visible_height
        = sys->fmt_out.i_height
        = height;

    gl->offscreen_chroma_out = VLC_CODEC_RGBA;
    gl->offscreen_vctx_out = NULL;

    vlc_mutex_init(&sys->lock);
    vlc_cond_init(&sys->cond);

    gl->sys = sys;

    if (InitEGL(gl, width, height) != VLC_SUCCESS)
    {
        msg_Err(gl, "Failed to create opengl context\n");
        goto error1;
    }

    gl->ext = VLC_GL_EXT_EGL;
    gl->make_current = MakeCurrent;
    gl->release_current = ReleaseCurrent;
    gl->resize = NULL;
    gl->swap_offscreen = Swap;
    gl->get_proc_address = GetSymbol;
    gl->destroy = Close;
    gl->egl.queryString = QueryString;
    gl->offscreen_vflip = true;

    sys->eglCreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    sys->eglDestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    if (sys->eglCreateImageKHR != NULL && sys->eglDestroyImageKHR != NULL)
    {
        gl->egl.createImageKHR = CreateImageKHR;
        gl->egl.destroyImageKHR = DestroyImageKHR;
    }

    vlc_gl_MakeCurrent(gl);
    int ret = vlc_gl_api_Init(&sys->api, gl);
    if (ret != VLC_SUCCESS)
    {
        msg_Err(gl, "Failed to initialize gl_api");
        vlc_gl_ReleaseCurrent(gl);
        goto error2;
    }

    const opengl_vtable_t *vt = &sys->api.vt;
    vt->GenBuffers(BUFFER_COUNT, sys->pixelbuffers);
    vt->GenFramebuffers(BUFFER_COUNT, sys->framebuffers);
    vt->GenTextures(BUFFER_COUNT, sys->textures);

    for (size_t i=0; i<BUFFER_COUNT; ++i)
    {
        vt->BindBuffer(GL_PIXEL_PACK_BUFFER, sys->pixelbuffers[i]);
        vt->BufferData(GL_PIXEL_PACK_BUFFER, width*height*4, NULL,
                       GL_STREAM_READ);
        vt->BindFramebuffer(GL_FRAMEBUFFER, sys->framebuffers[i]);
        vt->BindTexture(GL_TEXTURE_2D, sys->textures[i]);
        vt->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        vt->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, sys->textures[i], 0);

        struct pbo_picture_context *context = &sys->picture_contexts[i];
        context->buffer_mapping = NULL;
        context->lock = &sys->lock;
        context->cond = &sys->cond;
        context->context.destroy = picture_context_destroy;
        context->context.copy = picture_context_copy;
        context->rc = 0;
    }

    sys->current_flip = BUFFER_COUNT - 1;
    BindDrawFramebuffer(sys);

    vlc_gl_ReleaseCurrent(gl);

    return VLC_SUCCESS;

error2:
    eglTerminate(sys->display);
error1:
    vlc_obj_free(&gl->obj, sys);

    return VLC_EGENERIC;
}

vlc_module_begin()
    set_shortname( N_("egl_pbuffer") )
    set_description( N_("EGL PBuffer offscreen opengl provider") )
#ifdef USE_OPENGL_ES2
    set_capability( "opengl es2 offscreen", 1)
#else
    set_capability( "opengl offscreen", 1 )
#endif

    add_shortcut( "egl_pbuffer" )
    set_callback( Open )
vlc_module_end()

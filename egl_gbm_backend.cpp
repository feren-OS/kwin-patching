/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2015 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "egl_gbm_backend.h"
// kwin
#include "composite.h"
#include "drm_backend.h"
#include "options.h"
#include "screens.h"
// kwin libs
#include <kwinglplatform.h>
// Qt
#include <QOpenGLContext>
// system
#include <gbm.h>

namespace KWin
{

EglGbmBackend::EglGbmBackend(DrmBackend *b)
    : QObject(NULL)
    , AbstractEglBackend()
    , m_backend(b)
{
    initializeEgl();
    init();
    // Egl is always direct rendering
    setIsDirectRendering(true);
}

EglGbmBackend::~EglGbmBackend()
{
    // TODO: cleanup front buffer?
    cleanup();
    if (m_device) {
        gbm_device_destroy(m_device);
    }
}

void EglGbmBackend::cleanupSurfaces()
{
    for (auto it = m_outputs.constBegin(); it != m_outputs.constEnd(); ++it) {
        const Output &o = *it;
        if (o.eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay(), o.eglSurface);
        }
        if ((*it).gbmSurface) {
            gbm_surface_destroy((*it).gbmSurface);
        }
    }
}

bool EglGbmBackend::initializeEgl()
{
    initClientExtensions();
    EGLDisplay display = EGL_NO_DISPLAY;

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (!hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_base")) ||
            !hasClientExtension(QByteArrayLiteral("EGL_MESA_platform_gbm"))) {
        setFailed("EGL_EXT_platform_base and/or EGL_MESA_platform_gbm missing");
        return false;
    }

    m_device = gbm_create_device(m_backend->fd());
    if (!m_device) {
        setFailed("Could not create gbm device");
        return false;
    }

    display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, m_device, nullptr);

    if (display == EGL_NO_DISPLAY)
        return false;
    setEglDisplay(display);
    return initEglAPI();
}

void EglGbmBackend::init()
{
    if (!initRenderingContext()) {
        setFailed("Could not initialize rendering context");
        return;
    }

    initKWinGL();
    initBufferAge();
    initWayland();
}

bool EglGbmBackend::initRenderingContext()
{
    initBufferConfigs();

    EGLContext context = EGL_NO_CONTEXT;
#ifdef KWIN_HAVE_OPENGLES
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    context = eglCreateContext(eglDisplay(), config(), EGL_NO_CONTEXT, context_attribs);
#else
    const EGLint context_attribs_31_core[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
        EGL_CONTEXT_MINOR_VERSION_KHR, 1,
        EGL_CONTEXT_FLAGS_KHR,         EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR,
        EGL_NONE
    };

    const EGLint context_attribs_legacy[] = {
        EGL_NONE
    };

    const char* eglExtensionsCString = eglQueryString(eglDisplay(), EGL_EXTENSIONS);
    const QList<QByteArray> extensions = QByteArray::fromRawData(eglExtensionsCString, qstrlen(eglExtensionsCString)).split(' ');

    // Try to create a 3.1 core context
    if (options->glCoreProfile() && extensions.contains(QByteArrayLiteral("EGL_KHR_create_context")))
        context = eglCreateContext(eglDisplay(), config(), EGL_NO_CONTEXT, context_attribs_31_core);

    if (context == EGL_NO_CONTEXT)
        context = eglCreateContext(eglDisplay(), config(), EGL_NO_CONTEXT, context_attribs_legacy);
#endif

    if (context == EGL_NO_CONTEXT) {
        qCCritical(KWIN_CORE) << "Create Context failed";
        return false;
    }
    setContext(context);

    const auto outputs = m_backend->outputs();
    for (DrmOutput *drmOutput: outputs) {
        Output o;
        o.output = drmOutput;
        o.gbmSurface = gbm_surface_create(m_device, drmOutput->size().width(), drmOutput->size().height(),
                                         GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!o.gbmSurface) {
            qCCritical(KWIN_CORE) << "Create gbm surface failed";
            continue;
        }
        o.eglSurface = eglCreatePlatformWindowSurfaceEXT(eglDisplay(), config(), (void *)o.gbmSurface, nullptr);
        if (o.eglSurface == EGL_NO_SURFACE) {
            qCCritical(KWIN_CORE) << "Create Window Surface failed";
            gbm_surface_destroy(o.gbmSurface);
            continue;
        }
        m_outputs << o;
    }
    if (m_outputs.isEmpty()) {
        qCCritical(KWIN_CORE) << "Create Window Surfaces failed";
        return false;
    }
    // set our first surface as the one for the abstract backend, just to make it happy
    setSurface(m_outputs.first().eglSurface);

    return makeContextCurrent(m_outputs.first());
}

bool EglGbmBackend::makeContextCurrent(const Output &output)
{
    const EGLSurface surface = output.eglSurface;
    if (surface == EGL_NO_SURFACE) {
        return false;
    }
    if (eglMakeCurrent(eglDisplay(), surface, surface, context()) == EGL_FALSE) {
        qCCritical(KWIN_CORE) << "Make Context Current failed";
        return false;
    }

    EGLint error = eglGetError();
    if (error != EGL_SUCCESS) {
        qCWarning(KWIN_CORE) << "Error occurred while creating context " << error;
        return false;
    }
    // TODO: ensure the viewport is set correctly each time
    const QSize &overall = screens()->size();
    const QRect &v = output.output->geometry();
    // TODO: are the values correct?
    glViewport(-v.x(), v.height() - overall.height() - v.y(), overall.width(), overall.height());
    return true;
}

bool EglGbmBackend::initBufferConfigs()
{
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,         EGL_WINDOW_BIT,
        EGL_RED_SIZE,             1,
        EGL_GREEN_SIZE,           1,
        EGL_BLUE_SIZE,            1,
        EGL_ALPHA_SIZE,           0,
#ifdef KWIN_HAVE_OPENGLES
        EGL_RENDERABLE_TYPE,      EGL_OPENGL_ES2_BIT,
#else
        EGL_RENDERABLE_TYPE,      EGL_OPENGL_BIT,
#endif
        EGL_CONFIG_CAVEAT,        EGL_NONE,
        EGL_NONE,
    };

    EGLint count;
    EGLConfig configs[1024];
    if (eglChooseConfig(eglDisplay(), config_attribs, configs, 1, &count) == EGL_FALSE) {
        qCCritical(KWIN_CORE) << "choose config failed";
        return false;
    }
    if (count != 1) {
        qCCritical(KWIN_CORE) << "choose config did not return a config" << count;
        return false;
    }
    setConfig(configs[0]);

    return true;
}

void EglGbmBackend::present()
{
    for (auto &o: m_outputs) {
        makeContextCurrent(o);
        eglSwapBuffers(eglDisplay(), o.eglSurface);
        auto oldBuffer = o.buffer;
        o.buffer = m_backend->createBuffer(o.gbmSurface);
        m_backend->present(o.buffer, o.output);
        delete oldBuffer;
        if (supportsBufferAge()) {
            eglQuerySurface(eglDisplay(), o.eglSurface, EGL_BUFFER_AGE_EXT, &o.bufferAge);
        }
    }
}

void EglGbmBackend::screenGeometryChanged(const QSize &size)
{
    Q_UNUSED(size)
    // TODO, create new buffer?
}

SceneOpenGL::TexturePrivate *EglGbmBackend::createBackendTexture(SceneOpenGL::Texture *texture)
{
    return new EglGbmTexture(texture, this);
}

QRegion EglGbmBackend::prepareRenderingFrame()
{
    QRegion repaint;
    if (supportsBufferAge()) {
        for (auto it = m_outputs.constBegin(); it != m_outputs.constEnd(); ++it) {
            repaint = repaint.united(accumulatedDamageHistory((*it).bufferAge));
        }
    }
    startRenderTimer();
    return repaint;
}

void EglGbmBackend::prepareRenderingForScreen(int screenId)
{
    makeContextCurrent(m_outputs.at(screenId));
}

void EglGbmBackend::endRenderingFrame(const QRegion &renderedRegion, const QRegion &damagedRegion)
{
    if (damagedRegion.isEmpty()) {

        // If the damaged region of a window is fully occluded, the only
        // rendering done, if any, will have been to repair a reused back
        // buffer, making it identical to the front buffer.
        //
        // In this case we won't post the back buffer. Instead we'll just
        // set the buffer age to 1, so the repaired regions won't be
        // rendered again in the next frame.
        if (!renderedRegion.isEmpty())
            glFlush();

        for (auto &o: m_outputs) {
            o.bufferAge = 1;
        }
        return;
    }
    present();

    // Save the damaged region to history
    if (supportsBufferAge())
        addToDamageHistory(damagedRegion);
}

bool EglGbmBackend::usesOverlayWindow() const
{
    return false;
}

bool EglGbmBackend::perScreenRendering() const
{
    return true;
}

/************************************************
 * EglTexture
 ************************************************/

EglGbmTexture::EglGbmTexture(KWin::SceneOpenGL::Texture *texture, EglGbmBackend *backend)
    : AbstractEglTexture(texture, backend)
{
}

EglGbmTexture::~EglGbmTexture() = default;

} // namespace

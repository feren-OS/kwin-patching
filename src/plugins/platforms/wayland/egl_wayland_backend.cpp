/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2019 Roman Gilg <subdiff@gmail.com>
    SPDX-FileCopyrightText: 2013 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#define WL_EGL_PLATFORM 1

#include "egl_wayland_backend.h"

#include "wayland_backend.h"
#include "wayland_output.h"

#include "composite.h"
#include "logging.h"
#include "options.h"

#include "wayland_server.h"
#include "screens.h"

#include <unistd.h>
#include <fcntl.h>

// kwin libs
#include <kwinglplatform.h>

// KDE
#include <KWayland/Client/surface.h>
#include <KWaylandServer/buffer_interface.h>
#include <KWaylandServer/display.h>

// Qt
#include <QFile>
#include <QOpenGLContext>

#include <cmath>

namespace KWin
{
namespace Wayland
{

EglWaylandOutput::EglWaylandOutput(WaylandOutput *output, QObject *parent)
    : QObject(parent)
    , m_waylandOutput(output)
{
}

bool EglWaylandOutput::init(EglWaylandBackend *backend)
{
    auto surface = m_waylandOutput->surface();
    const QSize nativeSize = m_waylandOutput->geometry().size() * m_waylandOutput->scale();
    auto overlay = wl_egl_window_create(*surface, nativeSize.width(), nativeSize.height());
    if (!overlay) {
        qCCritical(KWIN_WAYLAND_BACKEND) << "Creating Wayland Egl window failed";
        return false;
    }
    m_overlay = overlay;

    EGLSurface eglSurface = EGL_NO_SURFACE;
    if (backend->havePlatformBase()) {
        eglSurface = eglCreatePlatformWindowSurfaceEXT(backend->eglDisplay(), backend->config(), (void *) overlay, nullptr);
    } else {
        eglSurface = eglCreateWindowSurface(backend->eglDisplay(), backend->config(), overlay, nullptr);
    }
    if (eglSurface == EGL_NO_SURFACE) {
        qCCritical(KWIN_WAYLAND_BACKEND) << "Create Window Surface failed";
        return false;
    }
    m_eglSurface = eglSurface;

    connect(m_waylandOutput, &WaylandOutput::sizeChanged, this, &EglWaylandOutput::updateSize);
    connect(m_waylandOutput, &WaylandOutput::modeChanged, this, &EglWaylandOutput::updateSize);

    return true;
}

void EglWaylandOutput::updateSize()
{
    const QSize nativeSize = m_waylandOutput->geometry().size() * m_waylandOutput->scale();
    wl_egl_window_resize(m_overlay, nativeSize.width(), nativeSize.height(), 0, 0);
}

EglWaylandBackend::EglWaylandBackend(WaylandBackend *b)
    : AbstractEglBackend()
    , m_backend(b)
{
    if (!m_backend) {
        setFailed("Wayland Backend has not been created");
        return;
    }
    qCDebug(KWIN_WAYLAND_BACKEND) << "Connected to Wayland display?" << (m_backend->display() ? "yes" : "no" );
    if (!m_backend->display()) {
        setFailed("Could not connect to Wayland compositor");
        return;
    }

    // Egl is always direct rendering
    setIsDirectRendering(true);

    connect(m_backend, &WaylandBackend::outputAdded, this, &EglWaylandBackend::createEglWaylandOutput);
    connect(m_backend, &WaylandBackend::outputRemoved, this,
        [this] (AbstractOutput *output) {
            auto it = std::find_if(m_outputs.begin(), m_outputs.end(),
                [output] (const EglWaylandOutput *o) {
                    return o->m_waylandOutput == output;
                }
            );
            if (it == m_outputs.end()) {
                return;
            }
            cleanupOutput(*it);
            m_outputs.erase(it);
        }
    );
}

EglWaylandBackend::~EglWaylandBackend()
{
    cleanup();
}

void EglWaylandBackend::cleanupSurfaces()
{
    for (auto o : m_outputs) {
        cleanupOutput(o);
    }
    m_outputs.clear();
}

bool EglWaylandBackend::createEglWaylandOutput(AbstractOutput *waylandOutput)
{
    auto *output = new EglWaylandOutput(static_cast<WaylandOutput *>(waylandOutput), this);
    if (!output->init(this)) {
        return false;
    }
    m_outputs << output;
    return true;
}

void EglWaylandBackend::cleanupOutput(EglWaylandOutput *output)
{
    wl_egl_window_destroy(output->m_overlay);
}

bool EglWaylandBackend::initializeEgl()
{
    initClientExtensions();
    EGLDisplay display = m_backend->sceneEglDisplay();

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (display == EGL_NO_DISPLAY) {
        m_havePlatformBase = hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_base"));
        if (m_havePlatformBase) {
            // Make sure that the wayland platform is supported
            if (!hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_wayland")))
                return false;

            display = eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, m_backend->display(), nullptr);
        } else {
            display = eglGetDisplay(m_backend->display());
        }
    }

    if (display == EGL_NO_DISPLAY)
        return false;
    setEglDisplay(display);
    return initEglAPI();
}

void EglWaylandBackend::init()
{
    if (!initializeEgl()) {
        setFailed("Could not initialize egl");
        return;
    }
    if (!initRenderingContext()) {
        setFailed("Could not initialize rendering context");
        return;
    }

    initKWinGL();
    initBufferAge();
    initWayland();
}

bool EglWaylandBackend::initRenderingContext()
{
    initBufferConfigs();

    if (!createContext()) {
        return false;
    }

    auto waylandOutputs = m_backend->waylandOutputs();

    // we only allow to start with at least one output
    if (waylandOutputs.isEmpty()) {
        return false;
    }

    for (auto *out : waylandOutputs) {
        if (!createEglWaylandOutput(out)) {
            return false;
        }
    }

    if (m_outputs.isEmpty()) {
        qCCritical(KWIN_WAYLAND_BACKEND) << "Create Window Surfaces failed";
        return false;
    }

    auto *firstOutput = m_outputs.first();
    // set our first surface as the one for the abstract backend, just to make it happy
    setSurface(firstOutput->m_eglSurface);
    return makeContextCurrent(firstOutput);
}

bool EglWaylandBackend::makeContextCurrent(EglWaylandOutput *output)
{
    const EGLSurface eglSurface = output->m_eglSurface;
    if (eglSurface == EGL_NO_SURFACE) {
        return false;
    }
    if (eglMakeCurrent(eglDisplay(), eglSurface, eglSurface, context()) == EGL_FALSE) {
        qCCritical(KWIN_WAYLAND_BACKEND) << "Make Context Current failed";
        return false;
    }

    EGLint error = eglGetError();
    if (error != EGL_SUCCESS) {
        qCWarning(KWIN_WAYLAND_BACKEND) << "Error occurred while creating context " << error;
        return false;
    }

    const QRect &v = output->m_waylandOutput->geometry();
    const qreal scale = output->m_waylandOutput->scale();

    const QSize overall = screens()->size();
    glViewport(-v.x() * scale, (v.height() - overall.height() + v.y()) * scale,
               overall.width() * scale, overall.height() * scale);
    return true;
}

bool EglWaylandBackend::initBufferConfigs()
{
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,         EGL_WINDOW_BIT,
        EGL_RED_SIZE,             1,
        EGL_GREEN_SIZE,           1,
        EGL_BLUE_SIZE,            1,
        EGL_ALPHA_SIZE,           0,
        EGL_RENDERABLE_TYPE,      isOpenGLES() ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT,
        EGL_CONFIG_CAVEAT,        EGL_NONE,
        EGL_NONE,
    };

    EGLint count;
    EGLConfig configs[1024];
    if (eglChooseConfig(eglDisplay(), config_attribs, configs, 1, &count) == EGL_FALSE) {
        qCCritical(KWIN_WAYLAND_BACKEND) << "choose config failed";
        return false;
    }
    if (count != 1) {
        qCCritical(KWIN_WAYLAND_BACKEND) << "choose config did not return a config" << count;
        return false;
    }
    setConfig(configs[0]);

    return true;
}

static QVector<EGLint> regionToRects(const QRegion &region, AbstractWaylandOutput *output)
{
    const int height = output->modeSize().height();
    const QMatrix4x4 matrix = WaylandOutput::logicalToNativeMatrix(output->geometry(),
                                                                   output->scale(),
                                                                   output->transform());

    QVector<EGLint> rects;
    rects.reserve(region.rectCount() * 4);
    for (const QRect &_rect : region) {
        const QRect rect = matrix.mapRect(_rect);

        rects << rect.left();
        rects << height - (rect.y() + rect.height());
        rects << rect.width();
        rects << rect.height();
    }
    return rects;
}

void EglWaylandBackend::aboutToStartPainting(int screenId, const QRegion &damagedRegion)
{
    Q_ASSERT_X(screenId != -1, "aboutToStartPainting", "not using per screen rendering");
    EglWaylandOutput *output = m_outputs.at(screenId);
    if (output->m_bufferAge > 0 && !damagedRegion.isEmpty() && supportsPartialUpdate()) {
        const QRegion region = damagedRegion & output->m_waylandOutput->geometry();

        QVector<EGLint> rects = regionToRects(region, output->m_waylandOutput);
        const bool correct = eglSetDamageRegionKHR(eglDisplay(), output->m_eglSurface,
                                                   rects.data(), rects.count()/4);
        if (!correct) {
            qCWarning(KWIN_WAYLAND_BACKEND) << "failed eglSetDamageRegionKHR" << eglGetError();
        }
    }
}

void EglWaylandBackend::presentOnSurface(EglWaylandOutput *output, const QRegion &damage)
{
    WaylandOutput *waylandOutput = output->m_waylandOutput;

    waylandOutput->surface()->setupFrameCallback();
    waylandOutput->surface()->setScale(std::ceil(waylandOutput->scale()));
    Q_EMIT waylandOutput->outputChange(damage);

    if (supportsSwapBuffersWithDamage() && !output->m_damageHistory.isEmpty()) {
        QVector<EGLint> rects = regionToRects(output->m_damageHistory.constFirst(), waylandOutput);
        if (!eglSwapBuffersWithDamageEXT(eglDisplay(), output->m_eglSurface,
                                         rects.data(), rects.count() / 4)) {
            qCCritical(KWIN_WAYLAND_BACKEND, "eglSwapBuffersWithDamage() failed: %x", eglGetError());
        }
    } else {
        if (!eglSwapBuffers(eglDisplay(), output->m_eglSurface)) {
            qCCritical(KWIN_WAYLAND_BACKEND, "eglSwapBuffers() failed: %x", eglGetError());
        }
    }

    if (supportsBufferAge()) {
        eglQuerySurface(eglDisplay(), output->m_eglSurface, EGL_BUFFER_AGE_EXT, &output->m_bufferAge);
    }

}

void EglWaylandBackend::screenGeometryChanged(const QSize &size)
{
    Q_UNUSED(size)
    // no backend specific code needed
    // TODO: base implementation in OpenGLBackend

    // The back buffer contents are now undefined
    for (auto *output : qAsConst(m_outputs)) {
        output->m_bufferAge = 0;
    }
}

SceneOpenGLTexturePrivate *EglWaylandBackend::createBackendTexture(SceneOpenGLTexture *texture)
{
    return new EglWaylandTexture(texture, this);
}

QRegion EglWaylandBackend::beginFrame(int screenId)
{
    eglWaitNative(EGL_CORE_NATIVE_ENGINE);

    auto *output = m_outputs.at(screenId);
    makeContextCurrent(output);
    if (supportsBufferAge()) {
        QRegion region;

        // Note: An age of zero means the buffer contents are undefined
        if (output->m_bufferAge > 0 && output->m_bufferAge <= output->m_damageHistory.count()) {
            for (int i = 0; i < output->m_bufferAge - 1; i++)
                region |= output->m_damageHistory[i];
        } else {
            region = output->m_waylandOutput->geometry();
        }

        return region;
    }
    return QRegion();
}

void EglWaylandBackend::endFrame(int screenId, const QRegion &renderedRegion, const QRegion &damagedRegion)
{
    EglWaylandOutput *output = m_outputs[screenId];
    QRegion damage = damagedRegion.intersected(output->m_waylandOutput->geometry());
    presentOnSurface(output, damage);

    if (supportsBufferAge()) {
        if (output->m_damageHistory.count() > 10) {
            output->m_damageHistory.removeLast();
        }

        output->m_damageHistory.prepend(damage);
    }
}

/************************************************
 * EglTexture
 ************************************************/

EglWaylandTexture::EglWaylandTexture(KWin::SceneOpenGLTexture *texture, KWin::Wayland::EglWaylandBackend *backend)
    : AbstractEglTexture(texture, backend)
{
}

EglWaylandTexture::~EglWaylandTexture() = default;

}
}

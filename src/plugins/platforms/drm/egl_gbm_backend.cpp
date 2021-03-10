/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "egl_gbm_backend.h"
// kwin
#include "composite.h"
#include "drm_backend.h"
#include "drm_output.h"
#include "gbm_surface.h"
#include "logging.h"
#include "options.h"
#include "renderloop_p.h"
#include "screens.h"
#include "surfaceitem_wayland.h"
#include "drm_gpu.h"
#include "linux_dmabuf.h"
// kwin libs
#include <kwinglplatform.h>
#include <kwineglimagetexture.h>
// system
#include <gbm.h>
#include <unistd.h>
#include <errno.h>
// kwayland server
#include "KWaylandServer/surface_interface.h"
#include "KWaylandServer/buffer_interface.h"
#include "KWaylandServer/linuxdmabuf_v1_interface.h"
#include <egl_dmabuf.h>
#include <drm_fourcc.h>

namespace KWin
{

EglGbmBackend::EglGbmBackend(DrmBackend *drmBackend, DrmGpu *gpu)
    : AbstractEglDrmBackend(drmBackend, gpu)
{
}

void EglGbmBackend::cleanupSurfaces()
{
    for (auto it = m_outputs.begin(); it != m_outputs.end(); ++it) {
        cleanupOutput(*it);
    }
    m_outputs.clear();
}

void EglGbmBackend::cleanupFramebuffer(Output &output)
{
    if (!output.render.framebuffer) {
        return;
    }
    makeContextCurrent(output);
    glDeleteTextures(1, &output.render.texture);
    output.render.texture = 0;
    glDeleteFramebuffers(1, &output.render.framebuffer);
    output.render.framebuffer = 0;
}

void EglGbmBackend::cleanupOutput(Output &output)
{
    cleanupFramebuffer(output);
    output.output->releaseGbm();

    if (output.eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(eglDisplay(), output.eglSurface);
    }
    if (output.secondaryGbmBo) {
        output.gbmSurface.get()->releaseBuffer(output.secondaryGbmBo);
    }
    if (output.directScanoutBuffer) {
        gbm_bo_destroy(output.directScanoutBuffer);
    }
    if (output.dmabufFd) {
        close(output.dmabufFd);
    }
}

bool EglGbmBackend::initializeEgl()
{
    initClientExtensions();
    EGLDisplay display = m_gpu->eglDisplay();

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (display == EGL_NO_DISPLAY) {
        const bool hasMesaGBM = hasClientExtension(QByteArrayLiteral("EGL_MESA_platform_gbm"));
        const bool hasKHRGBM = hasClientExtension(QByteArrayLiteral("EGL_KHR_platform_gbm"));
        const GLenum platform = hasMesaGBM ? EGL_PLATFORM_GBM_MESA : EGL_PLATFORM_GBM_KHR;

        if (!hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_base")) ||
                (!hasMesaGBM && !hasKHRGBM)) {
            setFailed("Missing one or more extensions between EGL_EXT_platform_base, "
                      "EGL_MESA_platform_gbm, EGL_KHR_platform_gbm");
            return false;
        }

        auto device = gbm_create_device(m_gpu->fd());
        if (!device) {
            setFailed("Could not create gbm device");
            return false;
        }
        m_gpu->setGbmDevice(device);

        display = eglGetPlatformDisplayEXT(platform, device, nullptr);
        m_gpu->setEglDisplay(display);
    }

    if (display == EGL_NO_DISPLAY) {
        return false;
    }
    setEglDisplay(display);
    return initEglAPI();
}

void EglGbmBackend::init()
{
    if (!initializeEgl()) {
        setFailed("Could not initialize egl");
        return;
    }
    if (!initRenderingContext()) {
        setFailed("Could not initialize rendering context");
        return;
    }
    initBufferAge();
    // at the moment: no secondary GPU -> no OpenGL context!
    if (isPrimary()) {
        initKWinGL();
        initWayland();
    }
}

bool EglGbmBackend::initRenderingContext()
{
    initBufferConfigs();
    // no secondary GPU -> no OpenGL context!
    if (isPrimary() && !createContext()) {
        return false;
    }

    const auto outputs = m_gpu->outputs();

    for (DrmOutput *drmOutput: outputs) {
        addOutput(drmOutput);
    }

    if (m_outputs.isEmpty() && !outputs.isEmpty()) {
        qCCritical(KWIN_DRM) << "Create Window Surfaces failed";
        return false;
    }
    if (!m_outputs.isEmpty()) {
        // Set our first surface as the one for the abstract backend, just to make it happy.
        setSurface(m_outputs.first().eglSurface);
        if (isPrimary()) {
            return makeContextCurrent(m_outputs.first());
        }
    }
    return true;
}

EGLSurface EglGbmBackend::createEglSurface(std::shared_ptr<GbmSurface> gbmSurface) const
{
    auto eglSurface = eglCreatePlatformWindowSurfaceEXT(eglDisplay(), config(),
                                                        (void *)(gbmSurface->surface()), nullptr);
    if (eglSurface == EGL_NO_SURFACE) {
        qCCritical(KWIN_DRM) << "Creating EGL surface failed";
        return EGL_NO_SURFACE;
    }
    return eglSurface;
}

bool EglGbmBackend::resetOutput(Output &output, DrmOutput *drmOutput)
{
    output.output = drmOutput;
    const QSize size = drmOutput->hardwareTransforms() ? drmOutput->pixelSize() :
                                                         drmOutput->modeSize();
    int flags = GBM_BO_USE_RENDERING;
    if (drmOutput->gpu() == m_gpu) {
        flags |= GBM_BO_USE_SCANOUT;
    } else {
        flags |= GBM_BO_USE_LINEAR;
    }
    auto gbmSurface = std::make_shared<GbmSurface>(m_gpu->gbmDevice(),
                                                   size.width(), size.height(),
                                                   GBM_FORMAT_XRGB8888,
                                                   flags);
    if (!gbmSurface) {
        qCCritical(KWIN_DRM) << "Creating GBM surface failed";
        return false;
    }
    auto eglSurface = createEglSurface(gbmSurface);
    if (eglSurface == EGL_NO_SURFACE) {
        return false;
    }

    if (surface() == output.eglSurface || surface() == EGL_NO_SURFACE) {
        setSurface(eglSurface);
    }
    // destroy previous surface
    if (output.eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(eglDisplay(), output.eglSurface);
    }
    output.eglSurface = eglSurface;
    output.gbmSurface = gbmSurface;

    resetFramebuffer(output);
    return true;
}

void EglGbmBackend::addOutput(DrmOutput *drmOutput)
{
    if (isPrimary()) {
        Output newOutput;
        if (resetOutput(newOutput, drmOutput)) {
            QVector<Output> &outputs = drmOutput->gpu() == m_gpu ? m_outputs : m_secondaryGpuOutputs;
            connect(drmOutput, &DrmOutput::modeChanged, this,
                [drmOutput, &outputs, this] {
                    auto it = std::find_if(outputs.begin(), outputs.end(),
                        [drmOutput] (const auto &output) {
                            return output.output == drmOutput;
                        }
                    );
                    if (it == outputs.end()) {
                        return;
                    }
                    resetOutput(*it, drmOutput);
                }
            );
            outputs << newOutput;
        }
    } else {
        Output newOutput;
        newOutput.output = drmOutput;
        renderingBackend()->addOutput(drmOutput);
        m_outputs << newOutput;
    }
}

void EglGbmBackend::removeOutput(DrmOutput *drmOutput)
{
    QVector<Output> &outputs = drmOutput->gpu() == m_gpu ? m_outputs : m_secondaryGpuOutputs;
    auto it = std::find_if(outputs.begin(), outputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == outputs.end()) {
        return;
    }
    if (isPrimary()) {
        cleanupOutput(*it);
    } else {
        renderingBackend()->removeOutput((*it).output);
    }
    outputs.erase(it);
}

int EglGbmBackend::getDmabufForSecondaryGpuOutput(AbstractOutput *output, uint32_t *format, uint32_t *stride)
{
    DrmOutput *drmOutput = static_cast<DrmOutput*>(output);
    auto it = std::find_if(m_secondaryGpuOutputs.begin(), m_secondaryGpuOutputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == m_secondaryGpuOutputs.end()) {
        return -1;
    }
    if (it->dmabufFd) {
        close(it->dmabufFd);
        it->dmabufFd = 0;
    }
    if (it->secondaryGbmBo) {
        it->gbmSurface.get()->releaseBuffer(it->secondaryGbmBo);
        it->secondaryGbmBo = nullptr;
    }
    renderFramebufferToSurface(*it);
    auto error = eglSwapBuffers(eglDisplay(), it->eglSurface);
    if (error != EGL_TRUE) {
        qCDebug(KWIN_DRM) << "an error occurred while swapping buffers" << error;
        return -1;
    }
    it->secondaryGbmBo = it->gbmSurface->lockFrontBuffer();
    int fd = gbm_bo_get_fd(it->secondaryGbmBo);
    if (fd == -1) {
        qCDebug(KWIN_DRM) << "failed to export gbm_bo as dma-buf!";
        return -1;
    }
    it->dmabufFd = fd;
    *format = gbm_bo_get_format(it->secondaryGbmBo);
    *stride = gbm_bo_get_stride(it->secondaryGbmBo);
    return it->dmabufFd;
}

QRegion EglGbmBackend::beginFrameForSecondaryGpu(AbstractOutput *output)
{
    DrmOutput *drmOutput = static_cast<DrmOutput*>(output);
    auto it = std::find_if(m_secondaryGpuOutputs.begin(), m_secondaryGpuOutputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == m_secondaryGpuOutputs.end()) {
        return QRegion();
    }
    return prepareRenderingForOutput(*it);
}

const float vertices[] = {
   -1.0f,  1.0f,
   -1.0f, -1.0f,
    1.0f, -1.0f,

   -1.0f,  1.0f,
    1.0f, -1.0f,
    1.0f,  1.0f,
};

const float texCoords[] = {
    0.0f,  1.0f,
    0.0f,  0.0f,
    1.0f,  0.0f,

    0.0f,  1.0f,
    1.0f,  0.0f,
    1.0f,  1.0f
};

bool EglGbmBackend::resetFramebuffer(Output &output)
{
    cleanupFramebuffer(output);

    if (output.output->hardwareTransforms()) {
        // No need for an extra render target.
        return true;
    }

    makeContextCurrent(output);

    glGenFramebuffers(1, &output.render.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, output.render.framebuffer);
    GLRenderTarget::setKWinFramebuffer(output.render.framebuffer);

    glGenTextures(1, &output.render.texture);
    glBindTexture(GL_TEXTURE_2D, output.render.texture);

    const QSize texSize = output.output->pixelSize();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texSize.width(), texSize.height(),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindTexture(GL_TEXTURE_2D, 0);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           output.render.texture, 0);

    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        qCWarning(KWIN_DRM) << "Error: framebuffer not complete";
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLRenderTarget::setKWinFramebuffer(0);

    return true;
}

void EglGbmBackend::initRenderTarget(Output &output)
{
    if (output.render.vbo) {
        // Already initialized.
        return;
    }
    std::shared_ptr<GLVertexBuffer> vbo(new GLVertexBuffer(KWin::GLVertexBuffer::Static));
    vbo->setData(6, 2, vertices, texCoords);
    output.render.vbo = vbo;
}

void EglGbmBackend::renderFramebufferToSurface(Output &output)
{
    if (!output.render.framebuffer && isPrimary()) {
        // No additional render target.
        return;
    }
    const auto size = output.output->modeSize();
    if (isPrimary()) {
        // primary GPU
        makeContextCurrent(output);

        glViewport(0, 0, size.width(), size.height());

        auto shader = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture);

        QMatrix4x4 mvpMatrix;

        const DrmOutput *drmOutput = output.output;
        switch (drmOutput->transform()) {
        case DrmOutput::Transform::Normal:
        case DrmOutput::Transform::Flipped:
            break;
        case DrmOutput::Transform::Rotated90:
        case DrmOutput::Transform::Flipped90:
            mvpMatrix.rotate(90, 0, 0, 1);
            break;
        case DrmOutput::Transform::Rotated180:
        case DrmOutput::Transform::Flipped180:
            mvpMatrix.rotate(180, 0, 0, 1);
            break;
        case DrmOutput::Transform::Rotated270:
        case DrmOutput::Transform::Flipped270:
            mvpMatrix.rotate(270, 0, 0, 1);
            break;
        }
        switch (drmOutput->transform()) {
        case DrmOutput::Transform::Flipped:
        case DrmOutput::Transform::Flipped90:
        case DrmOutput::Transform::Flipped180:
        case DrmOutput::Transform::Flipped270:
            mvpMatrix.scale(-1, 1);
            break;
        default:
            break;
        }

        shader->setUniform(GLShader::ModelViewProjectionMatrix, mvpMatrix);

        initRenderTarget(output);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        GLRenderTarget::setKWinFramebuffer(0);
        glBindTexture(GL_TEXTURE_2D, output.render.texture);
        output.render.vbo->render(GL_TRIANGLES);
        ShaderManager::instance()->popShader();
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        // secondary GPU: render on primary and import framebuffer
        uint32_t stride = 0;
        uint32_t format = 0;
        int fd = renderingBackend()->getDmabufForSecondaryGpuOutput(output.output, &format, &stride);
        if (fd != -1) {
            struct gbm_import_fd_data data = {};
            data.fd = fd;
            data.width = (uint32_t) size.width();
            data.height = (uint32_t) size.height();
            data.stride = stride;
            data.format = format;
            gbm_bo *importedBuffer = gbm_bo_import(m_gpu->gbmDevice(), GBM_BO_IMPORT_FD, &data, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
            if (!importedBuffer) {
                qCDebug(KWIN_DRM) << "failed to import dma-buf!" << strerror(errno);
            } else {
                if (output.directScanoutBuffer) {
                    gbm_bo_destroy(output.directScanoutBuffer);
                }
                output.directScanoutBuffer = importedBuffer;
            }
        }
    }
}

void EglGbmBackend::prepareRenderFramebuffer(const Output &output) const
{
    // When render.framebuffer is 0 we may just reset to the screen framebuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, output.render.framebuffer);
    GLRenderTarget::setKWinFramebuffer(output.render.framebuffer);
}

bool EglGbmBackend::makeContextCurrent(const Output &output) const
{
    Q_ASSERT(isPrimary());
    const EGLSurface surface = output.eglSurface;
    if (surface == EGL_NO_SURFACE) {
        return false;
    }
    if (eglMakeCurrent(eglDisplay(), surface, surface, context()) == EGL_FALSE) {
        qCCritical(KWIN_DRM) << "Make Context Current failed" << eglGetError();
        return false;
    }
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
        EGL_RENDERABLE_TYPE,      isOpenGLES() ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT,
        EGL_CONFIG_CAVEAT,        EGL_NONE,
        EGL_NONE,
    };

    EGLint count;
    EGLConfig configs[1024];
    if (!eglChooseConfig(eglDisplay(), config_attribs, configs,
                         sizeof(configs) / sizeof(EGLConfig),
                         &count)) {
        qCCritical(KWIN_DRM) << "choose config failed";
        return false;
    }

    qCDebug(KWIN_DRM) << "EGL buffer configs count:" << count;

    // Loop through all configs, choosing the first one that has suitable format.
    for (EGLint i = 0; i < count; i++) {
        EGLint gbmFormat;
        // Query some configuration parameters, to show in debug log.
        eglGetConfigAttrib(eglDisplay(), configs[i], EGL_NATIVE_VISUAL_ID, &gbmFormat);

        if (KWIN_DRM().isDebugEnabled()) {
            // GBM formats are declared as FOURCC code (integer from ASCII chars, so use this fact).
            char gbmFormatStr[sizeof(EGLint) + 1] = {0};
            memcpy(gbmFormatStr, &gbmFormat, sizeof(EGLint));

            // Query number of bits for color channel.
            EGLint blueSize, redSize, greenSize, alphaSize;
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_RED_SIZE, &redSize);
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_GREEN_SIZE, &greenSize);
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_BLUE_SIZE, &blueSize);
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_ALPHA_SIZE, &alphaSize);
            qCDebug(KWIN_DRM) << "  EGL config #" << i << " has GBM FOURCC format:" << gbmFormatStr
                              << "; color sizes (RGBA order):"
                              << redSize << greenSize << blueSize << alphaSize;
        }

        if ((gbmFormat == GBM_FORMAT_XRGB8888) || (gbmFormat == GBM_FORMAT_ARGB8888)) {
            setConfig(configs[i]);
            return true;
        }
    }

    qCCritical(KWIN_DRM) << "Choosing EGL config did not return a suitable config. There were"
                         << count << "configs.";
    return false;
}

static QVector<EGLint> regionToRects(const QRegion &region, AbstractWaylandOutput *output)
{
    const int height = output->modeSize().height();

    const QMatrix4x4 matrix = DrmOutput::logicalToNativeMatrix(output->geometry(),
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

void EglGbmBackend::aboutToStartPainting(int screenId, const QRegion &damagedRegion)
{
    Q_ASSERT_X(screenId != -1, "aboutToStartPainting", "not using per screen rendering");
    const Output &output = m_outputs.at(screenId);
    if (output.bufferAge > 0 && !damagedRegion.isEmpty() && supportsPartialUpdate()) {
        const QRegion region = damagedRegion & output.output->geometry();

        QVector<EGLint> rects = regionToRects(region, output.output);
        const bool correct = eglSetDamageRegionKHR(eglDisplay(), output.eglSurface,
                                                   rects.data(), rects.count()/4);
        if (!correct) {
            qCWarning(KWIN_DRM) << "failed eglSetDamageRegionKHR" << eglGetError();
        }
    }
}

bool EglGbmBackend::presentOnOutput(Output &output, const QRegion &damagedRegion)
{
    if (output.directScanoutBuffer) {
        output.buffer = new DrmSurfaceBuffer(m_gpu->fd(), output.directScanoutBuffer, output.bufferInterface);
    } else if (isPrimary()) {
        if (supportsSwapBuffersWithDamage()) {
            QVector<EGLint> rects = regionToRects(output.damageHistory.constFirst(), output.output);
            if (!eglSwapBuffersWithDamageEXT(eglDisplay(), output.eglSurface,
                                             rects.data(), rects.count() / 4)) {
                qCCritical(KWIN_DRM, "eglSwapBuffersWithDamageEXT() failed: %x", eglGetError());
                return false;
            }
        } else {
            if (!eglSwapBuffers(eglDisplay(), output.eglSurface)) {
                qCCritical(KWIN_DRM, "eglSwapBuffers() failed: %x", eglGetError());
                return false;
            }
        }
        output.buffer = new DrmSurfaceBuffer(m_gpu->fd(), output.gbmSurface);
    } else {
        qCDebug(KWIN_DRM) << "imported gbm_bo does not exist!";
        return false;
    }

    Q_EMIT output.output->outputChange(damagedRegion);
    if (!m_backend->present(output.buffer, output.output)) {
        return false;
    }

    if (supportsBufferAge()) {
        eglQuerySurface(eglDisplay(), output.eglSurface, EGL_BUFFER_AGE_EXT, &output.bufferAge);
    }
    return true;
}

SceneOpenGLTexturePrivate *EglGbmBackend::createBackendTexture(SceneOpenGLTexture *texture)
{
    return new EglGbmTexture(texture, this);
}

void EglGbmBackend::setViewport(const Output &output) const
{
    const QSize &overall = screens()->size();
    const QRect &v = output.output->geometry();
    qreal scale = output.output->scale();

    glViewport(-v.x() * scale, (v.height() - overall.height() + v.y()) * scale,
               overall.width() * scale, overall.height() * scale);
}

QRegion EglGbmBackend::beginFrame(int screenId)
{
    auto output = m_outputs[screenId];
    if (output.directScanoutBuffer) {
        gbm_bo_destroy(output.directScanoutBuffer);
        output.directScanoutBuffer = nullptr;
        output.surfaceInterface = nullptr;
        output.bufferInterface = nullptr;
    }
    if (isPrimary()) {
        return prepareRenderingForOutput(output);
    } else {
        return renderingBackend()->beginFrameForSecondaryGpu(output.output);
    }
}

QRegion EglGbmBackend::prepareRenderingForOutput(Output &output) const
{
    makeContextCurrent(output);
    prepareRenderFramebuffer(output);
    setViewport(output);

    if (supportsBufferAge()) {
        QRegion region;

        // Note: An age of zero means the buffer contents are undefined
        if (output.bufferAge > 0 && output.bufferAge <= output.damageHistory.count()) {
            for (int i = 0; i < output.bufferAge - 1; i++)
                region |= output.damageHistory[i];
        } else {
            region = output.output->geometry();
        }

        return region;
    }
    return output.output->geometry();
}

void EglGbmBackend::endFrame(int screenId, const QRegion &renderedRegion,
                             const QRegion &damagedRegion)
{
    Q_UNUSED(renderedRegion)

    Output &output = m_outputs[screenId];
    DrmOutput *drmOutput = output.output;

    renderFramebufferToSurface(output);

    if (!presentOnOutput(output, damagedRegion)) {
        output.damageHistory.clear();
        RenderLoopPrivate *renderLoopPrivate = RenderLoopPrivate::get(drmOutput->renderLoop());
        renderLoopPrivate->notifyFrameFailed();
        return;
    }

    if (supportsBufferAge()) {
        const QRegion dirty = damagedRegion.intersected(output.output->geometry());
        if (output.damageHistory.count() > 10) {
            output.damageHistory.removeLast();
        }
        output.damageHistory.prepend(dirty);
    }
}

bool EglGbmBackend::scanout(int screenId, SurfaceItem *surfaceItem)
{
    SurfaceItemWayland *item = qobject_cast<SurfaceItemWayland *>(surfaceItem);
    if (!item) {
        return false;
    }

    KWaylandServer::SurfaceInterface *surface = item->surface();
    if (!surface || !surface->buffer() || !surface->buffer()->linuxDmabufBuffer()) {
        return false;
    }
    auto buffer = surface->buffer();
    Output output = m_outputs[screenId];
    if (buffer->linuxDmabufBuffer()->size() != output.output->modeSize()
        && output.output->isBeingRecorded()) {
        return false;
    }
    EglDmabufBuffer *dmabuf = static_cast<EglDmabufBuffer*>(buffer->linuxDmabufBuffer());
    if (!dmabuf || !dmabuf->planes().count() ||
        !gbm_device_is_format_supported(m_gpu->gbmDevice(), dmabuf->format(), GBM_BO_USE_SCANOUT)) {
        return false;
    }
    gbm_bo *importedBuffer;
    if (dmabuf->planes()[0].modifier != DRM_FORMAT_MOD_INVALID
        || dmabuf->planes()[0].offset > 0
        || dmabuf->planes().size() > 1) {
        gbm_import_fd_modifier_data data = {};
        data.format = dmabuf->format();
        data.width = (uint32_t) dmabuf->size().width();
        data.height = (uint32_t) dmabuf->size().height();
        data.num_fds = dmabuf->planes().count();
        data.modifier = dmabuf->planes()[0].modifier;
        for (int i = 0; i < dmabuf->planes().count(); i++) {
            auto plane = dmabuf->planes()[i];
            data.fds[i] = plane.fd;
            data.offsets[i] = plane.offset;
            data.strides[i] = plane.stride;
        }
        importedBuffer = gbm_bo_import(m_gpu->gbmDevice(), GBM_BO_IMPORT_FD_MODIFIER, &data, GBM_BO_USE_SCANOUT);
    } else {
        auto plane = dmabuf->planes()[0];
        gbm_import_fd_data data = {};
        data.fd = plane.fd;
        data.width = (uint32_t) dmabuf->size().width();
        data.height = (uint32_t) dmabuf->size().height();
        data.stride = plane.stride;
        data.format = dmabuf->format();
        importedBuffer = gbm_bo_import(m_gpu->gbmDevice(), GBM_BO_IMPORT_FD, &data, GBM_BO_USE_SCANOUT);
    }
    if (!importedBuffer) {
        qCDebug(KWIN_DRM) << "importing the dmabuf for direct scanout failed:" << strerror(errno);
        return false;
    }
    // damage tracking for screen casting
    QRegion damage;
    if (output.surfaceInterface == surface && buffer->size() == output.output->modeSize()) {
        QRegion trackedDamage = surfaceItem->damage();
        surfaceItem->resetDamage();
        for (const auto &rect : trackedDamage) {
            auto damageRect = QRect(rect);
            damageRect.translate(output.output->geometry().topLeft());
            damage |= damageRect;
        }
    } else {
        damage = output.output->geometry();
    }
    if (output.directScanoutBuffer) {
        gbm_bo_destroy(output.directScanoutBuffer);
    }
    output.directScanoutBuffer = importedBuffer;
    output.surfaceInterface = surface;
    output.bufferInterface = buffer;
    return presentOnOutput(output, damage);
}

QSharedPointer<GLTexture> EglGbmBackend::textureForOutput(AbstractOutput *abstractOutput) const
{
    const QVector<KWin::EglGbmBackend::Output>::const_iterator itOutput = std::find_if(m_outputs.begin(), m_outputs.end(),
        [abstractOutput] (const auto &output) {
            return output.output == abstractOutput;
        }
    );
    if (itOutput == m_outputs.end()) {
        return {};
    }

    DrmOutput *drmOutput = itOutput->output;
    if (!drmOutput->hardwareTransforms()) {
        const auto glTexture = QSharedPointer<KWin::GLTexture>::create(itOutput->render.texture, GL_RGBA8, drmOutput->pixelSize());
        glTexture->setYInverted(true);
        return glTexture;
    }

    EGLImageKHR image = eglCreateImageKHR(eglDisplay(), nullptr, EGL_NATIVE_PIXMAP_KHR,
                                          itOutput->directScanoutBuffer ? itOutput->directScanoutBuffer : itOutput->buffer->getBo(), nullptr);
    if (image == EGL_NO_IMAGE_KHR) {
        qCWarning(KWIN_DRM) << "Failed to record frame: Error creating EGLImageKHR - " << glGetError();
        return {};
    }

    return QSharedPointer<EGLImageTexture>::create(eglDisplay(), image, GL_RGBA8, drmOutput->modeSize());
}

bool EglGbmBackend::directScanoutAllowed(int screen) const
{
    return !m_backend->usesSoftwareCursor() && !m_outputs[screen].output->directScanoutInhibited();
}

/************************************************
 * EglTexture
 ************************************************/

EglGbmTexture::EglGbmTexture(KWin::SceneOpenGLTexture *texture, EglGbmBackend *backend)
    : AbstractEglTexture(texture, backend)
{
}

EglGbmTexture::~EglGbmTexture() = default;

}

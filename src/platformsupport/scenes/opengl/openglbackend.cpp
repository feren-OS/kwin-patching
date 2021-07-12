/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2009, 2010, 2011 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "openglbackend.h"
#include <kwineffects.h>
#include <logging.h>

#include "screens.h"

#include <epoxy/gl.h>

namespace KWin
{

OpenGLBackend::OpenGLBackend()
    : m_directRendering(false)
    , m_haveBufferAge(false)
    , m_failed(false)
{
}

OpenGLBackend::~OpenGLBackend()
{
}

void OpenGLBackend::setFailed(const QString &reason)
{
    qCWarning(KWIN_OPENGL) << "Creating the OpenGL rendering failed: " << reason;
    m_failed = true;
}

void OpenGLBackend::addToDamageHistory(const QRegion &region)
{
    if (m_damageHistory.count() > 10)
        m_damageHistory.removeLast();

    m_damageHistory.prepend(region);
}

QRegion OpenGLBackend::accumulatedDamageHistory(int bufferAge) const
{
    QRegion region;

    // Note: An age of zero means the buffer contents are undefined
    if (bufferAge > 0 && bufferAge <= m_damageHistory.count()) {
        for (int i = 0; i < bufferAge - 1; i++)
            region |= m_damageHistory[i];
    } else {
        const QSize &s = screens()->size();
        region = QRegion(0, 0, s.width(), s.height());
    }

    return region;
}

OverlayWindow* OpenGLBackend::overlayWindow() const
{
    return nullptr;
}

bool OpenGLBackend::scanout(int screenId, SurfaceItem *surfaceItem)
{
    Q_UNUSED(screenId)
    Q_UNUSED(surfaceItem)
    return false;
}

void OpenGLBackend::copyPixels(const QRegion &region)
{
    const int height = screens()->size().height();
    for (const QRect &r : region) {
        const int x0 = r.x();
        const int y0 = height - r.y() - r.height();
        const int x1 = r.x() + r.width();
        const int y1 = height - r.y();

        glBlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
}

QSharedPointer<KWin::GLTexture> OpenGLBackend::textureForOutput(AbstractOutput* output) const
{
    Q_UNUSED(output)
    return {};
}

void OpenGLBackend::aboutToStartPainting(int screenId, const QRegion &damage)
{
    Q_UNUSED(screenId)
    Q_UNUSED(damage)
}


bool OpenGLBackend::directScanoutAllowed(int screen) const
{
    Q_UNUSED(screen);
    return false;
}

}

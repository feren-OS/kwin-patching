/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "scene_qpainter_x11_backend.h"
#include "main.h"
#include "screens.h"
#include "softwarevsyncmonitor.h"
#include "x11windowed_backend.h"
#include "x11windowed_output.h"

namespace KWin
{
X11WindowedQPainterBackend::X11WindowedQPainterBackend(X11WindowedBackend *backend)
    : QPainterBackend()
    , m_backend(backend)
{
    connect(screens(), &Screens::changed, this, &X11WindowedQPainterBackend::createOutputs);
    createOutputs();
}

X11WindowedQPainterBackend::~X11WindowedQPainterBackend()
{
    qDeleteAll(m_outputs);
    if (m_gc) {
        xcb_free_gc(m_backend->connection(), m_gc);
    }
}

void X11WindowedQPainterBackend::createOutputs()
{
    qDeleteAll(m_outputs);
    m_outputs.clear();
    for (int i = 0; i < screens()->count(); ++i) {
        Output *output = new Output;
        output->window = m_backend->windowForScreen(i);
        output->buffer = QImage(screens()->size(i) * screens()->scale(i), QImage::Format_RGB32);
        output->buffer.fill(Qt::black);
        m_outputs << output;
    }
}

QImage *X11WindowedQPainterBackend::bufferForScreen(int screen)
{
    return &m_outputs.at(screen)->buffer;
}

bool X11WindowedQPainterBackend::needsFullRepaint(int screenId) const
{
    const Output *rendererOutput = m_outputs.value(screenId);
    Q_ASSERT(rendererOutput);
    return rendererOutput->needsFullRepaint;
}

void X11WindowedQPainterBackend::beginFrame(int screenId)
{
    Q_UNUSED(screenId)
}

void X11WindowedQPainterBackend::endFrame(int screenId, int mask, const QRegion &damage)
{
    Q_UNUSED(mask)
    Q_UNUSED(damage)

    X11WindowedOutput *output = static_cast<X11WindowedOutput *>(kwinApp()->platform()->findOutput(screenId));
    output->vsyncMonitor()->arm();

    xcb_connection_t *c = m_backend->connection();
    const xcb_window_t window = m_backend->window();
    if (m_gc == XCB_NONE) {
        m_gc = xcb_generate_id(c);
        xcb_create_gc(c, m_gc, window, 0, nullptr);
    }

    Output *rendererOutput = m_outputs.value(screenId);
    Q_ASSERT(rendererOutput);

    // TODO: only update changes?
    const QImage &buffer = rendererOutput->buffer;
    xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, rendererOutput->window,
                  m_gc, buffer.width(), buffer.height(), 0, 0, 0, 24,
                  buffer.sizeInBytes(), buffer.constBits());

    rendererOutput->needsFullRepaint = false;
}

}

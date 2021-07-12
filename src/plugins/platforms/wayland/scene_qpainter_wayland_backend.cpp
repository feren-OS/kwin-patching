/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2019 Roman Gilg <subdiff@gmail.com>
    SPDX-FileCopyrightText: 2013, 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "scene_qpainter_wayland_backend.h"
#include "wayland_backend.h"
#include "wayland_output.h"

#include "composite.h"
#include "logging.h"

#include <KWayland/Client/buffer.h>
#include <KWayland/Client/shm_pool.h>
#include <KWayland/Client/surface.h>

#include <cmath>

namespace KWin
{
namespace Wayland
{

WaylandQPainterOutput::WaylandQPainterOutput(WaylandOutput *output, QObject *parent)
    : QObject(parent)
    , m_waylandOutput(output)
{
}

WaylandQPainterOutput::~WaylandQPainterOutput()
{
    if (m_buffer) {
        m_buffer.toStrongRef()->setUsed(false);
    }
}

bool WaylandQPainterOutput::needsFullRepaint() const
{
    return m_needsFullRepaint;
}

void WaylandQPainterOutput::setNeedsFullRepaint(bool set)
{
    m_needsFullRepaint = set;
}

bool WaylandQPainterOutput::init(KWayland::Client::ShmPool *pool)
{
    m_pool = pool;
    m_backBuffer = QImage(QSize(), QImage::Format_RGB32);

    connect(pool, &KWayland::Client::ShmPool::poolResized, this, &WaylandQPainterOutput::remapBuffer);
    connect(m_waylandOutput, &WaylandOutput::sizeChanged, this, &WaylandQPainterOutput::updateSize);

    return true;
}

void WaylandQPainterOutput::remapBuffer()
{
    if (!m_buffer) {
        return;
    }
    auto b = m_buffer.toStrongRef();
    if (!b->isUsed()){
        return;
    }
    const QSize size = m_backBuffer.size();
    m_backBuffer = QImage(b->address(), size.width(), size.height(), QImage::Format_RGB32);
    qCDebug(KWIN_WAYLAND_BACKEND) << "Remapped back buffer of surface" << m_waylandOutput->surface();
}

void WaylandQPainterOutput::updateSize(const QSize &size)
{
    Q_UNUSED(size)
    if (!m_buffer) {
        return;
    }
    m_buffer.toStrongRef()->setUsed(false);
    m_buffer.clear();
}

void WaylandQPainterOutput::present(const QRegion &damage)
{
    auto s = m_waylandOutput->surface();
    s->attachBuffer(m_buffer);
    s->damage(damage);
    s->setScale(std::ceil(m_waylandOutput->scale()));
    s->commit();
}

void WaylandQPainterOutput::prepareRenderingFrame()
{
    if (m_buffer) {
        auto b = m_buffer.toStrongRef();
        if (b->isReleased()) {
            // we can re-use this buffer
            b->setReleased(false);
            return;
        } else {
            // buffer is still in use, get a new one
            b->setUsed(false);
        }
    }
    m_buffer.clear();

    const QSize nativeSize(m_waylandOutput->geometry().size() * m_waylandOutput->scale());

    m_buffer = m_pool->getBuffer(nativeSize, nativeSize.width() * 4);
    if (!m_buffer) {
        qCDebug(KWIN_WAYLAND_BACKEND) << "Did not get a new Buffer from Shm Pool";
        m_backBuffer = QImage();
        return;
    }

    auto b = m_buffer.toStrongRef();
    b->setUsed(true);

    m_backBuffer = QImage(b->address(), nativeSize.width(), nativeSize.height(), QImage::Format_RGB32);
    m_backBuffer.fill(Qt::transparent);
//    qCDebug(KWIN_WAYLAND_BACKEND) << "Created a new back buffer for output surface" << m_waylandOutput->surface();
}

QRegion WaylandQPainterOutput::mapToLocal(const QRegion &region) const
{
    return region.translated(-m_waylandOutput->geometry().topLeft());
}

WaylandQPainterBackend::WaylandQPainterBackend(Wayland::WaylandBackend *b)
    : QPainterBackend()
    , m_backend(b)
{

    const auto waylandOutputs = m_backend->waylandOutputs();
    for (auto *output: waylandOutputs) {
        createOutput(output);
    }
    connect(m_backend, &WaylandBackend::outputAdded, this, &WaylandQPainterBackend::createOutput);
    connect(m_backend, &WaylandBackend::outputRemoved, this,
        [this] (AbstractOutput *waylandOutput) {
            auto it = std::find_if(m_outputs.begin(), m_outputs.end(),
                [waylandOutput] (WaylandQPainterOutput *output) {
                    return output->m_waylandOutput == waylandOutput;
                }
            );
            if (it == m_outputs.end()) {
                return;
            }
            delete *it;
            m_outputs.erase(it);
        }
    );
}

WaylandQPainterBackend::~WaylandQPainterBackend()
{
}

void WaylandQPainterBackend::createOutput(AbstractOutput *waylandOutput)
{
    auto *output = new WaylandQPainterOutput(static_cast<WaylandOutput *>(waylandOutput), this);
    output->init(m_backend->shmPool());
    m_outputs << output;
}

void WaylandQPainterBackend::endFrame(int screenId, int mask, const QRegion &damage)
{
    Q_UNUSED(mask)

    WaylandQPainterOutput *rendererOutput = m_outputs.value(screenId);
    Q_ASSERT(rendererOutput);

    rendererOutput->setNeedsFullRepaint(false);
    rendererOutput->present(rendererOutput->mapToLocal(damage));
}

QImage *WaylandQPainterBackend::bufferForScreen(int screenId)
{
    auto *output = m_outputs[screenId];
    return &output->m_backBuffer;
}

void WaylandQPainterBackend::beginFrame(int screenId)
{
    WaylandQPainterOutput *rendererOutput = m_outputs.value(screenId);
    Q_ASSERT(rendererOutput);

    rendererOutput->prepareRenderingFrame();
    rendererOutput->setNeedsFullRepaint(true);
}

bool WaylandQPainterBackend::needsFullRepaint(int screenId) const
{
    const WaylandQPainterOutput *rendererOutput = m_outputs.value(screenId);
    Q_ASSERT(rendererOutput);
    return rendererOutput->needsFullRepaint();
}

}
}

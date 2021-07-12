/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef KWIN_SCENE_QPAINTER_FB_BACKEND_H
#define KWIN_SCENE_QPAINTER_FB_BACKEND_H
#include "qpainterbackend.h"

#include <QObject>
#include <QImage>

namespace KWin
{
class FramebufferBackend;

class FramebufferQPainterBackend : public QObject, public QPainterBackend
{
    Q_OBJECT
public:
    FramebufferQPainterBackend(FramebufferBackend *backend);
    ~FramebufferQPainterBackend() override;

    QImage *bufferForScreen(int screenId) override;
    bool needsFullRepaint(int screenId) const override;
    void beginFrame(int screenId) override;
    void endFrame(int screenId, int mask, const QRegion &damage) override;

private:
    void reactivate();
    void deactivate();

    /**
     * @brief mapped memory buffer on fb device
     */
    QImage m_renderBuffer;
    /**
     * @brief buffer to draw into
     */
    QImage m_backBuffer;

    FramebufferBackend *m_backend;
    bool m_needsFullRepaint;
};

}

#endif

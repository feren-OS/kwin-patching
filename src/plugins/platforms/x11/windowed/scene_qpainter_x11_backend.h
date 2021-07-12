/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef KWIN_SCENE_QPAINTER_X11_BACKEND_H
#define KWIN_SCENE_QPAINTER_X11_BACKEND_H

#include "qpainterbackend.h"

#include <QObject>
#include <QImage>
#include <QVector>

#include <xcb/xcb.h>

namespace KWin
{

class X11WindowedBackend;

class X11WindowedQPainterBackend : public QObject, public QPainterBackend
{
    Q_OBJECT
public:
    X11WindowedQPainterBackend(X11WindowedBackend *backend);
    ~X11WindowedQPainterBackend() override;

    QImage *bufferForScreen(int screenId) override;
    bool needsFullRepaint(int screenId) const override;
    void beginFrame(int screenId) override;
    void endFrame(int screenId, int mask, const QRegion &damage) override;

private:
    void createOutputs();
    xcb_gcontext_t m_gc = XCB_NONE;
    X11WindowedBackend *m_backend;
    struct Output {
        xcb_window_t window;
        QImage buffer;
        bool needsFullRepaint = true;
    };
    QVector<Output*> m_outputs;
};

}

#endif

/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef KWIN_SCENE_QPAINTER_VIRTUAL_BACKEND_H
#define KWIN_SCENE_QPAINTER_VIRTUAL_BACKEND_H

#include "qpainterbackend.h"

#include <QObject>
#include <QVector>

namespace KWin
{

class VirtualBackend;

class VirtualQPainterBackend : public QObject, public QPainterBackend
{
    Q_OBJECT
public:
    VirtualQPainterBackend(VirtualBackend *backend);
    ~VirtualQPainterBackend() override;

    QImage *bufferForScreen(int screenId) override;
    bool needsFullRepaint(int screenId) const override;
    void beginFrame(int screenId) override;
    void endFrame(int screenId, int mask, const QRegion &damage) override;

private:
    void createOutputs();

    QVector<QImage> m_backBuffers;
    VirtualBackend *m_backend;
    int m_frameCounter = 0;
};

}

#endif

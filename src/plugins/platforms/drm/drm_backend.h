/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef KWIN_DRM_BACKEND_H
#define KWIN_DRM_BACKEND_H
#include "platform.h"
#include "input.h"

#include "drm_buffer.h"
#if HAVE_GBM
#include "drm_buffer_gbm.h"
#endif
#include "drm_inputeventfilter.h"
#include "drm_pointer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QSize>
#include <QVector>
#include <xf86drmMode.h>

#include <memory>

struct gbm_bo;
struct gbm_device;
struct gbm_surface;

namespace KWin
{

class Udev;
class UdevMonitor;

class DrmOutput;
class DrmPlane;
class DrmCrtc;
class DrmConnector;
class GbmSurface;
class Cursor;
class DrmGpu;

class KWIN_EXPORT DrmBackend : public Platform
{
    Q_OBJECT
    Q_INTERFACES(KWin::Platform)
    Q_PLUGIN_METADATA(IID "org.kde.kwin.Platform" FILE "drm.json")
public:
    explicit DrmBackend(QObject *parent = nullptr);
    ~DrmBackend() override;

    QPainterBackend *createQPainterBackend() override;
    OpenGLBackend* createOpenGLBackend() override;
    DmaBufTexture *createDmaBufTexture(const QSize &size) override;
    Session *session() const override;
    bool initialize() override;
    void prepareShutdown() override;

    bool present(DrmBuffer *buffer, DrmOutput *output);

    Outputs outputs() const override;
    Outputs enabledOutputs() const override;
    QVector<DrmOutput*> drmOutputs() const {
        return m_outputs;
    }
    QVector<DrmOutput*> drmEnabledOutputs() const {
        return m_enabledOutputs;
    }

    void enableOutput(DrmOutput *output, bool enable);

    void createDpmsFilter();
    void checkOutputsAreOn();

    QVector<CompositingType> supportedCompositors() const override;

    QString supportInformation() const override;

public Q_SLOTS:
    void turnOutputsOn();

protected:
    void doHideCursor() override;
    void doShowCursor() override;
    void doSetSoftwareCursor() override;

private:
    friend class DrmGpu;
    void addOutput(DrmOutput* output);
    void removeOutput(DrmOutput* output);
    static void pageFlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data);
    void activate(bool active);
    void reactivate();
    void deactivate();
    bool updateOutputs();
    void updateCursor();
    void moveCursor();
    void initCursor();
    void readOutputsConfiguration();
    void writeOutputsConfiguration();
    QByteArray generateOutputConfigurationUuid() const;
    DrmOutput *findOutput(quint32 connector);
    void updateOutputsEnabled();
    QScopedPointer<Udev> m_udev;
    QScopedPointer<UdevMonitor> m_udevMonitor;
    Session *m_session = nullptr;
    // active output pipelines (planes + crtc + encoder + connector)
    QVector<DrmOutput*> m_outputs;
    // active and enabled pipelines (above + wl_output)
    QVector<DrmOutput*> m_enabledOutputs;

    bool m_active = false;
    QVector<DrmGpu*> m_gpus;
    QScopedPointer<DpmsInputEventFilter> m_dpmsFilter;
};


}

#endif


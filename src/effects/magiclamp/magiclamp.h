/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2008 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef KWIN_MAGICLAMP_H
#define KWIN_MAGICLAMP_H

#include <kwineffects.h>

namespace KWin
{

struct MagicLampAnimation
{
    TimeLine timeLine;
    std::chrono::milliseconds lastPresentTime = std::chrono::milliseconds::zero();
};

class MagicLampEffect
    : public Effect
{
    Q_OBJECT

public:
    MagicLampEffect();

    void reconfigure(ReconfigureFlags) override;
    void prePaintScreen(ScreenPrePaintData& data, std::chrono::milliseconds presentTime) override;
    void prePaintWindow(EffectWindow* w, WindowPrePaintData& data, std::chrono::milliseconds presentTime) override;
    void paintWindow(EffectWindow* w, int mask, QRegion region, WindowPaintData& data) override;
    void postPaintScreen() override;
    bool isActive() const override;

    int requestedEffectChainPosition() const override {
        return 50;
    }

    static bool supported();

public Q_SLOTS:
    void slotWindowDeleted(KWin::EffectWindow *w);
    void slotWindowMinimized(KWin::EffectWindow *w);
    void slotWindowUnminimized(KWin::EffectWindow *w);

private:
    std::chrono::milliseconds m_duration;
    QHash<const EffectWindow *, MagicLampAnimation> m_animations;

    enum IconPosition {
        Top,
        Bottom,
        Left,
        Right
    };
};

} // namespace

#endif

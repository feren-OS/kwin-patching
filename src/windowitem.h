/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "item.h"

namespace KDecoration2
{
class Decoration;
}

namespace KWin
{

/**
 * The WindowItem class represents a window in the scene.
 *
 * A WindowItem is made of a surface with client contents and optionally a server-side frame
 * and a drop-shadow.
 */
class KWIN_EXPORT WindowItem : public Item
{
    Q_OBJECT

public:
    QPoint windowPosition() const override;

    SurfaceItem *surfaceItem() const;
    DecorationItem *decorationItem() const;
    ShadowItem *shadowItem() const;

    void setShadow(Shadow *shadow);

protected:
    explicit WindowItem(Scene::Window *window, Item *parent = nullptr);
    void updateSurfaceItem(SurfaceItem *surfaceItem);

private Q_SLOTS:
    void updateDecorationItem();
    void updateSurfacePosition();

private:
    QScopedPointer<SurfaceItem> m_surfaceItem;
    QScopedPointer<DecorationItem> m_decorationItem;
    QScopedPointer<ShadowItem> m_shadowItem;
};

/**
 * The WindowItemX11 class represents an X11 window (both on X11 and Wayland sessions).
 *
 * Note that Xwayland windows and Wayland surfaces are associated asynchronously. This means
 * that the surfaceItem() function can return @c null until the window is fully initialized.
 */
class KWIN_EXPORT WindowItemX11 : public WindowItem
{
    Q_OBJECT

public:
    explicit WindowItemX11(Scene::Window *window, Item *parent = nullptr);

private Q_SLOTS:
    void initialize();
};

/**
 * The WindowItemWayland class represents a Wayland window.
 */
class KWIN_EXPORT WindowItemWayland : public WindowItem
{
    Q_OBJECT

public:
    explicit WindowItemWayland(Scene::Window *window, Item *parent = nullptr);
};

/**
 * The WindowItemInternal class represents a window created by the compositor, for
 * example, the task switcher, etc.
 */
class KWIN_EXPORT WindowItemInternal : public WindowItem
{
    Q_OBJECT

public:
    explicit WindowItemInternal(Scene::Window *window, Item *parent = nullptr);
};

} // namespace KWin

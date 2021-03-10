/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

/*
 The base class for compositing, implementing shared functionality
 between the OpenGL and XRender backends.

 Design:

 When compositing is turned on, XComposite extension is used to redirect
 drawing of windows to pixmaps and XDamage extension is used to get informed
 about damage (changes) to window contents. This code is mostly in composite.cpp .

 Compositor::performCompositing() starts one painting pass. Painting is done
 by painting the screen, which in turn paints every window. Painting can be affected
 using effects, which are chained. E.g. painting a screen means that actually
 paintScreen() of the first effect is called, which possibly does modifications
 and calls next effect's paintScreen() and so on, until Scene::finalPaintScreen()
 is called.

 There are 3 phases of every paint (not necessarily done together):
 The pre-paint phase, the paint phase and the post-paint phase.

 The pre-paint phase is used to find out about how the painting will be actually
 done (i.e. what the effects will do). For example when only a part of the screen
 needs to be updated and no effect will do any transformation it is possible to use
 an optimized paint function. How the painting will be done is controlled
 by the mask argument, see PAINT_WINDOW_* and PAINT_SCREEN_* flags in scene.h .
 For example an effect that decides to paint a normal windows as translucent
 will need to modify the mask in its prePaintWindow() to include
 the PAINT_WINDOW_TRANSLUCENT flag. The paintWindow() function will then get
 the mask with this flag turned on and will also paint using transparency.

 The paint pass does the actual painting, based on the information collected
 using the pre-paint pass. After running through the effects' paintScreen()
 either paintGenericScreen() or optimized paintSimpleScreen() are called.
 Those call paintWindow() on windows (not necessarily all), possibly using
 clipping to optimize performance and calling paintWindow() first with only
 PAINT_WINDOW_OPAQUE to paint the opaque parts and then later
 with PAINT_WINDOW_TRANSLUCENT to paint the transparent parts. Function
 paintWindow() again goes through effects' paintWindow() until
 finalPaintWindow() is called, which calls the window's performPaint() to
 do the actual painting.

 The post-paint can be used for cleanups and is also used for scheduling
 repaints during the next painting pass for animations. Effects wanting to
 repaint certain parts can manually damage them during post-paint and repaint
 of these parts will be done during the next paint pass.

*/

#include "scene.h"
#include "abstract_output.h"
#include "decorationitem.h"
#include "internal_client.h"
#include "platform.h"
#include "shadowitem.h"
#include "surfaceitem.h"
#include "unmanaged.h"
#include "waylandclient.h"
#include "windowitem.h"
#include "x11client.h"

#include <QQuickWindow>
#include <QVector2D>

#include "x11client.h"
#include "deleted.h"
#include "effects.h"
#include "overlaywindow.h"
#include "renderloop.h"
#include "screens.h"
#include "shadow.h"
#include "wayland_server.h"
#include "thumbnailitem.h"
#include "composite.h"

#include <KWaylandServer/buffer_interface.h>
#include <KWaylandServer/subcompositor_interface.h>
#include <KWaylandServer/surface_interface.h>

namespace KWin
{

//****************************************
// Scene
//****************************************

Scene::Scene(QObject *parent)
    : QObject(parent)
{
    if (kwinApp()->platform()->isPerScreenRenderingEnabled()) {
        connect(kwinApp()->platform(), &Platform::outputEnabled, this, &Scene::reallocRepaints);
        connect(kwinApp()->platform(), &Platform::outputDisabled, this, &Scene::reallocRepaints);
    }
    reallocRepaints();
}

Scene::~Scene()
{
    Q_ASSERT(m_windows.isEmpty());
}

void Scene::addRepaint(const QRegion &region)
{
    if (kwinApp()->platform()->isPerScreenRenderingEnabled()) {
        const QVector<AbstractOutput *> outputs = kwinApp()->platform()->enabledOutputs();
        if (m_repaints.count() != outputs.count()) {
            return; // Repaints haven't been reallocated yet, do nothing.
        }
        for (int screenId = 0; screenId < m_repaints.count(); ++screenId) {
            AbstractOutput *output = outputs[screenId];
            const QRegion dirtyRegion = region & output->geometry();
            if (!dirtyRegion.isEmpty()) {
                m_repaints[screenId] += dirtyRegion;
                output->renderLoop()->scheduleRepaint();
            }
        }
    } else {
        m_repaints[0] += region;
        kwinApp()->platform()->renderLoop()->scheduleRepaint();
    }
}

QRegion Scene::repaints(int screenId) const
{
    const int index = screenId == -1 ? 0 : screenId;
    return m_repaints[index];
}

void Scene::resetRepaints(int screenId)
{
    const int index = screenId == -1 ? 0 : screenId;
    m_repaints[index] = QRegion();
}

void Scene::reallocRepaints()
{
    if (kwinApp()->platform()->isPerScreenRenderingEnabled()) {
        m_repaints.resize(kwinApp()->platform()->enabledOutputs().count());
    } else {
        m_repaints.resize(1);
    }

    m_repaints.fill(infiniteRegion());
}

// returns mask and possibly modified region
void Scene::paintScreen(int* mask, const QRegion &damage, const QRegion &repaint,
                        QRegion *updateRegion, QRegion *validRegion, RenderLoop *renderLoop,
                        const QMatrix4x4 &projection)
{
    const QSize &screenSize = screens()->size();
    const QRegion displayRegion(0, 0, screenSize.width(), screenSize.height());
    *mask = (damage == displayRegion) ? 0 : PAINT_SCREEN_REGION;

    const std::chrono::milliseconds presentTime =
            std::chrono::duration_cast<std::chrono::milliseconds>(renderLoop->nextPresentationTimestamp());

    if (Q_UNLIKELY(presentTime < m_expectedPresentTimestamp)) {
        qCDebug(KWIN_CORE, "Provided presentation timestamp is invalid: %ld (current: %ld)",
                presentTime.count(), m_expectedPresentTimestamp.count());
    } else {
        m_expectedPresentTimestamp = presentTime;
    }

    // preparation step
    static_cast<EffectsHandlerImpl*>(effects)->startPaint();

    QRegion region = damage;

    ScreenPrePaintData pdata;
    pdata.mask = *mask;
    pdata.paint = region;

    effects->prePaintScreen(pdata, m_expectedPresentTimestamp);
    *mask = pdata.mask;
    region = pdata.paint;

    if (*mask & (PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS)) {
        // Region painting is not possible with transformations,
        // because screen damage doesn't match transformed positions.
        *mask &= ~PAINT_SCREEN_REGION;
        region = infiniteRegion();
    } else if (*mask & PAINT_SCREEN_REGION) {
        // make sure not to go outside visible screen
        region &= displayRegion;
    } else {
        // whole screen, not transformed, force region to be full
        region = displayRegion;
    }

    painted_region = region;
    repaint_region = repaint;

    ScreenPaintData data(projection, effects->findScreen(painted_screen));
    effects->paintScreen(*mask, region, data);

    foreach (Window *w, stacking_order) {
        effects->postPaintWindow(effectWindow(w));
    }

    effects->postPaintScreen();

    // make sure not to go outside of the screen area
    *updateRegion = damaged_region;
    *validRegion = (region | painted_region) & displayRegion;

    repaint_region = QRegion();
    damaged_region = QRegion();

    m_paintScreenCount = 0;

    // make sure all clipping is restored
    Q_ASSERT(!PaintClipper::clip());
}

// the function that'll be eventually called by paintScreen() above
void Scene::finalPaintScreen(int mask, const QRegion &region, ScreenPaintData& data)
{
    m_paintScreenCount++;
    if (mask & (PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS))
        paintGenericScreen(mask, data);
    else
        paintSimpleScreen(mask, region);

    Q_EMIT frameRendered();
}

// The generic painting code that can handle even transformations.
// It simply paints bottom-to-top.
void Scene::paintGenericScreen(int orig_mask, const ScreenPaintData &)
{
    QVector<Phase2Data> phase2;
    phase2.reserve(stacking_order.size());
    foreach (Window * w, stacking_order) { // bottom to top
        // Let the scene window update the window pixmap tree.
        w->preprocess(w->windowItem());

        // Reset the repaint_region.
        // This has to be done here because many effects schedule a repaint for
        // the next frame within Effects::prePaintWindow.
        w->resetRepaints(painted_screen);

        WindowPrePaintData data;
        data.mask = orig_mask | (w->isOpaque() ? PAINT_WINDOW_OPAQUE : PAINT_WINDOW_TRANSLUCENT);
        w->resetPaintingEnabled();
        data.paint = infiniteRegion(); // no clipping, so doesn't really matter
        data.clip = QRegion();
        data.quads = w->buildQuads();
        // preparation step
        effects->prePaintWindow(effectWindow(w), data, m_expectedPresentTimestamp);
#if !defined(QT_NO_DEBUG)
        if (data.quads.isTransformed()) {
            qFatal("Pre-paint calls are not allowed to transform quads!");
        }
#endif
        if (!w->isPaintingEnabled()) {
            continue;
        }
        phase2.append({w, infiniteRegion(), data.clip, data.mask, data.quads});
    }

    damaged_region = QRegion(QRect {{}, screens()->size()});
    if (m_paintScreenCount == 1) {
        aboutToStartPainting(painted_screen, damaged_region);

        if (orig_mask & PAINT_SCREEN_BACKGROUND_FIRST) {
            paintBackground(infiniteRegion());
        }
    }

    if (!(orig_mask & PAINT_SCREEN_BACKGROUND_FIRST)) {
        paintBackground(infiniteRegion());
    }
    foreach (const Phase2Data & d, phase2) {
        paintWindow(d.window, d.mask, d.region, d.quads);
    }
}

// The optimized case without any transformations at all.
// It can paint only the requested region and can use clipping
// to reduce painting and improve performance.
void Scene::paintSimpleScreen(int orig_mask, const QRegion &region)
{
    Q_ASSERT((orig_mask & (PAINT_SCREEN_TRANSFORMED
                         | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS)) == 0);
    QVector<Phase2Data> phase2data;
    phase2data.reserve(stacking_order.size());

    QRegion dirtyArea = region;
    bool opaqueFullscreen = false;

    // Traverse the scene windows from bottom to top.
    for (int i = 0; i < stacking_order.count(); ++i) {
        Window *window = stacking_order[i];
        Toplevel *toplevel = window->window();
        WindowPrePaintData data;
        data.mask = orig_mask | (window->isOpaque() ? PAINT_WINDOW_OPAQUE : PAINT_WINDOW_TRANSLUCENT);
        window->resetPaintingEnabled();
        data.paint = region;
        data.paint |= window->repaints(painted_screen);

        // Let the scene window update the window pixmap tree.
        window->preprocess(window->windowItem());

        // Reset the repaint_region.
        // This has to be done here because many effects schedule a repaint for
        // the next frame within Effects::prePaintWindow.
        window->resetRepaints(painted_screen);

        // Clip out the decoration for opaque windows; the decoration is drawn in the second pass
        opaqueFullscreen = false; // TODO: do we care about unmanged windows here (maybe input windows?)
        AbstractClient *client = dynamic_cast<AbstractClient *>(toplevel);
        if (window->isOpaque()) {
            if (client) {
                opaqueFullscreen = client->isFullScreen();
            }

            const SurfaceItem *surfaceItem = window->surfaceItem();
            if (surfaceItem) {
                data.clip |= surfaceItem->mapToGlobal(surfaceItem->shape());
            }
        } else if (toplevel->hasAlpha() && toplevel->opacity() == 1.0) {
            const SurfaceItem *surfaceItem = window->surfaceItem();
            if (surfaceItem) {
                const QRegion shape = surfaceItem->shape();
                const QRegion opaque = surfaceItem->opaque();
                data.clip = surfaceItem->mapToGlobal(shape & opaque);

                if (opaque == shape) {
                    data.mask = orig_mask | PAINT_WINDOW_OPAQUE;
                }
            }
        } else {
            data.clip = QRegion();
        }

        if (client && !client->decorationHasAlpha() && toplevel->opacity() == 1.0) {
            data.clip |= window->decorationShape().translated(window->pos());
        }

        data.quads = window->buildQuads();
        // preparation step
        effects->prePaintWindow(effectWindow(window), data, m_expectedPresentTimestamp);
#if !defined(QT_NO_DEBUG)
        if (data.quads.isTransformed()) {
            qFatal("Pre-paint calls are not allowed to transform quads!");
        }
#endif
        if (!window->isPaintingEnabled()) {
            continue;
        }
        dirtyArea |= data.paint;
        // Schedule the window for painting
        phase2data.append({ window, data.paint, data.clip, data.mask, data.quads });
    }

    // Save the part of the repaint region that's exclusively rendered to
    // bring a reused back buffer up to date. Then union the dirty region
    // with the repaint region.
    const QRegion repaintClip = repaint_region - dirtyArea;
    dirtyArea |= repaint_region;

    const QSize &screenSize = screens()->size();
    const QRegion displayRegion(0, 0, screenSize.width(), screenSize.height());
    bool fullRepaint(dirtyArea == displayRegion); // spare some expensive region operations
    if (!fullRepaint) {
        extendPaintRegion(dirtyArea, opaqueFullscreen);
        fullRepaint = (dirtyArea == displayRegion);
    }

    QRegion allclips, upperTranslucentDamage;
    upperTranslucentDamage = repaint_region;

    // This is the occlusion culling pass
    for (int i = phase2data.count() - 1; i >= 0; --i) {
        Phase2Data *data = &phase2data[i];

        if (fullRepaint) {
            data->region = displayRegion;
        } else {
            data->region |= upperTranslucentDamage;
        }

        // subtract the parts which will possibly been drawn as part of
        // a higher opaque window
        data->region -= allclips;

        // Here we rely on WindowPrePaintData::setTranslucent() to remove
        // the clip if needed.
        if (!data->clip.isEmpty() && !(data->mask & PAINT_WINDOW_TRANSLUCENT)) {
            // clip away the opaque regions for all windows below this one
            allclips |= data->clip;
            // extend the translucent damage for windows below this by remaining (translucent) regions
            if (!fullRepaint) {
                upperTranslucentDamage |= data->region - data->clip;
            }
        } else if (!fullRepaint) {
            upperTranslucentDamage |= data->region;
        }
    }

    QRegion paintedArea;
    // Fill any areas of the root window not covered by opaque windows
    if (m_paintScreenCount == 1) {
        aboutToStartPainting(painted_screen, dirtyArea);

        if (orig_mask & PAINT_SCREEN_BACKGROUND_FIRST) {
            paintBackground(infiniteRegion());
        }
    }
    if (!(orig_mask & PAINT_SCREEN_BACKGROUND_FIRST)) {
        paintedArea = dirtyArea - allclips;
        paintBackground(paintedArea);
    }

    // Now walk the list bottom to top and draw the windows.
    for (int i = 0; i < phase2data.count(); ++i) {
        Phase2Data *data = &phase2data[i];

        // add all regions which have been drawn so far
        paintedArea |= data->region;
        data->region = paintedArea;

        paintWindow(data->window, data->mask, data->region, data->quads);
    }

    if (fullRepaint) {
        painted_region = displayRegion;
        damaged_region = displayRegion - repaintClip;
    } else {
        painted_region |= paintedArea;

        // Clip the repainted region from the damaged region.
        // It's important that we don't add the union of the damaged region
        // and the repainted region to the damage history. Otherwise the
        // repaint region will grow with every frame until it eventually
        // covers the whole back buffer, at which point we're always doing
        // full repaints.
        damaged_region = paintedArea - repaintClip;
    }
}

void Scene::addToplevel(Toplevel *c)
{
    Q_ASSERT(!m_windows.contains(c));
    Scene::Window *w = createWindow(c);
    m_windows[ c ] = w;

    connect(c, &Toplevel::windowClosed, this, &Scene::windowClosed);

    c->effectWindow()->setSceneWindow(w);
    c->updateShadow();
    w->updateShadow(c->shadow());
}

void Scene::removeToplevel(Toplevel *toplevel)
{
    Q_ASSERT(m_windows.contains(toplevel));
    delete m_windows.take(toplevel);
    toplevel->effectWindow()->setSceneWindow(nullptr);
}

void Scene::windowClosed(Toplevel *toplevel, Deleted *deleted)
{
    if (!deleted) {
        removeToplevel(toplevel);
        return;
    }

    Q_ASSERT(m_windows.contains(toplevel));
    Window *window = m_windows.take(toplevel);
    window->updateToplevel(deleted);
    if (window->shadow()) {
        window->shadow()->setToplevel(deleted);
    }
    m_windows[deleted] = window;
}

void Scene::createStackingOrder(const QList<Toplevel *> &toplevels)
{
    // TODO: cache the stacking_order in case it has not changed
    foreach (Toplevel *c, toplevels) {
        Q_ASSERT(m_windows.contains(c));
        stacking_order.append(m_windows[ c ]);
    }
}

void Scene::clearStackingOrder()
{
    stacking_order.clear();
}

static Scene::Window *s_recursionCheck = nullptr;

void Scene::paintWindow(Window* w, int mask, const QRegion &_region, const WindowQuadList &quads)
{
    // no painting outside visible screen (and no transformations)
    const QRegion region = _region & QRect({0, 0}, screens()->size());
    if (region.isEmpty())  // completely clipped
        return;
    if (w->window()->isDeleted() && w->window()->skipsCloseAnimation()) {
        // should not get painted
        return;
    }

    if (s_recursionCheck == w) {
        return;
    }

    WindowPaintData data(w->window()->effectWindow(), screenProjectionMatrix());
    data.quads = quads;
    effects->paintWindow(effectWindow(w), mask, region, data);
    // paint thumbnails on top of window
    paintWindowThumbnails(w, region, data.opacity(), data.brightness(), data.saturation());
    // and desktop thumbnails
    paintDesktopThumbnails(w);
}

static void adjustClipRegion(AbstractThumbnailItem *item, QRegion &clippingRegion)
{
    if (item->clip() && item->clipTo()) {
        // the x/y positions of the parent item are not correct. The margins are added, though the size seems fine
        // that's why we have to get the offset by inspecting the anchors properties
        QQuickItem *parentItem = item->clipTo();
        QPointF offset;
        QVariant anchors = parentItem->property("anchors");
        if (anchors.isValid()) {
            if (QObject *anchorsObject = anchors.value<QObject*>()) {
                offset.setX(anchorsObject->property("leftMargin").toReal());
                offset.setY(anchorsObject->property("topMargin").toReal());
            }
        }
        QRectF rect = QRectF(parentItem->position() - offset, QSizeF(parentItem->width(), parentItem->height()));
        if (QQuickItem *p = parentItem->parentItem()) {
            rect = p->mapRectToScene(rect);
        }
        clippingRegion &= rect.adjusted(0,0,-1,-1).translated(item->window()->position()).toRect();
    }
}

void Scene::paintWindowThumbnails(Scene::Window *w, const QRegion &region, qreal opacity, qreal brightness, qreal saturation)
{
    EffectWindowImpl *wImpl = static_cast<EffectWindowImpl*>(effectWindow(w));
    for (QHash<WindowThumbnailItem*, QPointer<EffectWindowImpl> >::const_iterator it = wImpl->thumbnails().constBegin();
            it != wImpl->thumbnails().constEnd();
            ++it) {
        if (it.value().isNull()) {
            continue;
        }
        WindowThumbnailItem *item = it.key();
        if (!item->isVisible()) {
            continue;
        }
        EffectWindowImpl *thumb = it.value().data();
        WindowPaintData thumbData(thumb, screenProjectionMatrix());
        thumbData.setOpacity(opacity);
        thumbData.setBrightness(brightness * item->brightness());
        thumbData.setSaturation(saturation * item->saturation());

        const QRect visualThumbRect(thumb->expandedGeometry());

        QSizeF size = QSizeF(visualThumbRect.size());
        size.scale(QSizeF(item->width(), item->height()), Qt::KeepAspectRatio);
        if (size.width() > visualThumbRect.width() || size.height() > visualThumbRect.height()) {
            size = QSizeF(visualThumbRect.size());
        }
        thumbData.setXScale(size.width() / static_cast<qreal>(visualThumbRect.width()));
        thumbData.setYScale(size.height() / static_cast<qreal>(visualThumbRect.height()));

        if (!item->window()) {
            continue;
        }
        const QPointF point = item->mapToScene(QPointF(0,0));
        qreal x = point.x() + w->x() + (item->width() - size.width())/2;
        qreal y = point.y() + w->y() + (item->height() - size.height()) / 2;
        x -= thumb->x();
        y -= thumb->y();
        // compensate shadow topleft padding
        x += (thumb->x()-visualThumbRect.x())*thumbData.xScale();
        y += (thumb->y()-visualThumbRect.y())*thumbData.yScale();
        thumbData.setXTranslation(x);
        thumbData.setYTranslation(y);
        int thumbMask = PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_LANCZOS;
        if (thumbData.opacity() == 1.0) {
            thumbMask |= PAINT_WINDOW_OPAQUE;
        } else {
            thumbMask |= PAINT_WINDOW_TRANSLUCENT;
        }
        QRegion clippingRegion = region;
        clippingRegion &= QRegion(wImpl->x(), wImpl->y(), wImpl->width(), wImpl->height());
        adjustClipRegion(item, clippingRegion);
        effects->drawWindow(thumb, thumbMask, clippingRegion, thumbData);
    }
}

void Scene::paintDesktopThumbnails(Scene::Window *w)
{
    EffectWindowImpl *wImpl = static_cast<EffectWindowImpl*>(effectWindow(w));
    for (QList<DesktopThumbnailItem*>::const_iterator it = wImpl->desktopThumbnails().constBegin();
            it != wImpl->desktopThumbnails().constEnd();
            ++it) {
        DesktopThumbnailItem *item = *it;
        if (!item->isVisible()) {
            continue;
        }
        if (!item->window()) {
            continue;
        }
        s_recursionCheck = w;

        ScreenPaintData data;
        const QSize &screenSize = screens()->size();
        QSize size = screenSize;

        size.scale(item->width(), item->height(), Qt::KeepAspectRatio);
        data *= QVector2D(size.width() / double(screenSize.width()),
                          size.height() / double(screenSize.height()));
        const QPointF point = item->mapToScene(item->position());
        const qreal x = point.x() + w->x() + (item->width() - size.width())/2;
        const qreal y = point.y() + w->y() + (item->height() - size.height()) / 2;
        const QRect region = QRect(x, y, item->width(), item->height());
        QRegion clippingRegion = region;
        clippingRegion &= QRegion(wImpl->x(), wImpl->y(), wImpl->width(), wImpl->height());
        adjustClipRegion(item, clippingRegion);
        data += QPointF(x, y);
        const int desktopMask = PAINT_SCREEN_TRANSFORMED | PAINT_WINDOW_TRANSFORMED | PAINT_SCREEN_BACKGROUND_FIRST;
        paintDesktop(item->desktop(), desktopMask, clippingRegion, data);
        s_recursionCheck = nullptr;
    }
}

void Scene::paintDesktop(int desktop, int mask, const QRegion &region, ScreenPaintData &data)
{
    static_cast<EffectsHandlerImpl*>(effects)->paintDesktop(desktop, mask, region, data);
}

void Scene::aboutToStartPainting(int screenId, const QRegion &damage)
{
    Q_UNUSED(screenId)
    Q_UNUSED(damage)
}

// the function that'll be eventually called by paintWindow() above
void Scene::finalPaintWindow(EffectWindowImpl* w, int mask, const QRegion &region, WindowPaintData& data)
{
    effects->drawWindow(w, mask, region, data);
}

// will be eventually called from drawWindow()
void Scene::finalDrawWindow(EffectWindowImpl* w, int mask, const QRegion &region, WindowPaintData& data)
{
    if (waylandServer() && waylandServer()->isScreenLocked() && !w->window()->isLockScreen() && !w->window()->isInputMethod()) {
        return;
    }
    w->sceneWindow()->performPaint(mask, region, data);
}

void Scene::extendPaintRegion(QRegion &region, bool opaqueFullscreen)
{
    Q_UNUSED(region);
    Q_UNUSED(opaqueFullscreen);
}

void Scene::screenGeometryChanged(const QSize &size)
{
    if (!overlayWindow()) {
        return;
    }
    overlayWindow()->resize(size);
}

bool Scene::makeOpenGLContextCurrent()
{
    return false;
}

void Scene::doneOpenGLContextCurrent()
{
}

bool Scene::supportsSurfacelessContext() const
{
    return false;
}

bool Scene::supportsNativeFence() const
{
    return false;
}

void Scene::triggerFence()
{
}

QMatrix4x4 Scene::screenProjectionMatrix() const
{
    return QMatrix4x4();
}

xcb_render_picture_t Scene::xrenderBufferPicture() const
{
    return XCB_RENDER_PICTURE_NONE;
}

QPainter *Scene::scenePainter() const
{
    return nullptr;
}

QImage *Scene::qpainterRenderBuffer(int screenId) const
{
    Q_UNUSED(screenId)
    return nullptr;
}

QVector<QByteArray> Scene::openGLPlatformInterfaceExtensions() const
{
    return QVector<QByteArray>{};
}

//****************************************
// Scene::Window
//****************************************

Scene::Window::Window(Toplevel *client, QObject *parent)
    : QObject(parent)
    , toplevel(client)
    , filter(ImageFilterFast)
    , disable_painting(0)
    , cached_quad_list(nullptr)
{
    if (kwinApp()->platform()->isPerScreenRenderingEnabled()) {
        connect(kwinApp()->platform(), &Platform::outputEnabled, this, &Window::reallocRepaints);
        connect(kwinApp()->platform(), &Platform::outputDisabled, this, &Window::reallocRepaints);
    }
    reallocRepaints();

    if (qobject_cast<WaylandClient *>(client)) {
        m_windowItem.reset(new WindowItemWayland(this));
    } else if (qobject_cast<X11Client *>(client) || qobject_cast<Unmanaged *>(client)) {
        m_windowItem.reset(new WindowItemX11(this));
    } else if (qobject_cast<InternalClient *>(client)) {
        m_windowItem.reset(new WindowItemInternal(this));
    } else {
        Q_UNREACHABLE();
    }

    connect(toplevel, &Toplevel::frameGeometryChanged, this, &Window::updateWindowPosition);
    updateWindowPosition();
}

Scene::Window::~Window()
{
    for (int i = 0; i < m_repaints.count(); ++i) {
        const QRegion dirty = repaints(i);
        if (!dirty.isEmpty()) {
            Compositor::self()->addRepaint(dirty);
        }
    }
}

void Scene::Window::updateToplevel(Deleted *deleted)
{
    toplevel = deleted;
}

void Scene::Window::referencePreviousPixmap()
{
    if (surfaceItem()) {
        referencePreviousPixmap_helper(surfaceItem());
    }
}

void Scene::Window::referencePreviousPixmap_helper(SurfaceItem *item)
{
    item->referencePreviousPixmap();

    const QList<Item *> children = item->childItems();
    for (Item *child : children) {
        referencePreviousPixmap_helper(static_cast<SurfaceItem *>(child));
    }
}

void Scene::Window::unreferencePreviousPixmap()
{
    if (surfaceItem()) {
        unreferencePreviousPixmap_helper(surfaceItem());
    }
}

void Scene::Window::unreferencePreviousPixmap_helper(SurfaceItem *item)
{
    item->unreferencePreviousPixmap();

    const QList<Item *> children = item->childItems();
    for (Item *child : children) {
        unreferencePreviousPixmap_helper(static_cast<SurfaceItem *>(child));
    }
}

void Scene::Window::discardPixmap()
{
    if (surfaceItem()) {
        discardPixmap_helper(surfaceItem());
    }
}

void Scene::Window::discardPixmap_helper(SurfaceItem *item)
{
    item->discardPixmap();

    const QList<Item *> children = item->childItems();
    for (Item *child : children) {
        discardPixmap_helper(static_cast<SurfaceItem *>(child));
    }
}

void Scene::Window::updatePixmap()
{
    if (surfaceItem()) {
        updatePixmap_helper(surfaceItem());
    }
}

void Scene::Window::updatePixmap_helper(SurfaceItem *item)
{
    item->updatePixmap();

    const QList<Item *> children = item->childItems();
    for (Item *child : children) {
        updatePixmap_helper(static_cast<SurfaceItem *>(child));
    }
}

QRegion Scene::Window::decorationShape() const
{
    return QRegion(toplevel->rect()) - toplevel->transparentRect();
}

bool Scene::Window::isVisible() const
{
    if (toplevel->isDeleted())
        return false;
    if (!toplevel->isOnCurrentDesktop())
        return false;
    if (!toplevel->isOnCurrentActivity())
        return false;
    if (AbstractClient *c = dynamic_cast<AbstractClient*>(toplevel))
        return c->isShown(true);
    return true; // Unmanaged is always visible
}

bool Scene::Window::isOpaque() const
{
    return toplevel->opacity() == 1.0 && !toplevel->hasAlpha();
}

bool Scene::Window::isShaded() const
{
    if (AbstractClient *client = qobject_cast<AbstractClient *>(toplevel))
        return client->isShade();
    return false;
}

bool Scene::Window::isPaintingEnabled() const
{
    return !disable_painting;
}

void Scene::Window::resetPaintingEnabled()
{
    disable_painting = 0;
    if (toplevel->isDeleted())
        disable_painting |= PAINT_DISABLED_BY_DELETE;
    if (static_cast<EffectsHandlerImpl*>(effects)->isDesktopRendering()) {
        if (!toplevel->isOnDesktop(static_cast<EffectsHandlerImpl*>(effects)->currentRenderedDesktop())) {
            disable_painting |= PAINT_DISABLED_BY_DESKTOP;
        }
    } else {
        if (!toplevel->isOnCurrentDesktop())
            disable_painting |= PAINT_DISABLED_BY_DESKTOP;
    }
    if (!toplevel->isOnCurrentActivity())
        disable_painting |= PAINT_DISABLED_BY_ACTIVITY;
    if (AbstractClient *c = dynamic_cast<AbstractClient*>(toplevel)) {
        if (c->isMinimized())
            disable_painting |= PAINT_DISABLED_BY_MINIMIZE;
        if (c->isHiddenInternal()) {
            disable_painting |= PAINT_DISABLED;
        }
    }
}

void Scene::Window::enablePainting(int reason)
{
    disable_painting &= ~reason;
}

void Scene::Window::disablePainting(int reason)
{
    disable_painting |= reason;
}

WindowQuadList Scene::Window::buildQuads(bool force) const
{
    if (cached_quad_list != nullptr && !force)
        return *cached_quad_list;

    WindowQuadList *ret = new WindowQuadList;

    if (!isShaded()) {
        *ret += makeContentsQuads();
    }

    if (!toplevel->frameMargins().isNull()) {
        QRect rects[4];

        if (AbstractClient *client = qobject_cast<AbstractClient *>(toplevel)) {
            client->layoutDecorationRects(rects[0], rects[1], rects[2], rects[3]);
        } else if (Deleted *deleted = qobject_cast<Deleted *>(toplevel)) {
            deleted->layoutDecorationRects(rects[0], rects[1], rects[2], rects[3]);
        }

        *ret += makeDecorationQuads(rects, decorationShape());
    }
    if (shadowItem() && toplevel->wantsShadowToBeRendered()) {
        *ret << shadowItem()->shadow()->shadowQuads();
    }
    effects->buildQuads(toplevel->effectWindow(), *ret);
    cached_quad_list.reset(ret);
    return *ret;
}

WindowQuadList Scene::Window::makeDecorationQuads(const QRect *rects, const QRegion &region) const
{
    WindowQuadList list;

    const qreal textureScale = toplevel->screenScale();
    const int padding = 1;

    const QPoint topSpritePosition(padding, padding);
    const QPoint bottomSpritePosition(padding, topSpritePosition.y() + rects[1].height() + 2 * padding);
    const QPoint leftSpritePosition(bottomSpritePosition.y() + rects[3].height() + 2 * padding, padding);
    const QPoint rightSpritePosition(leftSpritePosition.x() + rects[0].width() + 2 * padding, padding);

    const QPoint offsets[4] = {
        QPoint(-rects[0].x(), -rects[0].y()) + leftSpritePosition,
        QPoint(-rects[1].x(), -rects[1].y()) + topSpritePosition,
        QPoint(-rects[2].x(), -rects[2].y()) + rightSpritePosition,
        QPoint(-rects[3].x(), -rects[3].y()) + bottomSpritePosition,
    };

    const Qt::Orientation orientations[4] = {
        Qt::Vertical,   // Left
        Qt::Horizontal, // Top
        Qt::Vertical,   // Right
        Qt::Horizontal, // Bottom
    };

    for (int i = 0; i < 4; i++) {
        const QRegion intersectedRegion = (region & rects[i]);
        for (const QRect &r : intersectedRegion) {
            if (!r.isValid())
                continue;

            const bool swap = orientations[i] == Qt::Vertical;

            const int x0 = r.x();
            const int y0 = r.y();
            const int x1 = r.x() + r.width();
            const int y1 = r.y() + r.height();

            const int u0 = (x0 + offsets[i].x()) * textureScale;
            const int v0 = (y0 + offsets[i].y()) * textureScale;
            const int u1 = (x1 + offsets[i].x()) * textureScale;
            const int v1 = (y1 + offsets[i].y()) * textureScale;

            WindowQuad quad(WindowQuadDecoration);
            quad.setUVAxisSwapped(swap);

            if (swap) {
                quad[0] = WindowVertex(x0, y0, v0, u0); // Top-left
                quad[1] = WindowVertex(x1, y0, v0, u1); // Top-right
                quad[2] = WindowVertex(x1, y1, v1, u1); // Bottom-right
                quad[3] = WindowVertex(x0, y1, v1, u0); // Bottom-left
            } else {
                quad[0] = WindowVertex(x0, y0, u0, v0); // Top-left
                quad[1] = WindowVertex(x1, y0, u1, v0); // Top-right
                quad[2] = WindowVertex(x1, y1, u1, v1); // Bottom-right
                quad[3] = WindowVertex(x0, y1, u0, v1); // Bottom-left
            }

            list.append(quad);
        }
    }

    return list;
}

WindowQuadList Scene::Window::makeContentsQuads() const
{
    // TODO(vlad): What about the case where we need to build window quads for a deleted
    // window? Presumably, the current window will be invalid so no window quads will be
    // generated. Is it okay?

    SurfaceItem *currentItem = surfaceItem();
    if (!currentItem)
        return WindowQuadList();

    WindowQuadList quads;
    int id = 0;

    // We need to assign an id to each generated window quad in order to be able to match
    // a list of window quads against a particular window pixmap. We traverse the window
    // pixmap tree in the depth-first search manner and assign an id to each window quad.
    // The id is the time when we visited the window pixmap.

    QStack<SurfaceItem *> stack;
    stack.push(currentItem);

    while (!stack.isEmpty()) {
        SurfaceItem *item = stack.pop();

        const QRegion region = item->shape();
        const int quadId = id++;

        for (const QRectF rect : region) {
            // Note that the window quad id is not unique if the window is shaped, i.e. the
            // region contains more than just one rectangle. We assume that the "source" quad
            // had been subdivided.
            WindowQuad quad(WindowQuadContents, quadId);

            const QPointF windowTopLeft = item->mapToWindow(rect.topLeft());
            const QPointF windowTopRight = item->mapToWindow(rect.topRight());
            const QPointF windowBottomRight = item->mapToWindow(rect.bottomRight());
            const QPointF windowBottomLeft = item->mapToWindow(rect.bottomLeft());

            const QPointF bufferTopLeft = item->mapToBuffer(rect.topLeft());
            const QPointF bufferTopRight = item->mapToBuffer(rect.topRight());
            const QPointF bufferBottomRight = item->mapToBuffer(rect.bottomRight());
            const QPointF bufferBottomLeft = item->mapToBuffer(rect.bottomLeft());

            quad[0] = WindowVertex(windowTopLeft, bufferTopLeft);
            quad[1] = WindowVertex(windowTopRight, bufferTopRight);
            quad[2] = WindowVertex(windowBottomRight, bufferBottomRight);
            quad[3] = WindowVertex(windowBottomLeft, bufferBottomLeft);

            quads << quad;
        }

        // Push the child window pixmaps onto the stack, remember we're visiting the pixmaps
        // in the depth-first search manner.
        const QList<Item *> children = item->childItems();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push(static_cast<SurfaceItem *>(*it));
        }
    }

    return quads;
}

void Scene::Window::discardQuads()
{
    cached_quad_list.reset();
}

const Shadow *Scene::Window::shadow() const
{
    if (shadowItem()) {
        return shadowItem()->shadow();
    }
    return nullptr;
}

Shadow *Scene::Window::shadow()
{
    if (shadowItem()) {
        return shadowItem()->shadow();
    }
    return nullptr;
}

void Scene::Window::updateShadow(Shadow* shadow)
{
    m_windowItem->setShadow(shadow);
}

void Scene::Window::preprocess(Item *item)
{
    item->preprocess();

    const QList<Item *> children = item->childItems();
    for (Item *child : children) {
        preprocess(child);
    }
}

void Scene::Window::addLayerRepaint(const QRegion &region)
{
    if (kwinApp()->platform()->isPerScreenRenderingEnabled()) {
        const QVector<AbstractOutput *> outputs = kwinApp()->platform()->enabledOutputs();
        if (m_repaints.count() != outputs.count()) {
            return; // Repaints haven't been reallocated yet, do nothing.
        }
        for (int screenId = 0; screenId < m_repaints.count(); ++screenId) {
            AbstractOutput *output = outputs[screenId];
            const QRegion dirtyRegion = region & output->geometry();
            if (!dirtyRegion.isEmpty()) {
                m_repaints[screenId] += dirtyRegion;
                output->renderLoop()->scheduleRepaint();
            }
        }
    } else {
        m_repaints[0] += region;
        kwinApp()->platform()->renderLoop()->scheduleRepaint();
    }
}

QRegion Scene::Window::repaints(int screen) const
{
    Q_ASSERT(!m_repaints.isEmpty());
    const int index = screen != -1 ? screen : 0;
    if (m_repaints[index] == infiniteRegion()) {
        return QRect(QPoint(0, 0), screens()->size());
    }
    return m_repaints[index];
}

void Scene::Window::resetRepaints(int screen)
{
    Q_ASSERT(!m_repaints.isEmpty());
    const int index = screen != -1 ? screen : 0;
    m_repaints[index] = QRegion();
}

void Scene::Window::reallocRepaints()
{
    if (kwinApp()->platform()->isPerScreenRenderingEnabled()) {
        m_repaints.resize(kwinApp()->platform()->enabledOutputs().count());
    } else {
        m_repaints.resize(1);
    }

    m_repaints.fill(infiniteRegion());
}

WindowItem *Scene::Window::windowItem() const
{
    return m_windowItem.data();
}

SurfaceItem *Scene::Window::surfaceItem() const
{
    return m_windowItem->surfaceItem();
}

ShadowItem *Scene::Window::shadowItem() const
{
    return m_windowItem->shadowItem();
}

void Scene::Window::scheduleRepaint()
{
    if (kwinApp()->platform()->isPerScreenRenderingEnabled()) {
        const QVector<AbstractOutput *> outputs = kwinApp()->platform()->enabledOutputs();
        for (AbstractOutput *output : outputs) {
            if (window()->isOnOutput(output)) {
                output->renderLoop()->scheduleRepaint();
            }
        }
    } else {
        kwinApp()->platform()->renderLoop()->scheduleRepaint();
    }
}

void Scene::Window::updateWindowPosition()
{
    m_windowItem->setPosition(pos());
}

//****************************************
// WindowPixmap
//****************************************
WindowPixmap::WindowPixmap(Scene::Window *window)
    : m_window(window)
    , m_pixmap(XCB_PIXMAP_NONE)
    , m_discarded(false)
{
}

WindowPixmap::~WindowPixmap()
{
    if (m_pixmap != XCB_WINDOW_NONE) {
        xcb_free_pixmap(connection(), m_pixmap);
    }
    clear();
}

void WindowPixmap::create()
{
    if (isValid() || toplevel()->isDeleted()) {
        return;
    }
    // always update from Buffer on Wayland, don't try using XPixmap
    if (kwinApp()->shouldUseWaylandForCompositing()) {
        // use Buffer
        update();
        return;
    }
    XServerGrabber grabber;
    xcb_pixmap_t pix = xcb_generate_id(connection());
    xcb_void_cookie_t namePixmapCookie = xcb_composite_name_window_pixmap_checked(connection(), toplevel()->frameId(), pix);
    Xcb::WindowAttributes windowAttributes(toplevel()->frameId());
    Xcb::WindowGeometry windowGeometry(toplevel()->frameId());
    if (xcb_generic_error_t *error = xcb_request_check(connection(), namePixmapCookie)) {
        qCDebug(KWIN_CORE, "Failed to create window pixmap for window 0x%x (error code %d)",
                toplevel()->window(), error->error_code);
        free(error);
        return;
    }
    // check that the received pixmap is valid and actually matches what we
    // know about the window (i.e. size)
    if (!windowAttributes || windowAttributes->map_state != XCB_MAP_STATE_VIEWABLE) {
        qCDebug(KWIN_CORE, "Failed to create window pixmap for window 0x%x (not viewable)",
                toplevel()->window());
        xcb_free_pixmap(connection(), pix);
        return;
    }
    const QRect bufferGeometry = toplevel()->bufferGeometry();
    if (windowGeometry.size() != bufferGeometry.size()) {
        qCDebug(KWIN_CORE, "Failed to create window pixmap for window 0x%x (mismatched geometry)",
                toplevel()->window());
        xcb_free_pixmap(connection(), pix);
        return;
    }
    m_pixmap = pix;
    m_pixmapSize = bufferGeometry.size();
    m_contentsRect = QRect(toplevel()->clientPos(), toplevel()->clientSize());
}

void WindowPixmap::clear()
{
    setBuffer(nullptr);
}

void WindowPixmap::setBuffer(KWaylandServer::BufferInterface *buffer)
{
    if (buffer == m_buffer) {
        return;
    }
    if (m_buffer) {
        disconnect(m_buffer, &KWaylandServer::BufferInterface::aboutToBeDestroyed, this, &WindowPixmap::clear);
        m_buffer->unref();
    }
    m_buffer = buffer;
    if (m_buffer) {
        m_buffer->ref();
        connect(m_buffer, &KWaylandServer::BufferInterface::aboutToBeDestroyed, this, &WindowPixmap::clear);
    }
}

void WindowPixmap::update()
{
    if (KWaylandServer::SurfaceInterface *s = surface()) {
        setBuffer(s->buffer());
    } else if (toplevel()->internalFramebufferObject()) {
        m_fbo = toplevel()->internalFramebufferObject();
    } else if (!toplevel()->internalImageObject().isNull()) {
        m_internalImage = toplevel()->internalImageObject();
    } else {
        clear();
    }
}

bool WindowPixmap::isValid() const
{
    if (m_buffer || !m_fbo.isNull() || !m_internalImage.isNull()) {
        return true;
    }
    return m_pixmap != XCB_PIXMAP_NONE;
}

KWaylandServer::SurfaceInterface *WindowPixmap::surface() const
{
    return m_surface;
}

void WindowPixmap::setSurface(KWaylandServer::SurfaceInterface *surface)
{
    m_surface = surface;
}

bool WindowPixmap::hasAlphaChannel() const
{
    if (buffer())
        return buffer()->hasAlphaChannel();
    return toplevel()->hasAlpha();
}

//****************************************
// Scene::EffectFrame
//****************************************
Scene::EffectFrame::EffectFrame(EffectFrameImpl* frame)
    : m_effectFrame(frame)
{
}

Scene::EffectFrame::~EffectFrame()
{
}

SceneFactory::SceneFactory(QObject *parent)
    : QObject(parent)
{
}

SceneFactory::~SceneFactory()
{
}

} // namespace

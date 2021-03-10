/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "toplevel.h"

#include "abstract_client.h"
#include "abstract_output.h"
#ifdef KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#include "atoms.h"
#include "client_machine.h"
#include "composite.h"
#include "effects.h"
#include "screens.h"
#include "shadow.h"
#include "windowitem.h"
#include "workspace.h"

#include <KWaylandServer/surface_interface.h>

#include <QDebug>

namespace KWin
{

Toplevel::Toplevel()
    : m_visual(XCB_NONE)
    , bit_depth(24)
    , info(nullptr)
    , ready_for_painting(false)
    , m_internalId(QUuid::createUuid())
    , m_client()
    , is_shape(false)
    , effect_window(nullptr)
    , m_clientMachine(new ClientMachine(this))
    , m_wmClientLeader(XCB_WINDOW_NONE)
    , m_screen(0)
    , m_skipCloseAnimation(false)
{
    connect(screens(), &Screens::changed, this, &Toplevel::checkScreen);
    connect(screens(), &Screens::countChanged, this, &Toplevel::checkScreen);
    setupCheckScreenConnection();
    connect(this, &Toplevel::bufferGeometryChanged, this, &Toplevel::inputTransformationChanged);

    // Only for compatibility reasons, drop in the next major release.
    connect(this, &Toplevel::frameGeometryChanged, this, &Toplevel::geometryChanged);
    connect(this, &Toplevel::geometryShapeChanged, this, &Toplevel::discardShapeRegion);
}

Toplevel::~Toplevel()
{
    delete info;
}

QDebug operator<<(QDebug debug, const Toplevel *toplevel)
{
    QDebugStateSaver saver(debug);
    debug.nospace();
    if (toplevel) {
        debug << toplevel->metaObject()->className() << '(' << static_cast<const void *>(toplevel);
        if (toplevel->window()) {
            debug << ", windowId=0x" << Qt::hex << toplevel->window() << Qt::dec;
        }
        if (const KWaylandServer::SurfaceInterface *surface = toplevel->surface()) {
            debug << ", surface=" << surface;
        }
        const AbstractClient *client = qobject_cast<const AbstractClient *>(toplevel);
        if (client) {
            if (!client->isPopupWindow()) {
                debug << ", caption=" << client->caption();
            }
            if (client->transientFor()) {
                debug << ", transientFor=" << client->transientFor();
            }
        }
        if (debug.verbosity() > 2) {
            debug << ", frameGeometry=" << toplevel->frameGeometry();
            debug << ", resourceName=" << toplevel->resourceName();
            debug << ", resourceClass=" << toplevel->resourceClass();
        }
        debug << ')';
    } else {
        debug << "Toplevel(0x0)";
    }
    return debug;
}

void Toplevel::detectShape(xcb_window_t id)
{
    const bool wasShape = is_shape;
    is_shape = Xcb::Extensions::self()->hasShape(id);
    if (wasShape != is_shape) {
        emit shapedChanged();
    }
}

// used only by Deleted::copy()
void Toplevel::copyToDeleted(Toplevel* c)
{
    m_internalId = c->internalId();
    m_frameGeometry = c->m_frameGeometry;
    m_clientGeometry = c->m_clientGeometry;
    m_visual = c->m_visual;
    bit_depth = c->bit_depth;
    info = c->info;
    m_client.reset(c->m_client, false);
    ready_for_painting = c->ready_for_painting;
    is_shape = c->is_shape;
    effect_window = c->effect_window;
    if (effect_window != nullptr)
        effect_window->setWindow(this);
    resource_name = c->resourceName();
    resource_class = c->resourceClass();
    m_clientMachine = c->m_clientMachine;
    m_clientMachine->setParent(this);
    m_wmClientLeader = c->wmClientLeader();
    opaque_region = c->opaqueRegion();
    m_screen = c->m_screen;
    m_skipCloseAnimation = c->m_skipCloseAnimation;
    m_internalFBO = c->m_internalFBO;
    m_internalImage = c->m_internalImage;
    m_opacity = c->m_opacity;
}

// before being deleted, remove references to everything that's now
// owner by Deleted
void Toplevel::disownDataPassedToDeleted()
{
    info = nullptr;
}

QRect Toplevel::visibleGeometry() const
{
    if (const WindowItem *item = windowItem()) {
        return item->mapToGlobal(item->boundingRect());
    }
    return QRect();
}

Xcb::Property Toplevel::fetchWmClientLeader() const
{
    return Xcb::Property(false, window(), atoms->wm_client_leader, XCB_ATOM_WINDOW, 0, 10000);
}

void Toplevel::readWmClientLeader(Xcb::Property &prop)
{
    m_wmClientLeader = prop.value<xcb_window_t>(window());
}

void Toplevel::getWmClientLeader()
{
    auto prop = fetchWmClientLeader();
    readWmClientLeader(prop);
}

/**
 * Returns sessionId for this client,
 * taken either from its window or from the leader window.
 */
QByteArray Toplevel::sessionId() const
{
    QByteArray result = Xcb::StringProperty(window(), atoms->sm_client_id);
    if (result.isEmpty() && m_wmClientLeader && m_wmClientLeader != window()) {
        result = Xcb::StringProperty(m_wmClientLeader, atoms->sm_client_id);
    }
    return result;
}

/**
 * Returns command property for this client,
 * taken either from its window or from the leader window.
 */
QByteArray Toplevel::wmCommand()
{
    QByteArray result = Xcb::StringProperty(window(), XCB_ATOM_WM_COMMAND);
    if (result.isEmpty() && m_wmClientLeader && m_wmClientLeader != window()) {
        result = Xcb::StringProperty(m_wmClientLeader, XCB_ATOM_WM_COMMAND);
    }
    result.replace(0, ' ');
    return result;
}

void Toplevel::getWmClientMachine()
{
    m_clientMachine->resolve(window(), wmClientLeader());
}

/**
 * Returns client machine for this client,
 * taken either from its window or from the leader window.
 */
QByteArray Toplevel::wmClientMachine(bool use_localhost) const
{
    if (!m_clientMachine) {
        // this should never happen
        return QByteArray();
    }
    if (use_localhost && m_clientMachine->isLocal()) {
        // special name for the local machine (localhost)
        return ClientMachine::localhost();
    }
    return m_clientMachine->hostName();
}

/**
 * Returns client leader window for this client.
 * Returns the client window itself if no leader window is defined.
 */
xcb_window_t Toplevel::wmClientLeader() const
{
    if (m_wmClientLeader != XCB_WINDOW_NONE) {
        return m_wmClientLeader;
    }
    return window();
}

void Toplevel::getResourceClass()
{
    if (!info) {
        return;
    }
    setResourceClass(QByteArray(info->windowClassName()).toLower(), QByteArray(info->windowClassClass()).toLower());
}

void Toplevel::setResourceClass(const QByteArray &name, const QByteArray &className)
{
    resource_name  = name;
    resource_class = className;
    emit windowClassChanged();
}

bool Toplevel::resourceMatch(const Toplevel *c1, const Toplevel *c2)
{
    return c1->resourceClass() == c2->resourceClass();
}

qreal Toplevel::opacity() const
{
    return m_opacity;
}

void Toplevel::setOpacity(qreal opacity)
{
    opacity = qBound(0.0, opacity, 1.0);
    if (m_opacity == opacity) {
        return;
    }
    const qreal oldOpacity = m_opacity;
    m_opacity = opacity;
    if (compositing()) {
        addRepaintFull();
        emit opacityChanged(this, oldOpacity);
    }
}

bool Toplevel::setupCompositing()
{
    if (!compositing())
        return false;

    effect_window = new EffectWindowImpl(this);
    Compositor::self()->scene()->addToplevel(this);

    connect(windowItem(), &WindowItem::xChanged, this, &Toplevel::visibleGeometryChanged);
    connect(windowItem(), &WindowItem::yChanged, this, &Toplevel::visibleGeometryChanged);
    connect(windowItem(), &WindowItem::boundingRectChanged, this, &Toplevel::visibleGeometryChanged);

    return true;
}

void Toplevel::finishCompositing(ReleaseReason)
{
    if (effect_window && effect_window->window() == this) { // otherwise it's already passed to Deleted, don't free data
        deleteEffectWindow();
    }
}

void Toplevel::discardWindowPixmap()
{
    if (effectWindow() != nullptr && effectWindow()->sceneWindow() != nullptr)
        effectWindow()->sceneWindow()->discardPixmap();
}

bool Toplevel::compositing() const
{
    if (!Workspace::self()) {
        return false;
    }
    return Workspace::self()->compositing();
}

void Toplevel::addRepaint(const QRect &rect)
{
    addRepaint(QRegion(rect));
}

void Toplevel::addRepaint(int x, int y, int width, int height)
{
    addRepaint(QRegion(x, y, width, height));
}

void Toplevel::addRepaint(const QRegion &region)
{
    if (!effectWindow() || !effectWindow()->sceneWindow()) {
        return;
    }
    effectWindow()->sceneWindow()->addLayerRepaint(region.translated(pos()));
}

void Toplevel::addLayerRepaint(const QRect &rect)
{
    addLayerRepaint(QRegion(rect));
}

void Toplevel::addLayerRepaint(int x, int y, int width, int height)
{
    addLayerRepaint(QRegion(x, y, width, height));
}

void Toplevel::addLayerRepaint(const QRegion &region)
{
    if (!effectWindow() || !effectWindow()->sceneWindow()) {
        return;
    }
    effectWindow()->sceneWindow()->addLayerRepaint(region);
}

void Toplevel::addRepaintFull()
{
    addLayerRepaint(visibleGeometry());
}

void Toplevel::addWorkspaceRepaint(int x, int y, int w, int h)
{
    addWorkspaceRepaint(QRect(x, y, w, h));
}

void Toplevel::addWorkspaceRepaint(const QRect& r2)
{
    if (!compositing())
        return;
    Compositor::self()->addRepaint(r2);
}

void Toplevel::addWorkspaceRepaint(const QRegion &region)
{
    if (compositing()) {
        Compositor::self()->addRepaint(region);
    }
}

void Toplevel::setReadyForPainting()
{
    if (!ready_for_painting) {
        ready_for_painting = true;
        if (compositing()) {
            addRepaintFull();
            emit windowShown(this);
        }
    }
}

void Toplevel::deleteEffectWindow()
{
    delete effect_window;
    effect_window = nullptr;
}

void Toplevel::checkScreen()
{
    if (screens()->count() == 1) {
        if (m_screen != 0) {
            m_screen = 0;
            emit screenChanged();
        }
    } else {
        const int s = screens()->number(frameGeometry().center());
        if (s != m_screen) {
            m_screen = s;
            emit screenChanged();
        }
    }
    qreal newScale = screens()->scale(m_screen);
    if (newScale != m_screenScale) {
        m_screenScale = newScale;
        emit screenScaleChanged();
    }
}

void Toplevel::setupCheckScreenConnection()
{
    connect(this, &Toplevel::frameGeometryChanged, this, &Toplevel::checkScreen);
    checkScreen();
}

void Toplevel::removeCheckScreenConnection()
{
    disconnect(this, &Toplevel::frameGeometryChanged, this, &Toplevel::checkScreen);
}

int Toplevel::screen() const
{
    return m_screen;
}

qreal Toplevel::screenScale() const
{
    return m_screenScale;
}

qreal Toplevel::bufferScale() const
{
    return surface() ? surface()->bufferScale() : 1;
}

bool Toplevel::isOnScreen(int screen) const
{
    return screens()->geometry(screen).intersects(frameGeometry());
}

bool Toplevel::isOnActiveScreen() const
{
    return isOnScreen(screens()->current());
}

bool Toplevel::isOnOutput(AbstractOutput *output) const
{
    return output->geometry().intersects(frameGeometry());
}

void Toplevel::updateShadow()
{
    const QRect oldVisibleRect = visibleGeometry();
    if (shadow()) {
        if (!effectWindow()->sceneWindow()->shadow()->updateShadow()) {
            effectWindow()->sceneWindow()->updateShadow(nullptr);
        }
        emit shadowChanged();
    } else {
        Shadow::createShadow(this);
    }
    if (oldVisibleRect != visibleGeometry())
        emit paddingChanged(this, oldVisibleRect);
}

Shadow *Toplevel::shadow()
{
    if (effectWindow() && effectWindow()->sceneWindow()) {
        return effectWindow()->sceneWindow()->shadow();
    } else {
        return nullptr;
    }
}

const Shadow *Toplevel::shadow() const
{
    if (effectWindow() && effectWindow()->sceneWindow()) {
        return effectWindow()->sceneWindow()->shadow();
    } else {
        return nullptr;
    }
}

SurfaceItem *Toplevel::surfaceItem() const
{
    if (effectWindow() && effectWindow()->sceneWindow()) {
        return effectWindow()->sceneWindow()->surfaceItem();
    }
    return nullptr;
}

WindowItem *Toplevel::windowItem() const
{
    if (effectWindow() && effectWindow()->sceneWindow()) {
        return effectWindow()->sceneWindow()->windowItem();
    }
    return nullptr;
}

bool Toplevel::wantsShadowToBeRendered() const
{
    return true;
}

void Toplevel::getWmOpaqueRegion()
{
    if (!info) {
        return;
    }

    const auto rects = info->opaqueRegion();
    QRegion new_opaque_region;
    for (const auto &r : rects) {
        new_opaque_region += QRect(r.pos.x, r.pos.y, r.size.width, r.size.height);
    }

    opaque_region = new_opaque_region;
}

QRegion Toplevel::shapeRegion() const
{
    if (m_shapeRegionIsValid) {
        return m_shapeRegion;
    }

    const QRect bufferGeometry = this->bufferGeometry();

    if (shape()) {
        auto cookie = xcb_shape_get_rectangles_unchecked(connection(), frameId(), XCB_SHAPE_SK_BOUNDING);
        ScopedCPointer<xcb_shape_get_rectangles_reply_t> reply(xcb_shape_get_rectangles_reply(connection(), cookie, nullptr));
        if (!reply.isNull()) {
            m_shapeRegion = QRegion();
            const xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(reply.data());
            const int rectCount = xcb_shape_get_rectangles_rectangles_length(reply.data());
            for (int i = 0; i < rectCount; ++i) {
                m_shapeRegion += QRegion(rects[i].x, rects[i].y, rects[i].width, rects[i].height);
            }
            // make sure the shape is sane (X is async, maybe even XShape is broken)
            m_shapeRegion &= QRegion(0, 0, bufferGeometry.width(), bufferGeometry.height());
        } else {
            m_shapeRegion = QRegion();
        }
    } else {
        m_shapeRegion = QRegion(0, 0, bufferGeometry.width(), bufferGeometry.height());
    }

    m_shapeRegionIsValid = true;
    return m_shapeRegion;
}

void Toplevel::discardShapeRegion()
{
    m_shapeRegionIsValid = false;
    m_shapeRegion = QRegion();
}

bool Toplevel::isClient() const
{
    return false;
}

bool Toplevel::isDeleted() const
{
    return false;
}

bool Toplevel::isOnCurrentActivity() const
{
#ifdef KWIN_BUILD_ACTIVITIES
    if (!Activities::self()) {
        return true;
    }
    return isOnActivity(Activities::self()->current());
#else
    return true;
#endif
}

void Toplevel::elevate(bool elevate)
{
    if (!effectWindow()) {
        return;
    }
    effectWindow()->elevate(elevate);
    addWorkspaceRepaint(visibleGeometry());
}

pid_t Toplevel::pid() const
{
    if (!info) {
        return -1;
    }
    return info->pid();
}

xcb_window_t Toplevel::frameId() const
{
    return m_client;
}

Xcb::Property Toplevel::fetchSkipCloseAnimation() const
{
    return Xcb::Property(false, window(), atoms->kde_skip_close_animation, XCB_ATOM_CARDINAL, 0, 1);
}

void Toplevel::readSkipCloseAnimation(Xcb::Property &property)
{
    setSkipCloseAnimation(property.toBool());
}

void Toplevel::getSkipCloseAnimation()
{
    Xcb::Property property = fetchSkipCloseAnimation();
    readSkipCloseAnimation(property);
}

bool Toplevel::skipsCloseAnimation() const
{
    return m_skipCloseAnimation;
}

void Toplevel::setSkipCloseAnimation(bool set)
{
    if (set == m_skipCloseAnimation) {
        return;
    }
    m_skipCloseAnimation = set;
    emit skipCloseAnimationChanged();
}

void Toplevel::setSurface(KWaylandServer::SurfaceInterface *surface)
{
    if (m_surface == surface) {
        return;
    }
    m_surface = surface;
    connect(m_surface, &KWaylandServer::SurfaceInterface::destroyed, this, [this]() {
        m_surface = nullptr;
        m_surfaceId = 0;
    });
    m_surfaceId = surface->id();
    emit surfaceChanged();
}

QByteArray Toplevel::windowRole() const
{
    if (!info) {
        return {};
    }
    return QByteArray(info->windowRole());
}

void Toplevel::setDepth(int depth)
{
    if (bit_depth == depth) {
        return;
    }
    const bool oldAlpha = hasAlpha();
    bit_depth = depth;
    if (oldAlpha != hasAlpha()) {
        emit hasAlphaChanged();
    }
}

QRegion Toplevel::inputShape() const
{
    if (m_surface) {
        return m_surface->input();
    } else {
        // TODO: maybe also for X11?
        return QRegion();
    }
}

QMatrix4x4 Toplevel::inputTransformation() const
{
    QMatrix4x4 m;
    m.translate(-x(), -y());
    return m;
}

bool Toplevel::hitTest(const QPoint &point) const
{
    if (m_surface && m_surface->isMapped()) {
        return m_surface->inputSurfaceAt(mapToLocal(point));
    }
    return inputGeometry().contains(point);
}

QPoint Toplevel::mapToFrame(const QPoint &point) const
{
    return point - frameGeometry().topLeft();
}

QPoint Toplevel::mapToLocal(const QPoint &point) const
{
    return point - bufferGeometry().topLeft();
}

QPointF Toplevel::mapToLocal(const QPointF &point) const
{
    return point - bufferGeometry().topLeft();
}

QRect Toplevel::inputGeometry() const
{
    return frameGeometry();
}

bool Toplevel::isLocalhost() const
{
    if (!m_clientMachine) {
        return true;
    }
    return m_clientMachine->isLocal();
}

QMargins Toplevel::frameMargins() const
{
    return QMargins();
}

} // namespace


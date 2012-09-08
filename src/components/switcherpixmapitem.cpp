
// This file is part of lipstick, a QML desktop library
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License version 2.1 as published by the Free Software Foundation
// and appearing in the file LICENSE.LGPL included in the packaging
// of this file.
//
// This code is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
//
// Copyright (c) 2011, Robin Burchell
// Copyright (c) 2012, Timur Kristóf <venemo@fedoraproject.org>
// Copyright (c) 2010, Nokia Corporation and/or its subsidiary(-ies) <directui@nokia.com>

#include <QApplication>
#include <QPainter>
#include <QTimer>
#include <QX11Info>

#include "switcherpixmapitem.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

// TODO: disable damage event processing when not on the screen

#ifdef Q_WS_X11
unsigned char xErrorCode = Success;

static int handleXError(Display *, XErrorEvent *event)
{
    xErrorCode = event->error_code;

    return 0;
}
#endif

struct SwitcherPixmapItem::Private
{
    Private()
        : xWindowPixmapIsValid(false)
        , xWindowPixmap(0)
        , xWindowPixmapDamage(0)
        , windowId(0)
    {}

    bool xWindowPixmapIsValid;
    Pixmap xWindowPixmap;
    Damage xWindowPixmapDamage;
    QPixmap qWindowPixmap;
    WId windowId;
};

SwitcherPixmapItem::SwitcherPixmapItem(QDeclarativeItem *parent)
    : QDeclarativeItem(parent)
    , d(new Private)
{
    setFlag(QGraphicsItem::ItemHasNoContents, false);
    connect(qApp, SIGNAL(damageEvent(Qt::HANDLE &, short &, short &, unsigned short &, unsigned short &)), this, SLOT(damageEvent(Qt::HANDLE &, short &, short &, unsigned short &, unsigned short &)));
}

SwitcherPixmapItem::~SwitcherPixmapItem()
{
    destroyDamage();
    if (d->xWindowPixmap)
        XFreePixmap(QX11Info::display(), d->xWindowPixmap);
    delete d;
}

void SwitcherPixmapItem::damageEvent(Qt::HANDLE &damage, short &x, short &y, unsigned short &width, unsigned short &height)
{
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(width);
    Q_UNUSED(height);
#ifdef Q_WS_X11
    if (d->xWindowPixmapDamage == damage) {
        XDamageSubtract(QX11Info::display(), d->xWindowPixmapDamage, None, None);
        update();
    }
#else
    Q_UNUSED(damage);
#endif
}

void SwitcherPixmapItem::destroyDamage()
{
    if (d->xWindowPixmapDamage != 0) {
        XDamageDestroy(QX11Info::display(), d->xWindowPixmapDamage);
        d->xWindowPixmapDamage = 0;
    }
}

void SwitcherPixmapItem::createDamage()
{
    // TODO: check on display status, don't create damage if off
    if (d->windowId == 0)
        return;

    // Register the pixmap for XDamage events
    d->xWindowPixmapDamage = XDamageCreate(QX11Info::display(), d->windowId, XDamageReportNonEmpty);
}

void SwitcherPixmapItem::updateXWindowPixmap()
{
#ifdef Q_WS_X11
    qDebug() << Q_FUNC_INFO << "Resetting X pixmap for " << d->windowId;

    // It is possible that the window is not redirected so check for errors.
    // XSync() needs to be called so that previous errors go to the original
    // handler.
    XSync(QX11Info::display(), FALSE);
    XErrorHandler errh = XSetErrorHandler(handleXError);
    xErrorCode = Success;

    // Get the pixmap ID of the X window
    Pixmap newWindowPixmap = XCompositeNameWindowPixmap(QX11Info::display(), d->windowId);

    // XCompositeNameWindowPixmap doesn't wait for the server to reply, we'll
    // need to do it ourselves to catch the possible BadMatch
    XSync(QX11Info::display(), FALSE);

    d->xWindowPixmapIsValid = xErrorCode == Success;
    if (d->xWindowPixmapIsValid) {
        // Unregister the old pixmap from XDamage events
        destroyDamage();

        if (d->xWindowPixmap != 0) {
            // Dereference the old pixmap ID
            XFreePixmap(QX11Info::display(), d->xWindowPixmap);
        }

        d->xWindowPixmap = newWindowPixmap;

        // Register the pixmap for XDamage events
        createDamage();

        d->qWindowPixmap = QPixmap::fromX11Pixmap(d->xWindowPixmap, QPixmap::ExplicitlyShared);
    } else {
        // If a BadMatch error occurred the window wasn't redirected yet; deference the invalid pixmap
        if (newWindowPixmap != 0) {
            XFreePixmap(QX11Info::display(), newWindowPixmap);
        }
    }

    // Reset the error handler
    XSetErrorHandler(errh);
#else
#error "not implemented"
#endif
}

void SwitcherPixmapItem::setWindowId(int window)
{
    d->windowId = window;
    d->xWindowPixmapIsValid = false;

    // TODO: should we XFreePixmap here?

    update();
}

int SwitcherPixmapItem::windowId() const
{
    return d->windowId;
}

void SwitcherPixmapItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
                               QWidget *widget)
{
    Q_UNUSED(option);
    Q_UNUSED(widget);

    if (!d->xWindowPixmapIsValid) {
        updateXWindowPixmap();
    }

    QT_TRY {
        painter->drawPixmap(QRect(0, 0, width(), height()), d->qWindowPixmap);
    } QT_CATCH (std::bad_alloc e) {
        // XGetImage failed, the window has been already unmapped
    }
}

bool SwitcherPixmapItem::handleXEvent(const XEvent &event)
{
    WId windowId;

    if (event.type == VisibilityNotify) {
        if (event.xvisibility.state != VisibilityFullyObscured ||
            event.xvisibility.send_event != True) {
            qDebug() << Q_FUNC_INFO << "Ignoring VisibilityNotify that isn't a send_event VisibilityFullyObscured for " << event.xvisibility.window;
            return false;
        }

        windowId = event.xvisibility.window;
        qDebug() << Q_FUNC_INFO << "Got obscured for " << windowId << "; want " << d->windowId;
    } else if (event.type == ConfigureNotify) {
        windowId = event.xconfigure.window;
        qDebug() << Q_FUNC_INFO << "ConfigureNotify for " << windowId << "; want " << d->windowId;
    } else {
        return false;
    }

    if (windowId != d->windowId)
        return false;

    qDebug() << Q_FUNC_INFO << "Invalidated, resetting pixmap for " << d->windowId;
    d->xWindowPixmapIsValid = false;
    update();
    return true;
}


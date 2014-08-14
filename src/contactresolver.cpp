/******************************************************************************
**
** This file is part of libcommhistory.
**
** Copyright (C) 2014 Jolla Ltd.
** Contact: John Brooks <john.brooks@jollamobile.com>
**
** This library is free software; you can redistribute it and/or modify it
** under the terms of the GNU Lesser General Public License version 2.1 as
** published by the Free Software Foundation.
**
** This library is distributed in the hope that it will be useful, but
** WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
** or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
** License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this library; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
**
******************************************************************************/

#include "contactresolver.h"
#include "debug.h"
#include <seasidecache.h>

using namespace CommHistory;

namespace CommHistory {

class ContactResolverPrivate : public QObject, public SeasideCache::ResolveListener
{
    Q_OBJECT
    Q_DECLARE_PUBLIC(ContactResolver)

public:
    ContactResolver *q_ptr;
    QSet<Recipient> pending;
    bool resolving;
    bool forceResolving;

    explicit ContactResolverPrivate(ContactResolver *parent);

    bool resolve(Recipient recipient);
    virtual void addressResolved(const QString &first, const QString &second, SeasideCache::CacheItem *item);

public slots:
    bool checkIfFinished();
};

}

ContactResolver::ContactResolver(QObject *parent)
    : QObject(parent), d_ptr(new ContactResolverPrivate(this))
{
}

ContactResolverPrivate::ContactResolverPrivate(ContactResolver *parent)
    : QObject(parent), q_ptr(parent), resolving(false), forceResolving(false)
{
}

bool ContactResolver::isResolving() const
{
    Q_D(const ContactResolver);
    return d->resolving;
}

bool ContactResolver::forceResolving() const
{
    Q_D(const ContactResolver);
    return d->forceResolving;
}

void ContactResolver::setForceResolving(bool enabled)
{
    Q_D(ContactResolver);
    d->forceResolving = enabled;
}

void ContactResolver::add(const Recipient &recipient)
{
    Q_D(ContactResolver);
    if (!d->resolving) {
        // On the first resolve, make a queued call to checkIfFinished.
        // This handles asynchronously emitting the signal if nothing has
        // to be resolved, to preserve API assumptions.
        bool ok = d->metaObject()->invokeMethod(d, "checkIfFinished", Qt::QueuedConnection);
        Q_UNUSED(ok);
        Q_ASSERT(ok);
    }

    d->resolving = true;
    d->resolve(recipient);
}

void ContactResolver::add(const RecipientList &recipients)
{
    Q_D(ContactResolver);

    if (!d->resolving) {
        bool ok = d->metaObject()->invokeMethod(d, "checkIfFinished", Qt::QueuedConnection);
        Q_UNUSED(ok);
        Q_ASSERT(ok);
    }

    d->resolving = true;
    foreach (const Recipient &recipient, recipients)
        d->resolve(recipient);
}

static QString contactName(const QContact &contact)
{
    return SeasideCache::generateDisplayLabel(contact, SeasideCache::displayLabelOrder());
}

// Returns true if resolved immediately
bool ContactResolverPrivate::resolve(Recipient recipient)
{
    if (!forceResolving && recipient.isContactResolved())
        return true;

    Q_ASSERT(!recipient.localUid().isEmpty());
    if (recipient.localUid().isEmpty() || recipient.remoteUid().isEmpty()) {
        // Cannot match any contact. Set as resolved to nothing.
        recipient.setResolvedContact(0, QString());
        return true;
    }

    if (pending.contains(recipient))
        return false;

    DEBUG() << "Resolve called for" << recipient;
    pending.insert(recipient);
    SeasideCache::CacheItem *item = 0;
    if (recipient.isPhoneNumber()) {
        item = SeasideCache::resolvePhoneNumber(this, recipient.remoteUid(), true);
    } else {
        item = SeasideCache::resolveOnlineAccount(this, recipient.localUid(), recipient.remoteUid(), true);
    }

    if (item && (item->contactState == SeasideCache::ContactComplete)) {
        recipient.setResolvedContact(item->iid, contactName(item->contact));
        DEBUG() << "Resolve returned result immediately" << recipient;
        return true;
    }

    return false;
}

bool ContactResolverPrivate::checkIfFinished()
{
    Q_Q(ContactResolver);
    if (resolving && pending.isEmpty()) {
        resolving = false;
        emit q->finished();
        return true;
    }
    return false;
}

void ContactResolverPrivate::addressResolved(const QString &first, const QString &second, SeasideCache::CacheItem *item)
{
    QSet<Recipient>::iterator it = pending.end();

    if (second.isEmpty()) {
        qWarning() << "Got addressResolved with empty UIDs" << first << second << item;
        return;
    } else if (first.isEmpty()) {
        // Phone numbers have no localUid, search the set manually
        for (it = pending.begin(); it != pending.end(); it++) {
            // second should be identical to input, so string equality is ok
            if (it->isPhoneNumber() && it->remoteUid() == second)
                break;
        }
    } else {
        it = pending.find(Recipient(first, second));
    }

    if (it == pending.end()) {
        qWarning() << "Got addressResolved that doesn't match any pending resolve tasks:" << first << second << item;
        foreach (const Recipient &r, pending)
            qWarning() << "Candidate" << r.isPhoneNumber() << r.remoteUid();
        return;
    }

    DEBUG() << "Resolved" << first << second << "to" << (item ? item->iid : -1) << (item ? contactName(item->contact) : QString());

    if (item)
        it->setResolvedContact(item->iid, contactName(item->contact));
    else
        it->setResolvedContact(0, QString());
    pending.erase(it);
    checkIfFinished();
}

#include "contactresolver.moc"


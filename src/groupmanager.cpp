/******************************************************************************
**
** This file is part of libcommhistory.
**
** Copyright (C) 2013 Jolla Ltd.
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
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

#include <QtDBus/QtDBus>
#include <QSqlQuery>

#include "commonutils.h"
#include "databaseio.h"
#include "databaseio_p.h"
#include "eventmodel.h"
#include "groupmanager.h"
#include "groupmanager_p.h"
#include "updatesemitter.h"
#include "group.h"
#include "event.h"
#include "constants.h"
#include "contactlistener.h"
#include "debug.h"

namespace {
static const int defaultChunkSize = 50;
}

using namespace CommHistory;

GroupManagerPrivate::GroupManagerPrivate(GroupManager *manager)
        : q_ptr(manager)
        , queryMode(EventModel::AsyncQuery)
        , chunkSize(defaultChunkSize)
        , firstChunkSize(0)
        , queryLimit(0)
        , queryOffset(0)
        , isReady(true)
        , filterLocalUid(QString())
        , filterRemoteUid(QString())
        , bgThread(0)
        , contactResolver(0)
        , resolveContacts(false)
{
    qRegisterMetaType<QList<CommHistory::Event> >();
    qRegisterMetaType<QList<CommHistory::Group> >();
    qRegisterMetaType<QList<int> >();

    emitter = UpdatesEmitter::instance();

    QDBusConnection::sessionBus().connect(
        QString(),
        QString(),
        COMM_HISTORY_SERVICE_NAME,
        EVENTS_ADDED_SIGNAL,
        this,
        SLOT(eventsAddedSlot(const QList<CommHistory::Event> &)));
    QDBusConnection::sessionBus().connect(
        QString(),
        QString(),
        COMM_HISTORY_SERVICE_NAME,
        GROUPS_ADDED_SIGNAL,
        this,
        SLOT(groupsAddedSlot(const QList<CommHistory::Group> &)));
    QDBusConnection::sessionBus().connect(
        QString(),
        QString(),
        COMM_HISTORY_SERVICE_NAME,
        GROUPS_UPDATED_SIGNAL,
        this,
        SLOT(groupsUpdatedSlot(const QList<int> &)));
    QDBusConnection::sessionBus().connect(
        QString(),
        QString(),
        COMM_HISTORY_SERVICE_NAME,
        GROUPS_UPDATED_FULL_SIGNAL,
        this,
        SLOT(groupsUpdatedFullSlot(const QList<CommHistory::Group> &)));
    QDBusConnection::sessionBus().connect(
        QString(),
        QString(),
        COMM_HISTORY_SERVICE_NAME,
        GROUPS_DELETED_SIGNAL,
        this,
        SLOT(groupsDeletedSlot(const QList<int> &)));
}

GroupManagerPrivate::~GroupManagerPrivate()
{
}

bool GroupManagerPrivate::commitTransaction(const QList<int> &groupIds)
{
    if (!database()->commit()) {
        emit q_ptr->groupsCommitted(groupIds, false);
        return false;
    } else {
        emit q_ptr->groupsCommitted(groupIds, true);
        return true;
    }
}

void GroupManagerPrivate::modifyInModel(Group &group, bool query)
{
    Q_Q(GroupManager);
    GroupObject *go = groups.value(group.id());
    if (!go)
        return;

    if (query) {
        Group newGroup;
        if (!database()->getGroup(group.id(), newGroup))
            return;
        go->set(newGroup);
    } else {
        go->copyValidProperties(group);
    }

    emit q->groupUpdated(go);
    DEBUG() << __PRETTY_FUNCTION__ << ": updated" << go->toString();
}

void GroupManagerPrivate::eventsAddedSlot(const QList<Event> &events)
{
    Q_Q(GroupManager);
    DEBUG() << __PRETTY_FUNCTION__ << events.count();

    foreach (const Event &event, events) {
        // statusmessages are not shown in group model
        if (event.type() == Event::StatusMessageEvent
            || event.type() == Event::ClassZeroSMSEvent) {
            continue;
        }

        GroupObject *go = groups.value(event.groupId());
        if (!go)
            continue;

        if (event.startTime() >= go->startTime()) {
            DEBUG() << __PRETTY_FUNCTION__ << ": updating group" << go->id();
            go->setLastEventId(event.id());
            if (event.type() == Event::MMSEvent) {
                go->setLastMessageText(event.subject().isEmpty() ? event.freeText() : event.subject());
            } else {
                go->setLastMessageText(event.freeText());
            }
            go->setLastVCardFileName(event.fromVCardFileName());
            go->setLastVCardLabel(event.fromVCardLabel());
            go->setLastEventStatus(event.status());
            go->setLastEventType(event.type());
            go->setLastEventIsDraft(event.isDraft());
            go->setStartTime(event.startTime());
            go->setEndTime(event.endTime());
        }

        if (!event.isRead())
            go->setUnreadMessages(go->unreadMessages() + 1);
        emit q->groupUpdated(go);
    }
}

void GroupManagerPrivate::groupsAddedSlot(const QList<CommHistory::Group> &addedGroups)
{
    Q_Q(GroupManager);
    DEBUG() << Q_FUNC_INFO << addedGroups.count();

    foreach (Group group, addedGroups) {
        GroupObject *go = groups.value(group.id());

        // If the group has not been added to the model, add it.
        if (!go
            && (filterLocalUid.isEmpty() || group.localUid() == filterLocalUid)
            && !group.recipients().isEmpty()
            && (filterRemoteUid.isEmpty()
                || group.recipients() == Recipient(group.localUid(), filterRemoteUid))) {
            go = new GroupObject(group, q);
            groups.insert(group.id(), go);
            emit q->groupAdded(go);
        }

        if (resolveContacts) {
            if (!contactResolver) {
                contactResolver = new ContactResolver(this);
                connect(contactResolver, SIGNAL(finished()),
                        this, SLOT(contactResolveFinished()));
            }

            pendingResolve.append(group);
            contactResolver->add(group);
        } else if (!groups.contains(group.id())) {
            GroupObject *go = new GroupObject(group, q);
            groups.insert(group.id(), go);
            emit q->groupAdded(go);
        }
    }
}

void GroupManagerPrivate::groupsUpdatedSlot(const QList<int> &groupIds)
{
    DEBUG() << __PRETTY_FUNCTION__ << groupIds.count();

    foreach (int id, groupIds) {
        Group g;
        g.setId(id);

        modifyInModel(g);
    }
}

void GroupManagerPrivate::groupsUpdatedFullSlot(const QList<CommHistory::Group> &groups)
{
    DEBUG() << __PRETTY_FUNCTION__ << groups.count();

    foreach (Group g, groups) {
        modifyInModel(g, false);
    }
}

void GroupManagerPrivate::groupsDeletedSlot(const QList<int> &groupIds)
{
    Q_Q(GroupManager);

    DEBUG() << __PRETTY_FUNCTION__ << groupIds.count();

    foreach (int id, groupIds) {
        GroupObject *go = groups.value(id);
        if (!go)
            continue;

        q->groupDeleted(go); 
        emit go->groupDeleted();
        go->deleteLater();
        groups.remove(id);
    }
}

bool GroupManagerPrivate::canFetchMore() const
{
    return false;
}

DatabaseIO* GroupManagerPrivate::database()
{
    return DatabaseIO::instance();
}

void GroupManagerPrivate::slotContactInfoChanged(const RecipientList &recipients)
{
    Q_Q(GroupManager);

    QSet<Recipient> changed = QSet<Recipient>::fromList(recipients.recipients());

    foreach (GroupObject *group, groups) {
        foreach (const Recipient &r, group->recipients()) {
            if (changed.contains(r)) {
                emit q->groupUpdated(group);
                break;
            }
        }
    }
}

GroupManager::GroupManager(QObject *parent)
    : QObject(parent),
      d(new GroupManagerPrivate(this))
{
}

GroupManager::~GroupManager()
{
    delete d;
    d = 0;
}

void GroupManager::setQueryMode(EventModel::QueryMode mode)
{
    d->queryMode = mode;
}

void GroupManager::setChunkSize(int size)
{
    d->chunkSize = size;
}

void GroupManager::setFirstChunkSize(int size)
{
    d->firstChunkSize = size;
}

void GroupManager::setLimit(int limit)
{
    d->queryLimit = limit;
}

void GroupManager::setOffset(int offset)
{
    d->queryOffset = offset;
}

GroupObject *GroupManager::group(int groupId) const
{
    return d->groups.value(groupId);
}

GroupObject *GroupManager::findGroup(const QString &localUid, const QString &remoteUid) const
{
    return findGroup(localUid, QStringList() << remoteUid);
}

GroupObject *GroupManager::findGroup(const QString &localUid, const QStringList &remoteUids) const
{
    RecipientList match = RecipientList::fromUids(localUid, remoteUids);
    foreach (GroupObject *g, d->groups) {
        if (g->localUid() == localUid && g->recipients() == match)
            return g;
    }

    return 0;
}

void GroupManagerPrivate::add(Group &group)
{
    Q_Q(GroupManager);

    DEBUG() << __PRETTY_FUNCTION__ << ": added" << group.toString();

    GroupObject *go = new GroupObject(group, q);
    groups.insert(go->id(), go);
    emit q->groupAdded(go);
}

bool GroupManager::addGroup(Group &group)
{
    if (!d->database()->transaction())
        return false;

    if (!d->database()->addGroup(group)) {
        d->database()->rollback();
        return false;
    }

    if (!d->commitTransaction(QList<int>() << group.id()))
        return false;

    if ((d->filterLocalUid.isEmpty() || group.localUid() == d->filterLocalUid)
        && (d->filterRemoteUid.isEmpty()
            || group.recipients() == Recipient(group.localUid(), d->filterRemoteUid))) {
        d->add(group);
    }

    d->emitter->groupsAdded(QList<Group>() << group);

    return true;
}

bool GroupManager::addGroups(QList<Group> &groups)
{
    QList<int> addedIds;
    QList<Group> addedGroups;

    QMutableListIterator<Group> i(groups);

    if (!d->database()->transaction())
        return false;

    while (i.hasNext()) {
        Group &group = i.next();
        if (!d->database()->addGroup(group)) {
            d->database()->rollback();
            return false;
        }

        if ((d->filterLocalUid.isEmpty() || group.localUid() == d->filterLocalUid)
            && (d->filterRemoteUid.isEmpty()
                || group.recipients() == Recipient(group.localUid(), d->filterRemoteUid))) {
            d->add(group);
        }

        addedIds.append(group.id());
        addedGroups.append(group);
    }

    if (!d->commitTransaction(addedIds))
        return false;

    d->emitter->groupsAdded(addedGroups);
    return true;
}

bool GroupManager::modifyGroup(Group &group)
{
    DEBUG() << Q_FUNC_INFO << group.id();

    if (group.id() == -1) {
        qWarning() << __FUNCTION__ << "Group id not set";
        return false;
    }

    if (!d->database()->transaction())
        return false;

    if (group.lastModified() == QDateTime::fromTime_t(0)) {
         group.setLastModified(QDateTime::currentDateTime());
    }

    if (!d->database()->modifyGroup(group)) {
        d->database()->rollback();
        return false;
    }

    if (!d->commitTransaction(QList<int>() << group.id()))
        return false;

    emit d->emitter->groupsUpdatedFull(QList<Group>() << group);
    return true;
}

bool GroupManager::getGroups(const QString &localUid,
                           const QString &remoteUid)
{
    d->filterLocalUid = localUid;
    d->filterRemoteUid = remoteUid;
    d->isReady = false;

    if (!d->groups.isEmpty()) {
        foreach (GroupObject *go, d->groups)
            emit groupDeleted(go);
        qDeleteAll(d->groups);
        d->groups.clear();
    }

    QString queryOrder;
    if (d->queryLimit > 0)
        queryOrder += QString::fromLatin1("LIMIT %1 ").arg(d->queryLimit);
    if (d->queryOffset > 0)
        queryOrder += QString::fromLatin1("OFFSET %1 ").arg(d->queryOffset);

    QList<Group> results;
    if (!d->database()->getGroups(localUid, remoteUid, results, queryOrder))
        return false;

    if (d->resolveContacts && d->queryMode != EventModel::SyncQuery) {
        if (!d->contactResolver) {
            d->contactResolver = new ContactResolver(this);
            connect(d->contactResolver, SIGNAL(finished()),
                    d, SLOT(contactResolveFinished()));
        }

        d->pendingResolve.append(results);
        d->contactResolver->add(results);
    } else {
        foreach (Group g, results) {
            GroupObject *go = new GroupObject(g, this);
            d->groups.insert(g.id(), go);
            emit groupAdded(go);
        }

        if (!d->isReady) {
            d->isReady = true;
            emit modelReady(true);
        }
    }

    return true;
}

void GroupManagerPrivate::contactResolveFinished()
{
    Q_Q(GroupManager);

    QList<Group> results = pendingResolve;
    pendingResolve.clear();

    DEBUG() << "Finished resolving" << results.size() << "groups";

    foreach (const Group &g, results) {
        GroupObject *go = groups.value(g.id());
        if (!go) {
            go = new GroupObject(g, q);
            DEBUG() << g.id() << g.recipients().debugString();
            groups.insert(g.id(), go);
            emit q->groupAdded(go);
        } else {
            emit q->groupUpdated(go);
        }
    }
}

bool GroupManager::markAsReadGroup(int id)
{
    DEBUG() << Q_FUNC_INFO << id;

    if (!d->database()->transaction())
        return false;

    if (!d->database()->markAsReadGroup(id)) {
        d->database()->rollback();
        return false;
    }

    if (!d->commitTransaction(QList<int>() << id))
        return false;

    GroupObject *group = 0;
    foreach (GroupObject *g, d->groups) {
        if (g->id() == id) {
            group = g;
            group->setUnreadMessages(0);
            break;
        }
    }

    if (group)
        emit d->emitter->groupsUpdatedFull(QList<Group>() << group->toGroup());
    else
        emit d->emitter->groupsUpdated(QList<int>() << id);

    return true;
}

void GroupManager::updateGroups(QList<Group> &groups)
{
    // no need to update d->groups
    // cause they will be updated on the emitted signal as well
    if (!groups.isEmpty())
        emit d->emitter->groupsUpdatedFull(groups);
}

bool GroupManager::deleteGroups(const QList<int> &groupIds)
{
    DEBUG() << Q_FUNC_INFO << groupIds;

    if (!d->database()->transaction())
        return false;

    if (!d->database()->deleteGroups(groupIds, d->bgThread)) {
        d->database()->rollback();
        return false;
    }

    if (!d->commitTransaction(groupIds))
        return false;

    emit d->emitter->groupsDeleted(groupIds);
    return true;
}

bool GroupManager::deleteAll()
{
    DEBUG() << Q_FUNC_INFO;

    QList<int> ids;
    foreach (GroupObject *group, d->groups) {
        ids << group->id();
    }

    if (ids.isEmpty())
        return true;

    return deleteGroups(ids);
}

bool GroupManager::canFetchMore() const
{
    return d->canFetchMore();
}

void GroupManager::fetchMore()
{
}

QList<GroupObject*> GroupManager::groups() const
{
    return d->groups.values();
}

bool GroupManager::isReady() const
{
    return d->isReady;
}

EventModel::QueryMode GroupManager::queryMode() const
{
    return d->queryMode;
}

int GroupManager::chunkSize() const
{
    return d->chunkSize;
}

int GroupManager::firstChunkSize() const
{
    return d->firstChunkSize;
}

int GroupManager::limit() const
{
    return d->queryLimit;
}

int GroupManager::offset() const
{
    return d->queryOffset;
}

void GroupManager::setBackgroundThread(QThread *thread)
{
    d->bgThread = thread;
}

QThread* GroupManager::backgroundThread()
{
    return d->bgThread;
}

DatabaseIO& GroupManager::databaseIO()
{
    return *d->database();
}

bool GroupManager::resolveContacts() const
{
    return d->resolveContacts;
}

void GroupManager::setResolveContacts(bool enabled)
{
    if (d->resolveContacts == enabled)
        return;
    d->resolveContacts = enabled;

    if (d->resolveContacts && !d->contactListener) {
        d->contactListener = ContactListener::instance();
        connect(d->contactListener.data(),
                SIGNAL(contactInfoChanged(RecipientList)),
                d,
                SLOT(slotContactInfoChanged(RecipientList)));
    } else if (!d->resolveContacts && d->contactListener) {
        disconnect(d->contactListener.data(), 0, d, 0);
        d->contactListener.clear();
    }
}


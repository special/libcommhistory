/******************************************************************************
**
** This file is part of libcommhistory.
**
** Copyright (C) 2013 Jolla Ltd. <matthew.vogt@jollamobile.com>
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

#include <QSqlQuery>
#include <QSqlError>

#include "databaseio_p.h"
#include "commhistorydatabase.h"
#include "eventmodel_p.h"
#include "contactlistener.h"

#include "recentcontactsmodel.h"
#include "debug.h"

namespace CommHistory {

using namespace CommHistory;

static int eventContact(const Event &event)
{
    return event.recipients().contactIds().value(0);
}

class RecentContactsModelPrivate : public EventModelPrivate {
public:
    Q_DECLARE_PUBLIC(RecentContactsModel);

    RecentContactsModelPrivate(EventModel *model)
        : EventModelPrivate(model)
    {
        setResolveContacts(true);
    }

    virtual bool acceptsEvent(const Event &event) const;
    virtual bool fillModel(int start, int end, QList<Event> events);
    virtual void prependEvents(QList<Event> events);

    virtual void slotContactInfoChanged(const RecipientList &recipients);
};

bool RecentContactsModelPrivate::acceptsEvent(const Event &event) const
{
    // Contact must be resolved before we can do anything, so just accept
    Q_UNUSED(event);
    return true;
}

bool RecentContactsModelPrivate::fillModel(int start, int end, QList<Event> events)
{
    Q_Q(RecentContactsModel);
    Q_UNUSED(start);
    Q_UNUSED(end);

    // This model doesn't fetchMore, so fill is only called once. We can use the prepend logic to get
    // the right contact behaviors.
    prependEvents(events);

    emit q->resolvingChanged();
    modelUpdatedSlot(true);
    return true;
}

void RecentContactsModelPrivate::slotContactInfoChanged(const RecipientList &recipients)
{
    EventModelPrivate::slotContactInfoChanged(recipients);

    // Remove any event for this contact (there can only be one)
    const int rowCount = eventRootItem->childCount();
    for (int row = 0; row < rowCount; ++row) {
        const Event &existing(eventRootItem->eventAt(row));
        if (existing.recipients().contactIds().isEmpty()) {
            deleteFromModel(existing.id());
            break;
        }
    }
}

void RecentContactsModelPrivate::prependEvents(QList<Event> events)
{
    Q_Q(RecentContactsModel);

    // Ensure the new events represent different contacts
    QList<Event> newEvents;
    QSet<int> newContactIds;
    foreach (const Event &event, events) {
        const int eventContactId = eventContact(event);
        if (eventContactId && !newContactIds.contains(eventContactId)) {
            newContactIds.insert(eventContactId);
            newEvents.append(event);

            // Don't add any more events than we can present
            if (newEvents.count() == queryLimit) {
                break;
            }
        }
    }

    if (newEvents.isEmpty())
        return;

    QSet<int> removeSet;

    // Does the new event replace an existing event?
    const int rowCount = eventRootItem->childCount();
    for (int row = 0; row < rowCount; ++row) {
        const Event &existing(eventRootItem->eventAt(row));
        if (newContactIds.contains(eventContact(existing))) {
            removeSet.insert(row);
        }
    }

    // Do we need to remove the final event(s) to maintain the limit?
    if (queryLimit) {
        int trimCount = rowCount + newEvents.count() - removeSet.count() - queryLimit;
        int removeIndex = rowCount - 1;
        while (trimCount > 0) {
            while (removeSet.contains(removeIndex)) {
                --removeIndex;
            }
            if (removeIndex < 0) {
                break;
            } else {
                removeSet.insert(removeIndex);
                --removeIndex;
                --trimCount;
            }
        }
    }

    // Remove the rows that have been made obsolete
    QList<int> removeIndices = removeSet.toList();
    qSort(removeIndices);

    int count;
    while ((count = removeIndices.count()) != 0) {
        int end = removeIndices.last();
        int consecutiveCount = 1;
        for ( ; (count - consecutiveCount) > 0; ++consecutiveCount) {
            if (removeIndices.at(count - 1 - consecutiveCount) != (end - consecutiveCount)) {
                break;
            }
        }

        removeIndices = removeIndices.mid(0, count - consecutiveCount);

        int start = (end - consecutiveCount + 1);
        q->beginRemoveRows(QModelIndex(), start, end);
        while (end >= start) {
            eventRootItem->removeAt(end);
            --end;
        }
        q->endRemoveRows();
    }

    // Insert the new events at the start
    int start = 0;
    q->beginInsertRows(QModelIndex(), start, newEvents.count() - 1);
    QList<Event>::const_iterator it = newEvents.constBegin(), end = newEvents.constEnd();
    for ( ; it != end; ++it) {
        eventRootItem->insertChildAt(start++, new EventTreeItem(*it, eventRootItem));
    }
    q->endInsertRows();
}

RecentContactsModel::RecentContactsModel(QObject *parent)
    : EventModel(*new RecentContactsModelPrivate(this), parent)
{
}

RecentContactsModel::~RecentContactsModel()
{
}

bool RecentContactsModel::resolving() const
{
    Q_D(const RecentContactsModel);

    return !d->isReady || (d->addResolver && d->addResolver->isResolving()) ||
           (d->receiveResolver && d->receiveResolver->isResolving());
}

bool RecentContactsModel::getEvents()
{
    Q_D(RecentContactsModel);

    beginResetModel();
    d->clearEvents();
    endResetModel();

    QString limitClause;
    if (d->queryLimit) {
        // Default to twice the configured limit, because some of the addresses may
        // resolve to the same final contact
        limitClause = QString::fromLatin1(" LIMIT %1").arg(2 * d->queryLimit);
    }

    /* Grouping by string indexes is expensive, so we avoid just querying for the
     * remoteUids of all events. This query finds all events with groups (messages),
     * then all events without groups, and returns the sorted union of them. */
    QString q = DatabaseIOPrivate::eventQueryBase();
    q += " JOIN Groups ON ("
          " Events.id = ("
           " SELECT id FROM Events WHERE groupId=Groups.id"
           " ORDER BY endTime DESC, id DESC LIMIT 1"
         " ))"
         " UNION ALL ";
    q += DatabaseIOPrivate::eventQueryBase();
    q += " JOIN ("
          " SELECT id, max(endTime) FROM Events"
          " WHERE groupId IS NULL GROUP BY remoteUid, endTime"
         " ) USING (id)";

    q += " ORDER BY endTime DESC, id DESC " + limitClause;

    QSqlQuery query = DatabaseIOPrivate::instance()->createQuery();
    if (!query.prepare(q)) {
        qWarning() << "Failed to execute query";
        qWarning() << query.lastError();
        qWarning() << query.lastQuery();
        return false;
    }

    bool re = d->executeQuery(query);
    if (re)
        emit resolvingChanged();
    return re;

}

} // namespace CommHistory

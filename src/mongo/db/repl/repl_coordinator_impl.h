/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <map>

#include "mongo/base/status.h"
#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

    class ReplicationCoordinatorImpl : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(ReplicationCoordinatorImpl);

    public:

        ReplicationCoordinatorImpl();
        virtual ~ReplicationCoordinatorImpl();

        // ================== Members of public ReplicationCoordinator API ===================

        virtual void startReplication();

        virtual void shutdown();

        virtual bool isShutdownOkay() const;

        virtual Mode getReplicationMode() const;

        virtual const MemberState& getCurrentMemberState() const;

        virtual Status awaitReplication(const OpTime& ts,
                                        const WriteConcernOptions& writeConcern,
                                        Milliseconds timeout);

        virtual bool canAcceptWritesFor(const NamespaceString& collection);

        virtual bool canServeReadsFor(const NamespaceString& collection);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(const HostAndPort& member, const OpTime& ts);

        // ================== Members of replication code internal API ===================

        // Called by the TopologyCoordinator whenever this node's replica set state transitions
        void setCurrentMemberState(const MemberState& newState);

        // Called by the TopologyCoordinator whenever the replica set configuration is updated
        void setCurrentReplicaSetConifg(const TopologyCoordinatorImpl::ReplicaSetConfig& newConfig);

    private:

        // Protects all member data of this ReplicationCoordinator
        boost::mutex _lock;

        // Condition variable that is signaled by setLastOptime to wake up any threads waiting
        // in awaitReplication for their write concern to be satisfied.
        boost::condition_variable _optimeUpdatedCondition;

        // Pointer to the TopologyCoordinator owned by this ReplicationCoordinator
        TopologyCoordinator* _topCoord;

        // Maps nodes in this replication group to the last oplog operation they have committed
        std::map<HostAndPort, OpTime> _hostOptimeMap;

        // Current ReplicaSet state // TODO(spencer): what about master/slave?
        MemberState _currentState;

        // The current ReplicaSet configuration object, including the information about tag groups
        // that is used to satisfy write concern requests with named gle modes.
        TopologyCoordinatorImpl::ReplicaSetConfig _rsConfig;
    };

} // namespace repl
} // namespace mongo

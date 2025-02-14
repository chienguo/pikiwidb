/*
 * Copyright (c) 2023-present, OpenAtom Foundation, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "cmd_raft.h"

#include <cstdint>
#include <optional>
#include <string>

// #include "net/event_loop.h"
#include "praft/praft.h"
#include "pstd/log.h"
#include "pstd/pstd_string.h"

#include "client.h"
#include "config.h"
#include "pikiwidb.h"
#include "replication.h"

namespace pikiwidb {

RaftNodeCmd::RaftNodeCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsRaft, kAclCategoryRaft) {}

bool RaftNodeCmd::DoInitial(PClient* client) {
  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);

  if (cmd != kAddCmd && cmd != kRemoveCmd && cmd != kDoSnapshot) {
    client->SetRes(CmdRes::kErrOther, "RAFT.NODE supports ADD / REMOVE / DOSNAPSHOT only");
    return false;
  }
  return true;
}

void RaftNodeCmd::DoCmd(PClient* client) {
  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);
  if (cmd == kAddCmd) {
    DoCmdAdd(client);
  } else if (cmd == kRemoveCmd) {
    DoCmdRemove(client);
  } else if (cmd == kDoSnapshot) {
    DoCmdSnapshot(client);
  } else {
    client->SetRes(CmdRes::kErrOther, "RAFT.NODE supports ADD / REMOVE / DOSNAPSHOT only");
  }
}

void RaftNodeCmd::DoCmdAdd(PClient* client) {
  // Check whether it is a leader. If it is not a leader, return the leader information
  if (!PRAFT.IsLeader()) {
    client->SetRes(CmdRes::kWrongLeader, PRAFT.GetLeaderID());
    return;
  }

  if (client->argv_.size() != 4) {
    client->SetRes(CmdRes::kWrongNum, client->CmdName());
    return;
  }

  // RedisRaft has nodeid, but in Braft, NodeId is IP:Port.
  // So we do not need to parse and use nodeid like redis;
  auto s = PRAFT.AddPeer(client->argv_[3]);
  if (s.ok()) {
    client->SetRes(CmdRes::kOK);
  } else {
    client->SetRes(CmdRes::kErrOther, fmt::format("Failed to add peer: {}", s.error_str()));
  }
}

void RaftNodeCmd::DoCmdRemove(PClient* client) {
  // If the node has been initialized, it needs to close the previous initialization and rejoin the other group
  if (!PRAFT.IsInitialized()) {
    client->SetRes(CmdRes::kErrOther, "Don't already cluster member");
    return;
  }

  if (client->argv_.size() != 3) {
    client->SetRes(CmdRes::kWrongNum, client->CmdName());
    return;
  }

  // Check whether it is a leader. If it is not a leader, send remove request to leader
  if (!PRAFT.IsLeader()) {
    // Get the leader information
    braft::PeerId leader_peer_id(PRAFT.GetLeaderID());
    // @todo There will be an unreasonable address, need to consider how to deal with it
    if (leader_peer_id.is_empty()) {
      client->SetRes(CmdRes::kErrOther,
                     "The leader address of the cluster is incorrect, try again or delete the node from another node");
      return;
    }

    // Connect target
    std::string peer_ip = butil::ip2str(leader_peer_id.addr.ip).c_str();
    auto port = leader_peer_id.addr.port - pikiwidb::g_config.raft_port_offset;
    auto peer_id = client->argv_[2];
    auto ret =
        PRAFT.GetClusterCmdCtx().Set(ClusterCmdType::kRemove, client, std::move(peer_ip), port, std::move(peer_id));
    if (!ret) {  // other clients have removed
      return client->SetRes(CmdRes::kErrOther, "Other clients have removed");
    }
    PRAFT.GetClusterCmdCtx().ConnectTargetNode();
    INFO("Sent remove request to leader successfully");

    // Not reply any message here, we will reply after the connection is established.
    client->Clear();
    return;
  }

  auto s = PRAFT.RemovePeer(client->argv_[2]);
  if (s.ok()) {
    client->SetRes(CmdRes::kOK);
  } else {
    client->SetRes(CmdRes::kErrOther, fmt::format("Failed to remove peer: {}", s.error_str()));
  }
}

void RaftNodeCmd::DoCmdSnapshot(PClient* client) {
  auto self_snapshot_index = PSTORE.GetBackend(client->GetCurrentDB())->GetStorage()->GetSmallestFlushedLogIndex();
  INFO("DoCmdSnapshot self_snapshot_index:{}", self_snapshot_index);
  auto s = PRAFT.DoSnapshot(self_snapshot_index);
  if (s.ok()) {
    client->SetRes(CmdRes::kOK);
  }
}

RaftClusterCmd::RaftClusterCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsRaft, kAclCategoryRaft) {}

bool RaftClusterCmd::DoInitial(PClient* client) {
  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);
  if (cmd != kInitCmd && cmd != kJoinCmd) {
    client->SetRes(CmdRes::kErrOther, "RAFT.CLUSTER supports INIT/JOIN only");
    return false;
  }
  return true;
}

void RaftClusterCmd::DoCmd(PClient* client) {
  if (PRAFT.IsInitialized()) {
    return client->SetRes(CmdRes::kErrOther, "Already cluster member");
  }

  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);
  if (cmd == kInitCmd) {
    DoCmdInit(client);
  } else {
    DoCmdJoin(client);
  }
}

void RaftClusterCmd::DoCmdInit(PClient* client) {
  if (client->argv_.size() != 2 && client->argv_.size() != 3) {
    return client->SetRes(CmdRes::kWrongNum, client->CmdName());
  }

  std::string cluster_id;
  if (client->argv_.size() == 3) {
    cluster_id = client->argv_[2];
    if (cluster_id.size() != RAFT_GROUPID_LEN) {
      return client->SetRes(CmdRes::kInvalidParameter,
                            "Cluster id must be " + std::to_string(RAFT_GROUPID_LEN) + " characters");
    }
  } else {
    cluster_id = pstd::RandomHexChars(RAFT_GROUPID_LEN);
  }
  auto s = PRAFT.Init(cluster_id, false);
  if (!s.ok()) {
    return client->SetRes(CmdRes::kErrOther, fmt::format("Failed to init node: ", s.error_str()));
  }
  client->SetRes(CmdRes::kOK);
}

static inline std::optional<std::pair<std::string, int32_t>> GetIpAndPortFromEndPoint(const std::string& endpoint) {
  auto pos = endpoint.find(':');
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  int32_t ret = 0;
  pstd::String2int(endpoint.substr(pos + 1), &ret);
  return {{endpoint.substr(0, pos), ret}};
}

void RaftClusterCmd::DoCmdJoin(PClient* client) {
  // If the node has been initialized, it needs to close the previous initialization and rejoin the other group
  if (PRAFT.IsInitialized()) {
    return client->SetRes(CmdRes::kErrOther,
                          "A node that has been added to a cluster must be removed \
      from the old cluster before it can be added to the new cluster");
  }

  if (client->argv_.size() < 3) {
    return client->SetRes(CmdRes::kWrongNum, client->CmdName());
  }

  // (KKorpse)TODO: Support multiple nodes join at the same time.
  if (client->argv_.size() > 3) {
    return client->SetRes(CmdRes::kInvalidParameter, "Too many arguments");
  }

  auto addr = client->argv_[2];
  if (braft::PeerId(addr).is_empty()) {
    return client->SetRes(CmdRes::kErrOther, fmt::format("Invalid ip::port: {}", addr));
  }

  auto ip_port = GetIpAndPortFromEndPoint(addr);
  if (!ip_port.has_value()) {
    return client->SetRes(CmdRes::kErrOther, fmt::format("Invalid ip::port: {}", addr));
  }
  auto& [peer_ip, port] = *ip_port;

  // Connect target
  auto ret = PRAFT.GetClusterCmdCtx().Set(ClusterCmdType::kJoin, client, std::move(peer_ip), port);
  if (!ret) {  // other clients have joined
    return client->SetRes(CmdRes::kErrOther, "Other clients have joined");
  }
  PRAFT.GetClusterCmdCtx().ConnectTargetNode();
  INFO("Sent join request to leader successfully");

  // Not reply any message here, we will reply after the connection is established.
  client->Clear();
}

}  // namespace pikiwidb

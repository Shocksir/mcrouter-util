/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <memory>
#include <string>

#include "mcrouter/config.h"
#include "mcrouter/config-impl.h"
#include "mcrouter/ProxyConfigIf.h"
#include "mcrouter/ProxyRequestPriority.h"
#include "mcrouter/ProxyRequestLogger.h"

namespace facebook { namespace memcache {

class McReply;
class McRequest;

namespace mcrouter {

class ProxyClientCommon;
class ProxyRoute;

/**
 * This object is alive for the duration of user's request,
 * including any subrequests that might have been sent out.
 *
 * It starts it's life under a unique_ptr outside of proxy threads.
 * When handed off to a proxy thread and ready to execute,
 * we save the current configuration and convert it to shared
 * ownership.
 *
 * Records collected stats on destruction.
 */
class ProxyRequestContext {
public:
  /**
   * Creates a new context
   */
  template <typename... Args>
  static std::unique_ptr<ProxyRequestContext> create(Args&&... args) {
    return std::unique_ptr<ProxyRequestContext>(
      new ProxyRequestContext(std::forward<Args>(args)...));
  }

  /**
   * Internally converts the context into one ready to route.
   * Config pointer is saved to keep the config alive, and
   * ownership is changed to shared so that all subrequests
   * keep track of this context.
   */
  static std::shared_ptr<ProxyRequestContext> process(
    std::unique_ptr<ProxyRequestContext> preq,
    std::shared_ptr<const ProxyConfigIf> config) {
    preq->config_ = std::move(config);
    return std::shared_ptr<ProxyRequestContext>(
      preq.release(),
      /* Note: we want to delete on main context here since the destructor
         can do complicated things, like finalize stats entry and
         destroy a stale config.  There might not be enough stack space
         for these operations. */
      [] (ProxyRequestContext* ctx) {
        folly::fibers::runInMainContext([ctx]{ delete ctx; });
      });
  }

  using ClientCallback = std::function<void(const ProxyClientCommon&)>;
  using ShardSplitCallback = std::function<void(const ShardSplitter&)>;

  /**
   * A request with this context will not be sent/logged anywhere.
   *
   * @param clientCallback  If non-nullptr, called by DestinationRoute when
   *   the request would normally be sent to destination;
   *   also in traverse() of DestinationRoute.
   * @param shardSplitCallback  If non-nullptr, called by ShardSplitRoute
   *   in traverse() with itself as the argument.
   */
  static std::shared_ptr<ProxyRequestContext> createRecording(
    proxy_t& proxy,
    ClientCallback clientCallback,
    ShardSplitCallback shardSplitCallback = nullptr);

  /**
   * Same as createRecording(), but also notifies the baton
   * when this context is destroyed (i.e. all requests referencing it
   * finish executing).
   */
  static std::shared_ptr<ProxyRequestContext> createRecordingNotify(
    proxy_t& proxy,
    folly::fibers::Baton& baton,
    ClientCallback clientCallback,
    ShardSplitCallback shardSplitCallback = nullptr);

  ~ProxyRequestContext();

  proxy_t& proxy() const {
    return proxy_;
  }

  bool recording() const noexcept {
    return recording_;
  }

  void recordDestination(const ProxyClientCommon& destination) const {
    if (recording_ && recordingState_->clientCallback) {
      recordingState_->clientCallback(destination);
    }
  }

  void recordShardSplitter(const ShardSplitter& splitter) const {
    if (recording_ && recordingState_->shardSplitCallback) {
      recordingState_->shardSplitCallback(splitter);
    }
  }

  uint64_t senderId() const;

  void setSenderIdForTest(uint64_t id);

  ProxyRoute& proxyRoute() const {
    assert(!recording_);
    return config_->proxyRoute();
  }

  const ProxyConfigIf& proxyConfig() const {
    assert(!recording_);
    return *config_;
  }

  bool failoverDisabled() const {
    return failoverDisabled_;
  }

  ProxyRequestPriority priority() const {
    return priority_;
  }

  /**
   * Called once a reply is received to record a stats sample if required.
   */
  template <typename Operation>
  void onReplyReceived(const ProxyClientCommon& pclient,
                       const McRequest& request,
                       const McReply& reply,
                       const int64_t startTimeUs,
                       const int64_t endTimeUs,
                       Operation) {
    if (recording_) {
      return;
    }

    assert(logger_.hasValue());
    logger_->log(request, reply, startTimeUs, endTimeUs, Operation());
    assert(additionalLogger_.hasValue());
    additionalLogger_->log(
      pclient, request, reply, startTimeUs, endTimeUs, Operation());
  }

  const McMsgRef& origReq() const {
    return origReq_;
  }

  /**
   * Sets the reply for this proxy request and sends it out
   * @param newReply the message that we are sending out as the reply
   *   for the request we are currently handling
   */
  void sendReply(McReply newReply);

  const std::string& userIpAddress() const noexcept {
    return userIpAddr_;
  }

  void setUserIpAddress(folly::StringPiece newAddr) noexcept {
    userIpAddr_ = newAddr.str();
  }

  /**
   * Returns the id of this requests.
   */
  uint64_t requestId() const;

 private:
  const uint64_t requestId_;
  proxy_t& proxy_;
  McMsgRef origReq_;
  folly::Optional<McReply> reply_;
  folly::Optional<McRequest> savedRequest_;
  bool replied_{false};
  bool failoverDisabled_{false};

  /** If true, this is currently being processed by a proxy and
      we want to notify we're done on destruction. */
  bool processing_{false};

  bool recording_{false};

  std::shared_ptr<McrouterClient> requester_;

  struct RecordingState {
    ClientCallback clientCallback;
    ShardSplitCallback shardSplitCallback;
  };

  union {
    void* context_{nullptr};
    std::unique_ptr<RecordingState> recordingState_;
  };

  /**
   * The function that will be called when the reply is ready
   */
  void (*enqueueReply_)(ProxyRequestContext& preq){nullptr};

  /**
   * The function that will be called when all replies (including async)
   * come back.
   * Guaranteed to be called after enqueueReply_ (right after in sync mode).
   */
  void (*reqComplete_)(ProxyRequestContext& preq){nullptr};

  std::shared_ptr<const ProxyConfigIf> config_;

  folly::Optional<ProxyRequestLogger> logger_;
  folly::Optional<AdditionalProxyRequestLogger> additionalLogger_;

  uint64_t senderIdForTest_{0};

  ProxyRequestPriority priority_{ProxyRequestPriority::kCritical};

  std::string userIpAddr_;

  ProxyRequestContext(
    proxy_t& pr,
    McMsgRef req,
    void (*enqReply)(ProxyRequestContext& preq),
    void* context,
    ProxyRequestPriority priority = ProxyRequestPriority::kCritical,
    void (*reqComplete)(ProxyRequestContext& preq) = nullptr);

  enum RecordingT { Recording };
  ProxyRequestContext(
    RecordingT,
    proxy_t& pr,
    ClientCallback clientCallback,
    ShardSplitCallback shardSplitCallback);

  ProxyRequestContext(const ProxyRequestContext&) = delete;
  ProxyRequestContext(ProxyRequestContext&&) noexcept = delete;
  ProxyRequestContext& operator=(const ProxyRequestContext&) = delete;
  ProxyRequestContext& operator=(ProxyRequestContext&&) = delete;

 public:
  /* Do not use for new code */
  class LegacyPrivateAccessor {
   public:
    static void*& context(ProxyRequestContext& preq) {
      assert(!preq.recording_);
      return preq.context_;
    }

    static bool& failoverDisabled(ProxyRequestContext& preq) {
      return preq.failoverDisabled_;
    }

    static McReply& reply(ProxyRequestContext& preq) {
      return preq.reply_.value();
    }
  };

private:
  friend class McrouterClient;
  friend class proxy_t;
};

}}}  // facebook::memcache::mcrouter

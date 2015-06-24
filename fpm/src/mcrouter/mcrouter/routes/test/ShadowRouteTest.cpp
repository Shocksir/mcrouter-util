/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "mcrouter/lib/test/RouteHandleTestUtil.h"
#include "mcrouter/McrouterFiberContext.h"
#include "mcrouter/McrouterInstance.h"
#include "mcrouter/ProxyRequestContext.h"
#include "mcrouter/routes/DefaultShadowPolicy.h"
#include "mcrouter/routes/McrouterRouteHandle.h"
#include "mcrouter/routes/ShadowRoute.h"
#include "mcrouter/routes/ShadowRouteIf.h"

using namespace facebook::memcache;
using namespace facebook::memcache::mcrouter;

using std::make_shared;
using std::string;
using std::vector;

using TestHandle = TestHandleImpl<McrouterRouteHandleIf>;

namespace {

McrouterInstance* getRouter() {
  McrouterOptions opts = defaultTestOptions();
  opts.config_str = "{ \"route\": \"NullRoute\" }";
  return McrouterInstance::init("test_shadow", opts);
}

std::shared_ptr<ProxyRequestContext> getContext() {
  return ProxyRequestContext::createRecording(*getRouter()->getProxy(0),
                                              nullptr);
}

}  // anonymous namespace

TEST(shadowRouteTest, defaultPolicy) {
  vector<std::shared_ptr<TestHandle>> normalHandle{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "a")),
  };
  auto normalRh = get_route_handles(normalHandle)[0];

  vector<std::shared_ptr<TestHandle>> shadowHandles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "c")),
  };

  TestFiberManager fm{fiber_local::ContextTypeTag()};

  auto settings = ShadowSettings::create(
      folly::dynamic::object("index_range", folly::dynamic{ 0, 1 }),
      *getRouter());

  auto shadowRhs = get_route_handles(shadowHandles);
  McrouterShadowData shadowData{
    {std::move(shadowRhs[0]), settings},
    {std::move(shadowRhs[1]), settings},
  };

  McrouterRouteHandle<ShadowRoute<DefaultShadowPolicy>> rh(
    normalRh,
    std::move(shadowData),
    DefaultShadowPolicy());

  auto ctx = getContext();
  fm.run([&] () {
    fiber_local::setSharedCtx(ctx);
    auto reply = rh.route(McRequest("key"), McOperation<mc_op_get>());

    EXPECT_TRUE(reply.result() == mc_res_found);
    EXPECT_TRUE(toString(reply.value()) == "a");
  });

  EXPECT_TRUE(shadowHandles[0]->saw_keys.empty());
  EXPECT_TRUE(shadowHandles[1]->saw_keys.empty());
  settings->setKeyRange(0, 1);

  fm.run([&] () {
    fiber_local::setSharedCtx(ctx);
    auto reply = rh.route(McRequest("key"), McOperation<mc_op_get>());

    EXPECT_TRUE(reply.result() == mc_res_found);
    EXPECT_TRUE(toString(reply.value()) == "a");
  });

  EXPECT_TRUE(shadowHandles[0]->saw_keys == vector<string>{"key"});
  EXPECT_TRUE(shadowHandles[1]->saw_keys == vector<string>{"key"});
}

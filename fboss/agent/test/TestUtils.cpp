/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/test/TestUtils.h"

#include <boost/cast.hpp>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "fboss/agent/ApplyThriftConfig.h"
#include "fboss/agent/RxPacket.h"
#include "fboss/agent/TunManager.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/mock/MockHwSwitch.h"
#include "fboss/agent/hw/mock/MockPlatform.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/state/VlanMap.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/RouteUpdater.h"
#include "fboss/agent/test/MockTunManager.h"
#include <folly/Memory.h>
#include <folly/json.h>

using folly::MacAddress;
using folly::IPAddress;
using folly::ByteRange;
using folly::IOBuf;
using folly::io::Cursor;
using folly::make_unique;
using folly::StringPiece;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using ::testing::_;
using ::testing::Return;

namespace facebook { namespace fboss {

shared_ptr<SwitchState> publishAndApplyConfig(
    shared_ptr<SwitchState>& state,
    const cfg::SwitchConfig* config,
    const Platform* platform,
    const cfg::SwitchConfig* prevCfg) {
  state->publish();
  return applyThriftConfig(state, config, platform, prevCfg);
}

shared_ptr<SwitchState> publishAndApplyConfigFile(
    shared_ptr<SwitchState>& state,
    StringPiece path,
    const Platform* platform,
    std::string prevConfigStr) {
  state->publish();
  // Parse the prev JSON config.
  cfg::SwitchConfig prevConfig;
  if (prevConfigStr.size()) {
    apache::thrift::SimpleJSONSerializer::deserialize<cfg::SwitchConfig>(
        prevConfigStr.c_str(), prevConfig);
  }
  return applyThriftConfigFile(state, path, platform, &prevConfig).first;
}

unique_ptr<SwSwitch> createMockSw(const shared_ptr<SwitchState>& state) {
  auto sw = make_unique<SwSwitch>(make_unique<MockPlatform>());
  auto stateAndBootType = std::make_pair(state, BootType::COLD_BOOT);
  HwInitResult ret;
  ret.switchState = state;
  ret.bootType = BootType::COLD_BOOT;
  EXPECT_HW_CALL(sw, init(_)).WillOnce(Return(ret));
  EXPECT_PLATFORM_CALL(sw, onHwInitialized(_)).Times(1);
  sw->init(nullptr);
  waitForStateUpdates(sw.get());
  return sw;
}

unique_ptr<SwSwitch> createMockSw(const shared_ptr<SwitchState>& state,
                                  const MacAddress& mac) {
  auto platform = make_unique<MockPlatform>();
  EXPECT_CALL(*platform.get(), getLocalMac()).WillRepeatedly(Return(mac));
  auto sw = make_unique<SwSwitch>(std::move(platform));
  auto stateAndBootType = std::make_pair(state, BootType::COLD_BOOT);
  HwInitResult ret;
  ret.switchState = state;
  ret.bootType = BootType::COLD_BOOT;
  EXPECT_HW_CALL(sw, init(_)).WillOnce(Return(ret));
  EXPECT_PLATFORM_CALL(sw, onHwInitialized(_)).Times(1);
  sw->init(nullptr);
  waitForStateUpdates(sw.get());
  return sw;
}

unique_ptr<SwSwitch> createMockSw(cfg::SwitchConfig* config,
                                  MacAddress mac,
                                  uint32_t maxPort,
                                  SwitchFlags flags) {
  // Create the initial state, which only has ports
  auto initialState = make_shared<SwitchState>();
  if (maxPort == 0) {
    for (const auto& port : config->ports) {
      maxPort = std::max(static_cast<int32_t>(maxPort), port.logicalID);
    }
  }
  for (uint32_t idx = 1; idx <= maxPort; ++idx) {
    initialState->registerPort(PortID(idx), folly::to<string>("port", idx));
  }

  // Create the SwSwitch
  auto platform = make_unique<MockPlatform>();
  EXPECT_CALL(*platform.get(), getLocalMac()).WillRepeatedly(Return(mac));
  auto sw = make_unique<SwSwitch>(std::move(platform));
  auto stateAndBootType = std::make_pair(initialState, BootType::COLD_BOOT);

  HwInitResult ret;
  ret.switchState = initialState;
  ret.bootType = BootType::COLD_BOOT;
  EXPECT_HW_CALL(sw, init(_)).WillOnce(Return(ret));
  EXPECT_PLATFORM_CALL(sw, onHwInitialized(_)).Times(1);

  if (flags & ENABLE_TUN) {
    // TODO(aeckert): I don't think this should be a first class
    // argument to SwSwitch::init() as unit tests are the only place
    // that pass in a TunManager to init(). Let's come up with a way
    // to mock the TunManager initialization instead of passing it in
    // like this.
    auto mockTunMgr = new MockTunManager(sw.get(), sw->getBackgroundEVB());
    std::unique_ptr<TunManager> tunMgr(mockTunMgr);
    sw->init(std::move(tunMgr), flags);
  } else {
    sw->init(nullptr, flags);
  }

  // Apply the thrift config
  auto updateFn = [&](const shared_ptr<SwitchState>& state) {
    return applyThriftConfig(state, config, sw->getPlatform());
  };
  sw->updateStateBlocking("test_setup", updateFn);
  return sw;
}

unique_ptr<SwSwitch> createMockSw(cfg::SwitchConfig* config,
                                  uint32_t maxPort,
                                  SwitchFlags flags) {
  return createMockSw(config, MacAddress("02:00:00:00:00:01"), maxPort, flags);
}

MockHwSwitch* getMockHw(SwSwitch* sw) {
  return boost::polymorphic_downcast<MockHwSwitch*>(sw->getHw());
}

MockPlatform* getMockPlatform(SwSwitch* sw) {
  return boost::polymorphic_downcast<MockPlatform*>(sw->getPlatform());
}

void waitForStateUpdates(SwSwitch* sw) {
  // All StateUpdates scheduled from this thread will be applied in order,
  // so we can simply perform a blocking no-op update.  When it is done
  // we can be sure that all previously scheduled updates have also been
  // applied.
  auto noopUpdate = [](const shared_ptr<SwitchState>& state) {
    return shared_ptr<SwitchState>();
  };
  sw->updateStateBlocking("waitForStateUpdates", noopUpdate);
}

void waitForBackgroundThread(SwSwitch* sw) {
  auto* evb = sw->getBackgroundEVB();
  evb->runInEventBaseThreadAndWait([]() { return; });
}

shared_ptr<SwitchState> testStateA() {
  // Setup a default state object
  auto state = make_shared<SwitchState>();

  // Add VLAN 1, and ports 1-10 which belong to it.
  auto vlan1 = make_shared<Vlan>(VlanID(1), "Vlan1");
  state->addVlan(vlan1);
  for (int idx = 1; idx <= 10; ++idx) {
    state->registerPort(PortID(idx), folly::to<string>("port", idx));
    vlan1->addPort(PortID(idx), false);
  }
  // Add VLAN 55, and ports 11-20 which belong to it.
  auto vlan55 = make_shared<Vlan>(VlanID(55), "Vlan55");
  state->addVlan(vlan55);
  for (int idx = 11; idx <= 20; ++idx) {
    state->registerPort(PortID(idx), folly::to<string>("port", idx));
    vlan55->addPort(PortID(idx), false);
  }
  // Add Interface 1 to VLAN 1
  auto intf1 = make_shared<Interface>(
      InterfaceID(1),
      RouterID(0),
      VlanID(1),
      "interface1",
      MacAddress("00:02:00:00:00:01"),
      9000,
      false /* is virtual */);
  Interface::Addresses addrs1;
  addrs1.emplace(IPAddress("10.0.0.1"), 24);
  addrs1.emplace(IPAddress("192.168.0.1"), 24);
  addrs1.emplace(IPAddress("2401:db00:2110:3001::0001"), 64);
  intf1->setAddresses(addrs1);
  state->addIntf(intf1);
  vlan1->setInterfaceID(InterfaceID(1));

  // Add Interface 55 to VLAN 55
  auto intf55 = make_shared<Interface>(
      InterfaceID(55),
      RouterID(0),
      VlanID(55),
      "interface55",
      MacAddress("00:02:00:00:00:55"),
      9000,
      false /* is virtual */);
  Interface::Addresses addrs55;
  addrs55.emplace(IPAddress("10.0.55.1"), 24);
  addrs55.emplace(IPAddress("192.168.55.1"), 24);
  addrs55.emplace(IPAddress("2401:db00:2110:3055::0001"), 64);
  intf55->setAddresses(addrs55);
  state->addIntf(intf55);
  vlan55->setInterfaceID(InterfaceID(55));


  RouteUpdater updater(state->getRouteTables());
  updater.addInterfaceAndLinkLocalRoutes(state->getInterfaces());

  RouteNextHops nexthops;
  nexthops.emplace(IPAddress("10.0.0.22")); // resolved by intf 1
  nexthops.emplace(IPAddress("10.0.0.23")); // resolved by intf 1
  nexthops.emplace(IPAddress("1.1.2.10")); // un-resolvable

  updater.addRoute(RouterID(0), IPAddress("10.1.1.0"), 24, nexthops);

  auto newRt = updater.updateDone();
  state->resetRouteTables(newRt);

  return state;
}

shared_ptr<SwitchState> testStateB() {
  // Setup a default state object
  auto state = make_shared<SwitchState>();

  // Add VLAN 1, and ports 1-9 which belong to it.
  auto vlan1 = make_shared<Vlan>(VlanID(1), "Vlan1");
  state->addVlan(vlan1);
  for (int idx = 1; idx <= 10; ++idx) {
    state->registerPort(PortID(idx), folly::to<string>("port", idx));
    vlan1->addPort(PortID(idx), false);
  }

  // Add Interface 1 to VLAN 1
  auto intf1 = make_shared<Interface>(
      InterfaceID(1),
      RouterID(0),
      VlanID(1),
      "interface1",
      MacAddress("00:02:00:00:00:01"),
      9000,
      false /* is virtual */);
  Interface::Addresses addrs1;
  addrs1.emplace(IPAddress("10.0.0.1"), 24);
  addrs1.emplace(IPAddress("192.168.0.1"), 24);
  addrs1.emplace(IPAddress("2401:db00:2110:3001::0001"), 64);
  intf1->setAddresses(addrs1);
  state->addIntf(intf1);
  vlan1->setInterfaceID(InterfaceID(1));
  RouteUpdater updater(state->getRouteTables());
  updater.addInterfaceAndLinkLocalRoutes(state->getInterfaces());
  auto newRt = updater.updateDone();
  state->resetRouteTables(newRt);
  return state;
}

std::string fbossHexDump(const IOBuf* buf) {
  Cursor cursor(buf);
  size_t length = cursor.totalLength();
  if (length == 0) {
    return "";
  }

  string result;
  result.reserve(length * 3);
  folly::format(&result, "{:02x}", cursor.read<uint8_t>());
  for (size_t n = 1; n < length; ++n) {
    folly::format(&result, " {:02x}", cursor.read<uint8_t>());
  }

  return result;
}

std::string fbossHexDump(const IOBuf& buf) {
  return fbossHexDump(&buf);
}

std::string fbossHexDump(folly::ByteRange buf) {
  IOBuf iobuf(IOBuf::WRAP_BUFFER, buf);
  return fbossHexDump(&iobuf);
}

std::string fbossHexDump(folly::StringPiece buf) {
  IOBuf iobuf(IOBuf::WRAP_BUFFER, ByteRange(buf));
  return fbossHexDump(&iobuf);
}

std::string fbossHexDump(const string& buf) {
  IOBuf iobuf(IOBuf::WRAP_BUFFER, ByteRange(StringPiece(buf)));
  return fbossHexDump(&iobuf);
}

TxPacketMatcher::TxPacketMatcher(StringPiece name, TxMatchFn fn)
  : name_(name.str()), fn_(std::move(fn)) {}

::testing::Matcher<shared_ptr<TxPacket>> TxPacketMatcher::createMatcher(
    folly::StringPiece name,
    TxMatchFn&& fn) {
  return ::testing::MakeMatcher(new TxPacketMatcher(name, std::move(fn)));
}

bool TxPacketMatcher::MatchAndExplain(
    shared_ptr<TxPacket> pkt, ::testing::MatchResultListener* l) const {
  try {
    fn_(pkt.get());
    return true;
  } catch (const std::exception& ex) {
    *l << ex.what();
    return false;
  }
}

void TxPacketMatcher::DescribeTo(std::ostream* os) const {
  *os << name_;
}

void TxPacketMatcher::DescribeNegationTo(std::ostream* os) const {
  *os << "not " << name_;
}

RxPacketMatcher::RxPacketMatcher(
    StringPiece name, InterfaceID dstIfID, RxMatchFn fn)
    : name_(name.str()), dstIfID_(dstIfID), fn_(std::move(fn)) {}

::testing::Matcher<RxMatchFnArgs>
RxPacketMatcher::createMatcher(
    folly::StringPiece name,
    InterfaceID dstIfID,
    RxMatchFn&& fn) {
  return ::testing::MakeMatcher(
      new RxPacketMatcher(name, dstIfID, std::move(fn)));
}

bool RxPacketMatcher::MatchAndExplain(
    RxMatchFnArgs args,
    ::testing::MatchResultListener* l) const {
  auto dstIfID = std::get<0>(args);
  auto pkt = std::get<1>(args);

  try {
    if (dstIfID != dstIfID_) {
      throw FbossError(
          "Mismatching dstIfID. Expected ", dstIfID_,
          " but received ", dstIfID);
    }

    fn_(pkt.get());
    return true;
  } catch (const std::exception& ex) {
    *l << ex.what();
    return false;
  }
}

void RxPacketMatcher::DescribeTo(std::ostream* os) const {
  *os << name_;
}

void RxPacketMatcher::DescribeNegationTo(std::ostream* os) const {
  *os << "not " << name_;
}

RoutePrefixV4 prefixV4(std::string str) {
  std::vector<std::string> vec;
  folly::split("/", str, vec);
  EXPECT_EQ(2, vec.size());
  auto prefix = RoutePrefixV4{folly::IPAddressV4(vec.at(0)),
                              folly::to<uint8_t>(vec.at(1))};
  return prefix;
}

RoutePrefixV6 prefixV6(std::string str) {
  std::vector<std::string> vec;
  folly::split("/", str, vec);
  EXPECT_EQ(2, vec.size());
  auto prefix = RoutePrefixV6{folly::IPAddressV6(vec.at(0)),
                              folly::to<uint8_t>(vec.at(1))};
  return prefix;
}


std::shared_ptr<Route<folly::IPAddressV4>>
GET_ROUTE_V4(const std::shared_ptr<RouteTableMap>& tables,
             RouterID rid, RoutePrefixV4 prefix) {
  EXPECT_NE(nullptr, tables);
  auto table = tables->getRouteTableIf(rid);
  EXPECT_NE(nullptr, table);
  auto rib4 = table->getRibV4();
  EXPECT_NE(nullptr, rib4);
  auto rt = rib4->exactMatch(prefix);
  EXPECT_NE(nullptr, rt);
  return rt;
}

std::shared_ptr<Route<folly::IPAddressV4>>
GET_ROUTE_V4(const std::shared_ptr<RouteTableMap>& tables,
             RouterID rid, std::string prefixStr) {
  return GET_ROUTE_V4(tables, rid, prefixV4(prefixStr));
}

std::shared_ptr<Route<folly::IPAddressV6>>
GET_ROUTE_V6(const std::shared_ptr<RouteTableMap>& tables,
             RouterID rid, RoutePrefixV6 prefix) {
  EXPECT_NE(nullptr, tables);
  auto table = tables->getRouteTableIf(rid);
  EXPECT_NE(nullptr, table);
  auto rib6 = table->getRibV6();
  EXPECT_NE(nullptr, rib6);
  auto rt = rib6->exactMatch(prefix);
  EXPECT_NE(nullptr, rt);
  return rt;
}

std::shared_ptr<Route<folly::IPAddressV6>>
GET_ROUTE_V6(const std::shared_ptr<RouteTableMap>& tables,
             RouterID rid, std::string prefixStr) {
  return GET_ROUTE_V6(tables, rid, prefixV6(prefixStr));
}

}} // namespace facebook::fboss

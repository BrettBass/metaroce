/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <libsoftmetaroce/congestion_control/BaselineWithRCN.h>
#include <libsoftmetaroce/congestion_control/CongestionControllerFactory.h>

using namespace testing;
namespace libsoftmetaroce {

class CongestionControllerFactoryTest : public ::testing::Test {
 protected:
  // The spec string is the only way an operator reaches these constructors, so
  // the cases below assert on what a spec produces rather than on a directly
  // constructed controller.
  static BaselineWithRCN* makeRcn(
      CongestionControllerFactory& factory,
      std::unique_ptr<CongestionController>& owner,
      const std::string& spec) {
    owner = factory.makeCongestionController(spec);
    return dynamic_cast<BaselineWithRCN*>(owner.get());
  }
};

TEST_F(CongestionControllerFactoryTest, BareNameUsesTheDocumentedDefaults) {
  CongestionControllerFactory factory;
  std::unique_ptr<CongestionController> owner;
  BaselineWithRCN* cc = makeRcn(factory, owner, "baseline_rcn");

  ASSERT_NE(cc, nullptr)
      << "baseline_rcn is the default value of --libsoftmetaroce_cc_algorithm, "
         "so it must build or every process fails flag validation";
  EXPECT_EQ(cc->gammaNumerator(), BaselineWithRCN::kDefaultGammaNumerator)
      << "a bare name takes the constructor's own defaults; a different value "
         "here means the spec arguments are being bound to the wrong "
         "parameters, which silently scales qwnd_max";
  EXPECT_EQ(cc->omegaNs(), BaselineWithRCN::kDefaultOmegaNs)
      << "omega is RTT headroom in nanoseconds, so a wrong default here "
         "changes the rate-delay product every ack is measured against";
}

TEST_F(CongestionControllerFactoryTest, ArgumentsBindInSpecOrder) {
  CongestionControllerFactory factory;
  std::unique_ptr<CongestionController> owner;
  BaselineWithRCN* cc = makeRcn(factory, owner, "baseline_rcn:2,1,8,2000");

  ASSERT_NE(cc, nullptr) << "a fully specified baseline_rcn spec must parse";
  EXPECT_EQ(cc->gammaNumerator(), 8u)
      << "the third argument is gammaNumerator; binding it elsewhere would "
         "leave the operator's requested gamma unapplied";
  EXPECT_EQ(cc->omegaNs(), 2000u)
      << "the fourth argument is omegaNs; binding it elsewhere would leave the "
         "operator's requested headroom unapplied";
}

TEST_F(CongestionControllerFactoryTest, LambdaIsTheTrailingArgument) {
  CongestionControllerFactory factory;
  // With beta a whole packet per mark and lambda half a packet per lost packet,
  // a loss of two shows which of them the spec bound.
  const auto windowAfterALossOfTwo = [&factory](const std::string& spec) {
    auto cc = factory.makeCongestionController(spec);
    if (cc == nullptr) {
      ADD_FAILURE() << spec << " should parse";
      return 0u;
    }
    cc->onFlowAdded(0);
    for (int sent = 0; sent < 8; ++sent) {
      cc->onPacketSent(/*flowId=*/0);
    }
    cc->onPacketAck(
        /*flowId=*/0,
        CongestionController::AckEvent{
            .numAckedPackets = 1,
            .measuredRttNs = 1000,
            .smoothedQpRttNs = 2000});
    cc->onPacketLoss(
        /*flowId=*/0,
        CongestionController::LossEvent{
            .numLostPackets = 2,
            .lossType = CongestionController::LossEvent::LossType::OutOfOrder});
    return cc->getQwnd();
  };

  EXPECT_EQ(windowAfterALossOfTwo("baseline:2,0,1"), 7u)
      << "the third baseline argument is lambda, so the loss should take one "
         "packet off the eight; bound anywhere else it would take two";
  EXPECT_EQ(windowAfterALossOfTwo("baseline_rcn:2,0,16,1000,1"), 7u)
      << "the fifth baseline_rcn argument is lambda; the four before it keep "
         "their places so existing specs mean what they did";
}

TEST_F(CongestionControllerFactoryTest, ZeroGammaIsRejectedNotFatal) {
  CongestionControllerFactory factory;

  EXPECT_EQ(factory.makeCongestionController("baseline_rcn:2,1,0"), nullptr)
      << "gammaNumerator divides the qwnd_max computation, and it arrives from "
         "an operator-supplied flag, so a zero must fail validation rather "
         "than abort the process";
}

TEST_F(CongestionControllerFactoryTest, ABetaThatCannotShiftAnIntIsRejected) {
  CongestionControllerFactory factory;

  EXPECT_EQ(factory.makeCongestionController("baseline:2,-1"), nullptr)
      << "beta is a shift amount, and shifting by a negative count is "
         "undefined behavior, so it must fail validation";
  EXPECT_EQ(factory.makeCongestionController("baseline:2,31"), nullptr)
      << "a shift as wide as an int is undefined behavior in the window "
         "reduction, so it must fail validation";
  EXPECT_NE(factory.makeCongestionController("baseline:2,30"), nullptr)
      << "the widest shift an int allows is still a valid beta";
  EXPECT_EQ(factory.makeCongestionController("baseline_rcn:2,-1"), nullptr)
      << "baseline_rcn reduces on marks by beta too, so it must reject the "
         "same values";
}

TEST_F(CongestionControllerFactoryTest, ALambdaThatCannotShiftAnIntIsRejected) {
  CongestionControllerFactory factory;

  EXPECT_EQ(factory.makeCongestionController("baseline:2,1,-1"), nullptr)
      << "lambda is a shift amount like beta, and shifting by a negative "
         "count is undefined behavior, so it must fail validation";
  EXPECT_EQ(factory.makeCongestionController("baseline:2,1,31"), nullptr)
      << "a shift as wide as an int is undefined behavior in the window "
         "reduction, so it must fail validation";
  EXPECT_NE(factory.makeCongestionController("baseline:2,1,30"), nullptr)
      << "the widest shift an int allows is still a valid lambda";
  EXPECT_EQ(
      factory.makeCongestionController("baseline_rcn:2,1,16,1000,-1"), nullptr)
      << "baseline_rcn reduces on loss by lambda too, so it must reject the "
         "same values";
}

TEST_F(CongestionControllerFactoryTest, NonIntegerGammaIsRejected) {
  CongestionControllerFactory factory;

  EXPECT_EQ(factory.makeCongestionController("baseline_rcn:2,1,1.1"), nullptr)
      << "gammaNumerator counts sixteenths, so a fractional value is a stale "
         "spelling of the old multiplier and must be reported, not truncated";
}

TEST_F(CongestionControllerFactoryTest, TooManyArgumentsAreRejected) {
  CongestionControllerFactory factory;

  EXPECT_EQ(
      factory.makeCongestionController("baseline_rcn:2,1,16,1000,0,7"), nullptr)
      << "an extra argument means the operator expected a parameter this "
         "algorithm does not have, so accepting it would silently ignore them";
}

} // namespace libsoftmetaroce

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

// Exercises the LD_PRELOAD interposer's C ABI contracts by linking the
// interposer library directly: this binary references ibv_* without
// linking libibverbs, so the loader binds every call to the interposer,
// the same binding LD_PRELOAD produces. Folly initializes through the
// interposer's own lazy path, as with a real C consumer, so this test
// deliberately does not use the folly gtest main.

#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

// Drop the header's port-query redirect so the legacy-shape case below
// references the raw compat symbol, as an ancient-header caller would. The
// modern-shape case calls the inline by its real name instead.
#undef ibv_query_port

// ibv_query_gid_type postdates the stablev60 headers this test builds
// against; declare the interposer symbol directly so the call below
// binds without linking libibverbs.
extern "C" int ibv_query_gid_type(
    ibv_context* context,
    uint8_t port_num,
    unsigned int index,
    ibv_gid_type* type);

namespace {

// Matches the backend's extension value for jumbo frames.
#ifndef IBV_MTU_8192
constexpr ibv_mtu IBV_MTU_8192 = static_cast<ibv_mtu>(6);
#endif

class PreloadAbiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    list_ = ibv_get_device_list(&numDevices_);
    if (list_ == nullptr || numDevices_ != 1) {
      GTEST_SKIP() << "no emulated device; the test environment has no "
                      "usable addresses";
    }
    ASSERT_STREQ(ibv_get_device_name(list_[0]), "lsmr0")
        << "precondition: the loader bound this binary to the interposer, "
           "which serves lsmr0; a real libibverbs serves no such device";
    ctx_ = ibv_open_device(list_[0]);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    if (ctx_ != nullptr) {
      EXPECT_EQ(ibv_close_device(ctx_), 0);
      ctx_ = nullptr;
    }
    if (list_ != nullptr) {
      ibv_free_device_list(list_);
      list_ = nullptr;
    }
  }

  ibv_device** list_ = nullptr;
  int numDevices_ = 0;
  ibv_context* ctx_ = nullptr;
};

TEST_F(PreloadAbiTest, DeviceNameIsBorrowed) {
  const char* first = ibv_get_device_name(list_[0]);
  const char* second = ibv_get_device_name(list_[0]);
  EXPECT_STREQ(first, "lsmr0")
      << "the interposer names its emulated device lsmr0, the name callers "
         "select; a real libibverbs serves no such device";
  EXPECT_EQ(first, second)
      << "callers must not free the name, so the interposer hands back one "
         "borrowed pointer instead of allocating per call";
  EXPECT_EQ(ibv_get_device_name(nullptr), nullptr)
      << "a null device has no name; the interposer must not dereference it";
}

TEST_F(PreloadAbiTest, LegacyPortQueryIsBounded) {
  struct {
    uint8_t attr[48];
    uint8_t canary[8];
  } legacy{};
  memset(&legacy, 0xa5, sizeof(legacy));
  auto* compat = reinterpret_cast<_compat_ibv_port_attr*>(&legacy.attr);
  EXPECT_EQ(ibv_query_port(ctx_, 1, compat), 0)
      << "the interposer serves ancient-header callers through the 48-byte "
         "legacy shape";
  ibv_port_state state = IBV_PORT_DOWN;
  memcpy(&state, &legacy.attr[offsetof(ibv_port_attr, state)], sizeof(state));
  EXPECT_EQ(state, IBV_PORT_ACTIVE)
      << "the emulated port must report active; callers refuse to send on a "
         "down port";
  uint8_t linkLayer = 0;
  memcpy(
      &linkLayer,
      &legacy.attr[offsetof(ibv_port_attr, link_layer)],
      sizeof(linkLayer));
  EXPECT_EQ(linkLayer, IBV_LINK_LAYER_ETHERNET)
      << "the port is Ethernet (RoCE v2); callers select paths by link layer";
  for (size_t i = 0; i < sizeof(legacy.canary); ++i) {
    EXPECT_EQ(legacy.canary[i], 0xa5)
        << "the compat fill must stop at the 48-byte legacy prefix; byte " << i
        << " past it was overwritten";
  }
  EXPECT_EQ(ibv_query_port(ctx_, 1, nullptr), EINVAL)
      << "a null caller buffer must fail instead of faulting";
}

TEST_F(PreloadAbiTest, ModernPortQuerySucceeds) {
  ibv_port_attr attr{};
  EXPECT_EQ(___ibv_query_port(ctx_, 1, &attr), 0)
      << "the real symbol behind the header inline must serve modern callers";
  EXPECT_EQ(attr.state, IBV_PORT_ACTIVE)
      << "the emulated port must report active; callers refuse to send on a "
         "down port";
  EXPECT_EQ(attr.max_mtu, IBV_MTU_8192)
      << "callers size their sends by max_mtu, which must expose the "
         "backend's 8K jumbo extension";
  EXPECT_EQ(attr.link_layer, IBV_LINK_LAYER_ETHERNET)
      << "the port is Ethernet (RoCE v2); callers select paths by link layer";
  EXPECT_GT(attr.gid_tbl_len, 0)
      << "an empty GID table leaves no addressable endpoint";
  EXPECT_EQ(attr.port_cap_flags2, 0)
      << "the compat fill covers the legacy prefix only; the tail keeps the "
         "inline's zeroes";
}

TEST_F(PreloadAbiTest, GidExDispatchesThroughPublicHeader) {
  ibv_gid_entry entry{};
  EXPECT_EQ(ibv_query_gid_ex(ctx_, 1, 0, &entry, 0), 0)
      << "the public inline must reach the interposed _ibv_query_gid_ex; "
         "success proves the loader binding";
  EXPECT_EQ(entry.gid_index, 0u)
      << "the entry must echo the requested index so callers can match it";
  // Cross-check against the legacy query: the same bytes.
  ibv_gid gid{};
  EXPECT_EQ(ibv_query_gid(ctx_, 1, 0, &gid), 0)
      << "precondition: the legacy query succeeds, or there is no baseline "
         "to cross-check";
  EXPECT_EQ(memcmp(entry.gid.raw, gid.raw, sizeof(gid.raw)), 0)
      << "the extended query must serve the same GID bytes as the legacy "
         "query";
}

TEST_F(PreloadAbiTest, GidExRejectsBadInput) {
  ibv_gid_entry entry{};
  EXPECT_EQ(ibv_query_gid_ex(ctx_, 1, 0, &entry, 1), EOPNOTSUPP)
      << "flag values the emulation does not implement must fail rather "
         "than be silently ignored";
  EXPECT_EQ(ibv_query_gid_ex(ctx_, 1, 999999, &entry, 0), EINVAL)
      << "an index past the table must fail instead of reading out of bounds";
  EXPECT_EQ(ibv_query_gid_ex(ctx_, 1, 0, nullptr, 0), EINVAL)
      << "a null entry buffer must fail instead of faulting";
  // Short entry sizes only arrive via direct callers; the header inline
  // always passes sizeof.
  EXPECT_EQ(_ibv_query_gid_ex(ctx_, 1, 0, &entry, 0, sizeof(entry) - 1), EINVAL)
      << "a short entry buffer must fail instead of truncating the write";
}

TEST_F(PreloadAbiTest, IovaRegistrationRequiresMatchingBase) {
  ibv_pd* pd = ibv_alloc_pd(ctx_);
  ASSERT_NE(pd, nullptr);
  uint8_t region[4096]{};
  uint64_t addr = reinterpret_cast<uint64_t>(region);
  // Ordinary: the identity mapping registers; a foreign base is refused.
  ibv_mr* mr =
      ibv_reg_mr_iova(pd, region, sizeof(region), addr, IBV_ACCESS_LOCAL_WRITE);
  ASSERT_NE(mr, nullptr)
      << "ordinary regions answer by virtual address, so the identity mapping "
         "must register";
  EXPECT_EQ(ibv_dereg_mr(mr), 0);
  EXPECT_EQ(
      ibv_reg_mr_iova(
          pd, region, sizeof(region), addr + 4096, IBV_ACCESS_LOCAL_WRITE),
      nullptr)
      << "a base matching neither addressing mode would address the wrong "
         "space, so registration must refuse it";
  EXPECT_EQ(errno, EINVAL)
      << "a refused mapping reports EINVAL so callers can tell bad parameters "
         "from other failures";
  // Zero-based: base zero registers; a nonzero base is refused.
  mr = ibv_reg_mr_iova(
      pd,
      region,
      sizeof(region),
      0,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_ZERO_BASED);
  ASSERT_NE(mr, nullptr)
      << "zero-based regions answer from their own base, so base zero must "
         "register";
  EXPECT_EQ(ibv_dereg_mr(mr), 0);
  EXPECT_EQ(
      ibv_reg_mr_iova(
          pd,
          region,
          sizeof(region),
          addr,
          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_ZERO_BASED),
      nullptr)
      << "a nonzero base on a zero-based region would address the wrong "
         "space, so registration must refuse it";
  EXPECT_EQ(errno, EINVAL)
      << "a refused mapping reports EINVAL so callers can tell bad parameters "
         "from other failures";
  // The unsigned-access entry point enforces the same rule.
  EXPECT_EQ(
      ibv_reg_mr_iova2(
          pd, region, sizeof(region), addr + 4096, IBV_ACCESS_LOCAL_WRITE),
      nullptr)
      << "the unsigned-access entry point must enforce the same mapping rule";
  EXPECT_EQ(errno, EINVAL)
      << "a refused mapping reports EINVAL so callers can tell bad parameters "
         "from other failures";
  EXPECT_EQ(ibv_dealloc_pd(pd), 0);
}

TEST_F(PreloadAbiTest, CompChannelCreationFailsFast) {
  EXPECT_EQ(ibv_create_comp_channel(ctx_), nullptr)
      << "event-driven completion is unsupported; creation must fail loudly "
         "rather than hand out a channel no event would arrive on";
  EXPECT_EQ(errno, ENOSYS)
      << "unsupported verbs report ENOSYS so callers can tell unimplemented "
         "from failed";
  ibv_cq* cq = ibv_create_cq(ctx_, 16, nullptr, nullptr, 0);
  ASSERT_NE(cq, nullptr);
  EXPECT_EQ(ibv_req_notify_cq(cq, 0), ENOSYS)
      << "arming an event no channel could deliver must fail rather than "
         "claim success and hang the waiter";
  EXPECT_EQ(ibv_destroy_cq(cq), 0);
}

TEST_F(PreloadAbiTest, GidTypeRejectsHugeIndex) {
  ibv_gid_type type = IBV_GID_TYPE_IB;
  EXPECT_EQ(ibv_query_gid_type(ctx_, 1, UINT_MAX, &type), -1)
      << "an index that narrows below zero must fail before the cast";
  EXPECT_EQ(errno, EINVAL)
      << "a rejected query reports EINVAL so callers can tell bad parameters "
         "from other failures";
  EXPECT_EQ(type, IBV_GID_TYPE_IB)
      << "a rejected query must not write the output";
  ibv_gid_type backendType = IBV_GID_TYPE_IB;
  EXPECT_EQ(ibv_query_gid_type(ctx_, 1, INT_MAX, &backendType), -1)
      << "an in-range but absent index fails in the backend, which must "
         "keep the -1 shape rather than leak a positive errno";
  EXPECT_EQ(errno, EINVAL)
      << "the backend's reason survives the shape enforcement";
  EXPECT_EQ(backendType, IBV_GID_TYPE_IB)
      << "a rejected query must not write the output";
}

// Re-encode an IP string to ibv_gid using the same scheme as
// MetaRoceVerbs::ibv_query_gid (IPv6 -> raw, IPv4 -> v4-in-v6 mapped).
ibv_gid encodeIpToIbvGid(const std::string& ip) {
  ibv_gid gid{};
  if (inet_pton(AF_INET6, ip.c_str(), gid.raw) == 1) {
    return gid;
  }
  struct in_addr v4{};
  if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
    gid.raw[10] = 0xff;
    gid.raw[11] = 0xff;
    memcpy(&gid.raw[12], &v4, sizeof(v4));
  }
  return gid;
}

// Inverse: render a table GID back to the address string the backend
// matched during discovery (dotted quad for v4-mapped, canonical v6).
std::string gidToIpString(const ibv_gid& gid) {
  static const uint8_t kV4Prefix[12] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  char buf[INET6_ADDRSTRLEN];
  if (memcmp(gid.raw, kV4Prefix, sizeof(kV4Prefix)) == 0) {
    inet_ntop(AF_INET, gid.raw + 12, buf, sizeof(buf));
  } else {
    inet_ntop(AF_INET6, gid.raw, buf, sizeof(buf));
  }
  return buf;
}

// Runs in a re-executed copy of this binary with LSMR_PRELOAD_GIDS set
// (see EnvGidsHonoredInFreshProcess): a fresh process exercises the real
// interposer init path, constructor and lazy init alike. Skips without
// the variable, so the in-suite and CTest-discovery runs stay green; the
// parent only accepts a run whose output shows this test passed.
TEST(PreloadEnvChild, EnvGidsSelectFirstGid) {
  const char* gids = std::getenv("LSMR_PRELOAD_GIDS");
  if (gids == nullptr) {
    GTEST_SKIP() << "runs only in the re-executed child, which the parent "
                    "test starts with LSMR_PRELOAD_GIDS set";
  }
  int numDevices = 0;
  ibv_device** list = ibv_get_device_list(&numDevices);
  ASSERT_NE(list, nullptr)
      << "the requested address must exist locally; the parent only "
         "requests addresses from its own live table";
  ASSERT_EQ(numDevices, 1)
      << "precondition: exactly one emulated device serves the query";
  ibv_context* ctx = ibv_open_device(list[0]);
  ASSERT_NE(ctx, nullptr);
  ibv_gid gid{};
  ASSERT_EQ(ibv_query_gid(ctx, 1, 0, &gid), 0)
      << "precondition: the selection check below compares gid index 0 "
         "against the requested address";
  EXPECT_EQ(memcmp(gid.raw, encodeIpToIbvGid(gids).raw, sizeof(gid.raw)), 0)
      << "gid index 0 must be the requested address (" << gids << ")";
  EXPECT_EQ(ibv_close_device(ctx), 0);
  ibv_free_device_list(list);
}

TEST_F(PreloadAbiTest, EnvGidsHonoredInFreshProcess) {
  // The fixture guarantees a live device, so the sandbox has usable
  // addresses. Request the last table entry: if the environment were
  // dropped, the child would fall back to discovery order and expose a
  // different first GID.
  ibv_port_attr attr{};
  ASSERT_EQ(___ibv_query_port(ctx_, 1, &attr), 0)
      << "precondition: the last-entry index below is computed from the "
         "port attributes";
  ASSERT_GT(attr.gid_tbl_len, 0)
      << "precondition: requesting the last entry needs a non-empty table";
  ibv_gid last{};
  ASSERT_EQ(ibv_query_gid(ctx_, 1, attr.gid_tbl_len - 1, &last), 0)
      << "precondition: the child request string is rendered from this "
         "entry";
  const std::string requested = gidToIpString(last);

  // Set the environment before forking so the child runs nothing but
  // dup2/exec; the parent's backend is already constructed, so the flag
  // change cannot affect it. Capture the child output to confirm the
  // test passed rather than skipped: both exit 0.
  char outTemplate[] = "/tmp/preload_env_child.XXXXXX";
  const int outFd = mkstemp(outTemplate);
  ASSERT_NE(outFd, -1) << "precondition: the parent reads the child "
                          "result from a temp capture file: "
                       << strerror(errno);
  ASSERT_EQ(setenv("LSMR_PRELOAD_GIDS", requested.c_str(), 1), 0)
      << "precondition: the child inherits the requested address from the "
         "environment";
  // FAIL() returns from the test when fork fails.
  // NOLINTNEXTLINE(facebook-hte-UncheckedFork)
  pid_t pid = fork();
  if (pid == -1) {
    FAIL() << "fork failed: " << strerror(errno);
  }
  if (pid == 0) {
    dup2(outFd, STDOUT_FILENO);
    dup2(outFd, STDERR_FILENO);
    execl(
        "/proc/self/exe",
        "PreloadAbiTest",
        "--gtest_filter=PreloadEnvChild.EnvGidsSelectFirstGid",
        (char*)nullptr);
    _exit(127); // execl failed; do not run parent teardown in the child.
  }
  unsetenv("LSMR_PRELOAD_GIDS");
  close(outFd);
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid)
      << "precondition: the verdict comes from the reaped child status";
  std::ifstream out(outTemplate);
  std::ostringstream captured;
  captured << out.rdbuf();
  unlink(outTemplate);
  EXPECT_TRUE(WIFEXITED(status)) << "child status=" << status << "\n"
                                 << captured.str();
  EXPECT_EQ(WEXITSTATUS(status), 0) << "LSMR_PRELOAD_GIDS=" << requested << "\n"
                                    << captured.str();
  EXPECT_NE(
      captured.str().find("OK ] PreloadEnvChild.EnvGidsSelectFirstGid"),
      std::string::npos)
      << "the child must run the test to pass, not skip it\n"
      << captured.str();
}
} // namespace

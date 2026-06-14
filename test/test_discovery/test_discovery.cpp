// Host unit tests for DiscoveryController. Run: pio test -e native
#include <unity.h>

#include <string>
#include <vector>

#include "DiscoveryController.h"
#include "ServerCandidate.h"

namespace {
ServerCandidate mk(std::string id, std::string name, std::string host, uint16_t port) {
  ServerCandidate s;
  s.instanceId = std::move(id);
  s.instanceName = std::move(name);
  s.host = host;
  s.port = port;
  s.scheme = "http";
  s.baseUrl = "http://" + host + ":" + std::to_string(port);
  return s;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_new_sighting_is_listed() {
  DiscoveryController c;
  c.update(0, {mk("a", "Alpha", "192.168.1.10", 3000)});
  TEST_ASSERT_EQUAL_UINT(1, c.count());
  TEST_ASSERT_EQUAL_STRING("Alpha", c.servers()[0].instanceName.c_str());
}

void test_same_id_dedupes_and_updates_in_place() {
  DiscoveryController c;
  c.update(0, {mk("a", "Alpha", "192.168.1.10", 3000)});
  // Same id, new IP (DHCP change) — must update the existing entry, not add one.
  c.update(100, {mk("a", "Alpha", "192.168.1.55", 3000)});
  TEST_ASSERT_EQUAL_UINT(1, c.count());
  TEST_ASSERT_EQUAL_STRING("192.168.1.55", c.servers()[0].host.c_str());
}

void test_two_distinct_servers_listed() {
  DiscoveryController c;
  c.update(0, {mk("a", "Alpha", "192.168.1.10", 3000),
               mk("b", "Bravo", "192.168.1.11", 3000)});
  TEST_ASSERT_EQUAL_UINT(2, c.count());
}

void test_stale_server_expires() {
  DiscoveryController c(1000);  // 1s TTL
  c.update(0, {mk("a", "Alpha", "192.168.1.10", 3000)});
  c.update(500, {mk("a", "Alpha", "192.168.1.10", 3000)});  // re-seen, stays alive
  c.update(1200, {});  // 1200-500 <= 1000 → still alive
  TEST_ASSERT_EQUAL_UINT(1, c.count());
  c.update(2300, {});  // 2300-500 > 1000 → expired
  TEST_ASSERT_EQUAL_UINT(0, c.count());
}

void test_selection_clamps_and_returns_server() {
  DiscoveryController c;
  c.update(0, {mk("a", "Alpha", "h1", 3000), mk("b", "Bravo", "h2", 3000)});
  TEST_ASSERT_EQUAL_INT(0, c.selectedIndex());
  c.moveSelection(-5);  // clamp low
  TEST_ASSERT_EQUAL_INT(0, c.selectedIndex());
  c.moveSelection(5);   // clamp high
  TEST_ASSERT_EQUAL_INT(1, c.selectedIndex());
  TEST_ASSERT_EQUAL_STRING("b", c.selected()->instanceId.c_str());
}

void test_selection_follows_server_across_reorder() {
  DiscoveryController c(1000);
  c.update(0, {mk("a", "Alpha", "h1", 3000),
               mk("b", "Bravo", "h2", 3000),
               mk("c", "Charlie", "h3", 3000)});
  c.moveSelection(1);  // select Bravo
  TEST_ASSERT_EQUAL_STRING("b", c.selected()->instanceId.c_str());
  // Alpha disappears; Bravo + Charlie re-seen. Cursor should stay on Bravo.
  c.update(2000, {mk("b", "Bravo", "h2", 3000), mk("c", "Charlie", "h3", 3000)});
  TEST_ASSERT_EQUAL_UINT(2, c.count());
  TEST_ASSERT_EQUAL_STRING("b", c.selected()->instanceId.c_str());
}

void test_dedupes_by_host_port_when_no_id() {
  DiscoveryController c;
  c.update(0, {mk("", "NoId", "192.168.1.10", 3000),
               mk("", "NoId", "192.168.1.10", 3000)});
  TEST_ASSERT_EQUAL_UINT(1, c.count());
  c.update(100, {mk("", "NoId", "192.168.1.10", 3001)});  // different port → distinct
  TEST_ASSERT_EQUAL_UINT(2, c.count());
}

void test_empty_list_has_no_selection() {
  DiscoveryController c;
  TEST_ASSERT_EQUAL_INT(-1, c.selectedIndex());
  TEST_ASSERT_NULL(c.selected());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_new_sighting_is_listed);
  RUN_TEST(test_same_id_dedupes_and_updates_in_place);
  RUN_TEST(test_two_distinct_servers_listed);
  RUN_TEST(test_stale_server_expires);
  RUN_TEST(test_selection_clamps_and_returns_server);
  RUN_TEST(test_selection_follows_server_across_reorder);
  RUN_TEST(test_dedupes_by_host_port_when_no_id);
  RUN_TEST(test_empty_list_has_no_selection);
  return UNITY_END();
}

// Host unit tests for DockController. Run: pio test -e native
#include <unity.h>

#include <queue>

#include "DockController.h"
#include "hal/IDock.h"

namespace {
class FakeDock : public IDock {
public:
  std::queue<DockEvent> events;
  int exposeCount = 0;
  int reclaimCount = 0;
  bool docked = false;

  void begin() override {}
  DockEvent poll(uint32_t) override {
    if (events.empty()) return DockEvent::None;
    DockEvent e = events.front();
    events.pop();
    return e;
  }
  bool isDocked() const override { return docked; }
  void exposeMassStorage() override { ++exposeCount; docked = true; }
  void reclaimMassStorage() override { ++reclaimCount; docked = false; }
};
}  // namespace

void setUp() {}
void tearDown() {}

void test_insert_docks_and_exposes_storage() {
  FakeDock d;
  DockController c(d);
  int dockCb = 0, undockCb = 0;
  c.onDock = [&]() { ++dockCb; };
  c.onUndock = [&]() { ++undockCb; };

  d.events.push(DockEvent::Inserted);
  c.update(0);

  TEST_ASSERT_EQUAL_INT(1, dockCb);
  TEST_ASSERT_EQUAL_INT(1, d.exposeCount);
  TEST_ASSERT_EQUAL_INT(0, undockCb);
  TEST_ASSERT_EQUAL_INT((int)DockState::Docked, (int)c.state());
}

void test_remove_undocks_and_reclaims_storage() {
  FakeDock d;
  DockController c(d);
  int dockCb = 0, undockCb = 0;
  c.onDock = [&]() { ++dockCb; };
  c.onUndock = [&]() { ++undockCb; };

  d.events.push(DockEvent::Inserted);
  c.update(0);
  d.events.push(DockEvent::Removed);
  c.update(1);

  TEST_ASSERT_EQUAL_INT(1, d.reclaimCount);
  TEST_ASSERT_EQUAL_INT(1, undockCb);
  TEST_ASSERT_EQUAL_INT((int)DockState::Undocked, (int)c.state());
}

void test_duplicate_insert_is_ignored() {
  FakeDock d;
  DockController c(d);
  int dockCb = 0;
  c.onDock = [&]() { ++dockCb; };

  d.events.push(DockEvent::Inserted);
  c.update(0);
  d.events.push(DockEvent::Inserted);  // already docked — must be a no-op
  c.update(1);

  TEST_ASSERT_EQUAL_INT(1, dockCb);
  TEST_ASSERT_EQUAL_INT(1, d.exposeCount);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_insert_docks_and_exposes_storage);
  RUN_TEST(test_remove_undocks_and_reclaims_storage);
  RUN_TEST(test_duplicate_insert_is_ignored);
  return UNITY_END();
}
